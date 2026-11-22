#include "../global.h"

// Shared pass of the image nodes (Pattern and Filter menus).
// An image node evaluates its inputs in image space: the graph above the node drawn over the node's own square
// ([0, 1] of tex_coord, Size x Size pixels, no mesh, no uv layout, parser_material_image_space), read back and
// processed on the cpu by the node's type (image_node_type_t, make_image_node_register). The result is uploaded into
// g_context->node_previews under the node's preview id and sampled by the node like an Image Texture at the
// current texture coordinate, repeating (make_image_node_sample; Tile UV places an image on the uv with
// make_image_node_sample_at; bound as _teximage_<preview id>). Baked image nodes have no Vector input (Tile UV places on it, and the
// pass-through generators have one: make_image_node_coord).
// Nothing is saved with the project.
// Every node preview traversal captures the node; a changed capture or parameter set makes it pending, and 0.5 s
// after the last change, with no mouse button held and no Bake Texture running, the pending nodes are baked:
// chained image nodes in dependency order, the inner node first.

#define MAKE_IMAGE_NODE_DEBOUNCE 0.5
static bool make_image_node_trace = false; // ARMORPAINT_IMAGE_NODE_TRACE in the environment: every capture is logged with its time
static i32 make_image_node_captures = 0; // Captures made since the start, see make_image_node_capture_count
static i32 make_image_node_capture_dst = 0; // Width of the target of the running capture, see make_image_node_box_taps
#define MAKE_IMAGE_NODE_BIG_PLANE ((size_t)64 << 20) // A 4096 x 4096 float plane is exactly this

#if !defined(IRON_WINDOWS)
#include <sys/mman.h>
#endif

// Zeroed memory for the planes of an image node (captured pixels, results, scratch planes of the cpu passes). Planes of 64 MB and
// up (4096 x 4096 floats and more) are mapped straight from the OS, so make_image_node_plane_free returns them to it at once: malloc
// keeps blocks that big cached and resident (measured at 4096: about 1.3 GB stayed after the nodes went back to 1024).
// Smaller planes use calloc. Returns NULL when memory runs out. Free with make_image_node_plane_free and the same size.
void *make_image_node_plane_alloc(size_t bytes) {
#if !defined(IRON_WINDOWS)
	if (bytes >= MAKE_IMAGE_NODE_BIG_PLANE) {
		void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
		return p == MAP_FAILED ? NULL : p;
	}
#endif
	return calloc(1, bytes > 0 ? bytes : 1);
}

void make_image_node_plane_free(void *p, size_t bytes) {
	if (p == NULL) {
		return;
	}
#if !defined(IRON_WINDOWS)
	if (bytes >= MAKE_IMAGE_NODE_BIG_PLANE) {
		munmap(p, bytes);
		return;
	}
#endif
	free(p);
}

extern ui_node_t    *ui_nodes_node_changed;
extern gpu_buffer_t *util_render_screen_aligned_full_vb;
extern gpu_buffer_t *util_render_screen_aligned_full_ib;

typedef struct make_image_node_state {
	image_node_type_t *type;
	i32                material_id;
	bool               has_baked;
	u64                baked_signature;
	bool               failed;           // The last bake of failed_signature failed (upload or out of memory), not retried for it
	u64                failed_signature;
	bool               pending;      // pending_pixels wait for the debounce
	u64                pending_signature;
	u8                *pending_pixels; // The captures one after the other (image_node_type_t.capture_format)
	size_t             pending_bytes;  // Size of pending_pixels, for make_image_node_plane_free
	i32                pending_size;
	void              *pending_params;
	bool               needs_capture; // Graph changed while busy, capture when the debounce fires
	string_array_t    *upstream;      // Preview ids of the image nodes the last capture sampled
	bool               captured;      // The versions below are those of the last capture, see make_image_node_unchanged
	ui_node_canvas_t  *captured_canvas;
	u32                captured_graph_version;
	u32                captured_asset_version;
	bool               cycle_seen;    // The running capture sampled a node that depends on this node, see make_image_node_edge_cyclic
	bool               cycle_told;    // The Console told about the loop this node is in
	gpu_texture_t     *pending_target; // Result by reference: the rendered result waits for the debounce (it replaces the result at the bake)
	i32                gpu_renders;    // Result by reference: renders of this node since the start (a test hook, make_image_node_gpu_stat)
	f64                gpu_render_ms;  // Result by reference: time of the last capture that rendered (shader, hash, pass), see make_image_node_gpu_stat
	f64                gpu_pass_ms;    // Result by reference: time of the pass of that render alone
} make_image_node_state_t;

// An image node of a material, found by make_image_node_collect
typedef struct make_image_node_entry {
	ui_node_t               *node;
	ui_node_canvas_t        *group;
	ui_node_t_array_t       *parents;
	char                    *id;
	make_image_node_state_t *state;
	bool                     done;
} make_image_node_entry_t;

static any_array_t             *make_image_node_types           = NULL;
static any_map_t               *make_image_node_states          = NULL; // Preview id to state
static f64                      make_image_node_last_change     = 0.0;
static bool                     make_image_node_scheduled       = false;
static bool                     make_image_node_force_all       = false; // Regenerate image nodes on the next update
static make_image_node_state_t *make_image_node_capturing       = NULL;  // Collects the image nodes sampled by the capture
static ui_node_canvas_t        *make_image_node_capture_group   = NULL;
static ui_node_t_array_t       *make_image_node_capture_parents = NULL;
static i32                      make_image_node_capture_k       = 0; // Capture of a type with several, see make_image_node_capture_index
static void                    *make_image_node_capture_params_ptr = NULL; // See make_image_node_capture_params
static i32                      make_image_node_capture_input   = 0; // Input of make_image_node_input_value

static void make_image_node_update(void *_);
static void make_image_node_state_row(i32 node_id);

void make_image_node_register(image_node_type_t *type) {
	if (make_image_node_types == NULL) {
		make_image_node_types = any_array_create_from_raw((void *[]){}, 0);
		make_image_node_trace = getenv("ARMORPAINT_IMAGE_NODE_TRACE") != NULL;
		any_map_set(ui_nodes_custom_buttons, "make_image_node_state_row", make_image_node_state_row);
	}
	any_array_push(make_image_node_types, type);
}

// The image node type of a node type, NULL for every other node
image_node_type_t *make_image_node_type_of(char *node_type) {
	if (make_image_node_types == NULL) {
		return NULL;
	}
	for (i32 i = 0; i < make_image_node_types->length; ++i) {
		image_node_type_t *t = make_image_node_types->buffer[i];
		if (string_equals(t->type, node_type)) {
			return t;
		}
	}
	return NULL;
}

static void make_image_node_redraw_nodes() {
	if (ui_nodes_hwnd != NULL) {
		ui_nodes_hwnd->redraws = 2;
	}
}

static string_array_t *make_image_node_states_keys() {
	if (make_image_node_states == NULL) {
		make_image_node_states = any_map_create();
	}
	return map_keys(make_image_node_states);
}

static void make_image_node_free_keys(string_array_t *keys) {
	array_free(keys);
	free(keys);
}

// The states in the slots of the map, NULL for an empty slot, without building a key list. Do not add or delete states while walking.
static i32 make_image_node_state_slots() {
	return make_image_node_states != NULL ? make_image_node_states->keys->capacity : 0;
}

static make_image_node_state_t *make_image_node_state_at(i32 slot) {
	return make_image_node_states->keys->buffer[slot] != NULL ? make_image_node_states->values->buffer[slot] : NULL;
}

static make_image_node_state_t *make_image_node_get_state(char *id, ui_node_t *node) {
	if (make_image_node_states == NULL) {
		make_image_node_states = any_map_create();
	}
	make_image_node_state_t *s = any_map_get(make_image_node_states, id);
	if (s == NULL) {
		s = calloc(1, sizeof(make_image_node_state_t));
		any_map_set(make_image_node_states, string_copy(id), s);
	}
	s->type        = make_image_node_type_of(node->type);
	s->material_id = g_context->material->id;
	return s;
}

static void make_image_node_clear_upstream(make_image_node_state_t *s) {
	if (s->upstream == NULL) {
		s->upstream = any_array_create_from_raw((void *[]){}, 0);
	}
	for (i32 i = 0; i < s->upstream->length; ++i) {
		free(s->upstream->buffer[i]);
	}
	s->upstream->length = 0;
}

static void make_image_node_clear_pending(make_image_node_state_t *s) {
	make_image_node_plane_free(s->pending_pixels, s->pending_bytes);
	free(s->pending_params);
	if (s->pending_target != NULL) {
		gpu_delete_texture(s->pending_target);
		s->pending_target = NULL;
	}
	s->pending_pixels = NULL;
	s->pending_bytes  = 0;
	s->pending_params = NULL;
	s->pending        = false;
}

static void make_image_node_free_state(make_image_node_state_t *s) {
	make_image_node_clear_pending(s);
	if (s->upstream != NULL) {
		make_image_node_clear_upstream(s);
		array_free(s->upstream);
		free(s->upstream);
	}
	free(s);
}

static void make_image_node_schedule() {
	make_image_node_last_change = sys_time();
	make_image_node_redraw_nodes(); // Baking state of the node
	if (!make_image_node_scheduled) {
		make_image_node_scheduled = true;
		sys_notify_on_update(make_image_node_update, NULL);
	}
}

// Mouse button held (slider drag) or Bake Texture running: no capture and no bake now.
// pdirty is no signal: a fill layer leaves it at 1.
static bool make_image_node_busy() {
	return bake_texture_node_baking || g_ui->input_down;
}

// xxh64 begin
// xxHash64 (Yann Collet, BSD 2 clause), single pass, public algorithm: 32 byte stripes in four lanes, then a final avalanche
static inline u64 make_image_node_rotl64(u64 x, int r) {
	return (x << r) | (x >> (64 - r));
}

static inline u64 make_image_node_read64(const u8 *p) {
	u64 v;
	memcpy(&v, p, 8);
	return v; // Little endian hosts only (every supported target)
}

static inline u64 make_image_node_xxh_round(u64 acc, u64 input) {
	acc += input * 14029467366897019727ull;
	acc = make_image_node_rotl64(acc, 31);
	return acc * 11400714785074694791ull;
}

static inline u64 make_image_node_xxh_merge(u64 acc, u64 val) {
	acc ^= make_image_node_xxh_round(0, val);
	return acc * 11400714785074694791ull + 9650029242287828579ull;
}

// 64 bit xxHash64 of n bytes with a seed, the seed chains calls (the output of one call is the seed of the next)
static u64 make_image_node_hash(u64 seed, const void *data, size_t n) {
	const u8 *p   = data;
	const u8 *end = p + n;
	u64       h;
	if (n >= 32) {
		u64       v1    = seed + 11400714785074694791ull + 14029467366897019727ull;
		u64       v2    = seed + 14029467366897019727ull;
		u64       v3    = seed;
		u64       v4    = seed - 11400714785074694791ull;
		const u8 *limit = end - 32;
		do {
			v1 = make_image_node_xxh_round(v1, make_image_node_read64(p));
			v2 = make_image_node_xxh_round(v2, make_image_node_read64(p + 8));
			v3 = make_image_node_xxh_round(v3, make_image_node_read64(p + 16));
			v4 = make_image_node_xxh_round(v4, make_image_node_read64(p + 24));
			p += 32;
		} while (p <= limit);
		h = make_image_node_rotl64(v1, 1) + make_image_node_rotl64(v2, 7) + make_image_node_rotl64(v3, 12) + make_image_node_rotl64(v4, 18);
		h = make_image_node_xxh_merge(h, v1);
		h = make_image_node_xxh_merge(h, v2);
		h = make_image_node_xxh_merge(h, v3);
		h = make_image_node_xxh_merge(h, v4);
	}
	else {
		h = seed + 2870177450012600261ull;
	}
	h += (u64)n;
	while (p + 8 <= end) {
		h ^= make_image_node_xxh_round(0, make_image_node_read64(p));
		h = make_image_node_rotl64(h, 27) * 11400714785074694791ull + 9650029242287828579ull;
		p += 8;
	}
	if (p + 4 <= end) {
		u32 w;
		memcpy(&w, p, 4);
		h ^= (u64)w * 11400714785074694791ull;
		h = make_image_node_rotl64(h, 23) * 14029467366897019727ull + 1609587929392839161ull;
		p += 4;
	}
	while (p < end) {
		h ^= (u64)(*p++) * 2870177450012600261ull;
		h = make_image_node_rotl64(h, 11) * 11400714785074694791ull;
	}
	h ^= h >> 33;
	h *= 14029467366897019727ull;
	h ^= h >> 29;
	h *= 1609587929392839161ull;
	h ^= h >> 32;
	return h;
}
// xxh64 end

// 64 bit signature over the captured pixels, the size and the processing parameters: xxHash64 chained over the three
static u64 make_image_node_signature(u8 *pixels, size_t bytes, i32 size, void *params, i32 params_size) {
	u64 h = make_image_node_hash(0, pixels, bytes);
	h     = make_image_node_hash(h, &size, sizeof(size));
	return make_image_node_hash(h, params, params_size);
}

// Points the material parser at the canvas (and group chain) of the node being captured, so that the links of its inputs can be read
static void make_image_node_parser_scope() {
	parser_material_canvases = any_array_create_from_raw(
	    (void *[]){
	        g_context->material->canvas,
	    },
	    1);
	parser_material_nodes = g_context->material->canvas->nodes;
	parser_material_links = g_context->material->canvas->links;
	if (make_image_node_capture_group != NULL) {
		parser_material_push_group_chain(make_image_node_capture_group, make_image_node_capture_parents);
	}
}

// ---- GPU passes ----
// Every GPU render of an image node is a full screen pass: one triangle over the target, tex_coord = the image uv ([0, 1] across the
// target, uv (0, 0) at the top left like the paint uvs), no mesh. The capture of an input graph is one (make_image_node_capture_shader draws the
// expression of the node); the GPU baked nodes (the section GPU baked nodes of IMAGE_NODES.md) draw their own passes with
// make_image_node_gpu_shader and make_image_node_gpu_pass, chained over a ping-pong pair when a pass reads the one before.

// A pass shader being written: the kong shader and the contexts that end it
typedef struct make_image_node_pass {
	node_shader_context_t *con;
	node_shader_t         *kong;
	material_t            *data;
} make_image_node_pass_t;

// Starts a full screen pass shader writing count targets of the named formats (render_path_get_tex_format names: RGBA128, RGBA64,
// R32, R16, R8, RGBA32). The vertex stage and tex_coord are written, the caller writes the fragment stage (node_shader_write_frag, frag_out)
static make_image_node_pass_t make_image_node_pass_begin(char **formats, i32 count) {
	any_array_t *attachments = any_array_create_from_raw((void *[]){}, 0);
	for (i32 i = 0; i < count; ++i) {
		any_array_push(attachments, formats[i]);
	}
	shader_context_t      *props = ALLOC_INIT(shader_context_t, {.name            = "mesh",
	                                                             .depth_write     = false,
	                                                             .compare_mode    = "always",
	                                                             .cull_mode       = "none",
	                                                             .vertex_elements = any_array_create_from_raw(
	                                                                 (void *[]){
	                                                                     ALLOC_INIT(vertex_element_t, {.name = "pos", .data = "short4norm"}),
	                                                                     ALLOC_INIT(vertex_element_t, {.name = "nor", .data = "short2norm"}),
	                                                                     ALLOC_INIT(vertex_element_t, {.name = "tex", .data = "short2norm"}),
	                                                                     ALLOC_INIT(vertex_element_t, {.name = "col", .data = "short4norm"}),
	                                                                 },
	                                                                 4),
	                                                             .color_attachments = attachments});
	make_image_node_pass_t p     = {0};
	p.data                       = ALLOC_INIT(material_t, {.name = "Material", .canvas = g_context->material->canvas});
	p.con                        = node_shader_context_create(p.data, props);
	p.kong                       = node_shader_context_make_kong(p.con);
	// util_render_screen_aligned_full_vb: one triangle covering the target
	node_shader_write_attrib_vert(p.kong, "output.pos = float4(input.pos.xy * 3.0, 0.0, 1.0);");
	node_shader_add_out(p.kong, "float2 tex_coord");
	node_shader_write_attrib_vert(p.kong, "output.tex_coord = float2(output.pos.x * 0.5 + 0.5, 0.5 - output.pos.y * 0.5);");
	node_shader_write_attrib_frag(p.kong, "float2 tex_coord = input.tex_coord;");
	return p;
}

// Ends a pass shader: compiles it into a shader context. source_hash (may be NULL) gets the xxHash64 of the shader text.

static shader_context_t *make_image_node_pass_end(make_image_node_pass_t *p, u64 *source_hash) {
	char *source = node_shader_get(p->kong);
	if (source_hash != NULL) {
		*source_hash = make_image_node_hash(0, source, strlen(source));
	}
	p->con->data->shader_from_source = true;
	gpu_create_shaders_from_kong(source, &p->con->data->vertex_shader, &p->con->data->fragment_shader, &p->con->data->_->vertex_shader_size,
	                             &p->con->data->_->fragment_shader_size);
	shader_context_load(p->con->data);
	shader_context_t *scon = p->con->data;
	node_shader_context_free(p->con);
	free(p->data);
	return scon;
}

// Shader evaluating expr(node) over the image square: a full screen pass (see above), mesh based inputs get a neutral value
// (parser_material_image_space). The node writes the expression while make_image_node_capturing is set. source_hash: see
// make_image_node_pass_end (NULL when not needed).
static shader_context_t *make_image_node_capture_shader(ui_node_t *node, char *(*expr)(ui_node_t *), char *format, u64 *source_hash) {
	make_image_node_pass_t p = make_image_node_pass_begin(&format, 1);
	parser_material_init();
	make_image_node_parser_scope();
	parser_material_con                   = p.con;
	parser_material_kong                  = p.kong;
	parser_material_image_space           = true;
	parser_material_transform_color_space = false;
	char *res                             = expr(node);
	parser_material_transform_color_space = true;
	parser_material_image_space           = false;

	p.kong->frag_out = "float4";
	node_shader_write_frag(p.kong, string_tmp("output = %s;", res));
	parser_material_finalize(p.con);
	return make_image_node_pass_end(&p, source_hash);
}

// A hand written fragment pass of a GPU baked node (an Advanced Blur stage, the Edge Detect Sobel, a reduction step). body is kong fragment
// code: it reads tex_coord (the image uv of the target pixel) and writes output (one target) or output[0], output[1], ... (target_count > 1);
// all float4, a single channel target keeps R. inputs names the textures it reads (sample_lod(name, sampler_linear, uv, 0.0)), uniforms the
// constants it reads, each "float4 name" (types float, float2, float3, float4; in the code constants.name). formats are the target formats
// (render_path_get_tex_format names), one per target. make_image_node_gpu_pass binds the inputs and the floats of the uniforms in the order
// of these lists. Free with make_image_node_gpu_shader_free.
shader_context_t *make_image_node_gpu_shader(char *body, char **inputs, i32 input_count, char **uniforms, i32 uniform_count, char **formats, i32 target_count) {
	make_image_node_pass_t p = make_image_node_pass_begin(formats, target_count);
	for (i32 i = 0; i < input_count; ++i) {
		node_shader_add_texture(p.kong, inputs[i], NULL);
	}
	for (i32 i = 0; i < uniform_count; ++i) {
		node_shader_add_constant(p.kong, uniforms[i], NULL);
	}
	p.kong->frag_out = target_count > 1 ? string_tmp("float4[%d]", target_count) : "float4";
	node_shader_write_frag(p.kong, body);
	return make_image_node_pass_end(&p, NULL);
}

void make_image_node_gpu_shader_free(shader_context_t *shader) {
	make_material_delete_context(shader); // The pipeline may still be in use by this frame: deleted on the next one
}

// Number of floats of a constant type of make_image_node_gpu_shader (0 for a type it does not set)
static i32 make_image_node_uniform_floats(char *type) {
	if (string_equals(type, "float")) {
		return 1;
	}
	if (string_equals(type, "float2")) {
		return 2;
	}
	if (string_equals(type, "float3")) {
		return 3;
	}
	return string_equals(type, "float4") ? 4 : 0;
}

// Draws one full screen pass of shader into targets (target_count render targets written at once: MRT, all of one size), reading the
// textures inputs (bound in the order the shader lists them in make_image_node_gpu_shader; NULL and 0 for the shader of an input graph)
// and the floats of uniforms (consumed by the constants of the shader that carry no link, in their order; NULL for none). The targets
// are cleared to 0 first. Nothing is read back: the targets stay on the GPU and the next pass reads them. The shader is NOT freed, a node runs it for
// every pass of its algorithm (make_image_node_gpu_shader_free). Call it where a capture runs, with no other pass active.
void make_image_node_gpu_pass(shader_context_t *shader, gpu_texture_t **targets, i32 target_count, gpu_texture_t **inputs, i32 input_count, f32 *uniforms) {
	if (util_render_screen_aligned_full_vb == NULL) {
		util_render_create_screen_aligned_full_data();
	}
	static string_array_t *empty = NULL;
	if (empty == NULL) {
		empty = any_array_create_from_raw(
		    (void *[]){
		        "",
		    },
		    1);
	}
	any_array_t *additional = NULL;
	if (target_count > 1) {
		additional = any_array_create_from_raw((void *[]){}, 0);
		for (i32 i = 1; i < target_count; ++i) {
			any_array_push(additional, targets[i]);
		}
	}
	_gpu_begin(targets[0], additional, NULL, GPU_CLEAR_COLOR, 0x00000000, 0.0);
	gpu_set_pipeline(shader->_->pipe);
	uniforms_set_context_consts(shader, empty);
	uniforms_set_obj_consts(shader, g_context->paint_object->base); // Time, picker and other constants the graph reads
	i32 input = 0;
	for (i32 j = 0; shader->texture_units != NULL && j < shader->texture_units->length && j < shader->_->tex_units->length; ++j) {
		if (shader->texture_units->buffer[j]->link != NULL) {
			continue; // Bound by the links of the graph (uniforms_set_obj_consts)
		}
		if (input < input_count && inputs[input] != NULL) {
			gpu_set_texture(shader->_->tex_units->buffer[j], inputs[input]);
		}
		input++;
	}
	i32 next = 0;
	for (i32 i = 0; uniforms != NULL && shader->constants != NULL && i < shader->constants->length && i < shader->_->constants->length; ++i) {
		shader_const_t *c = shader->constants->buffer[i];
		i32             n = c->link == NULL ? make_image_node_uniform_floats(c->type) : 0;
		i32             l = shader->_->constants->buffer[i];
		f32            *u = uniforms + next;
		if (n == 1) {
			gpu_set_float(l, u[0]);
		}
		else if (n == 2) {
			gpu_set_float2(l, u[0], u[1]);
		}
		else if (n == 3) {
			gpu_set_float3(l, u[0], u[1], u[2]);
		}
		else if (n == 4) {
			gpu_set_float4(l, u[0], u[1], u[2], u[3]);
		}
		next += n;
	}
	gpu_set_vertex_buffer(util_render_screen_aligned_full_vb);
	gpu_set_index_buffer(util_render_screen_aligned_full_ib);
	gpu_draw();
	gpu_end();
	_mesh_object_last_pipeline = NULL; // Mesh draws set their pipeline again
	if (additional != NULL) {
		array_free(additional);
		free(additional);
	}
}

// A ping-pong pair of size x size render targets of a format: pass 1 reads src and writes dst, then make_image_node_ping_pong_swap, and so on.
// Nothing is in src at first: the first pass reads an input of its own (or src is written first).
make_image_node_ping_pong_t make_image_node_ping_pong_create(i32 size, gpu_texture_format_t format) {
	make_image_node_ping_pong_t pp = {0};
	pp.targets[0]                  = gpu_create_render_target(size, size, format);
	pp.targets[1]                  = gpu_create_render_target(size, size, format);
	pp.src                         = 0;
	return pp;
}

gpu_texture_t *make_image_node_ping_pong_src(make_image_node_ping_pong_t *pp) {
	return pp->targets[pp->src];
}

gpu_texture_t *make_image_node_ping_pong_dst(make_image_node_ping_pong_t *pp) {
	return pp->targets[1 - pp->src];
}

void make_image_node_ping_pong_swap(make_image_node_ping_pong_t *pp) {
	pp->src = 1 - pp->src;
}

// Frees both targets except keep (the one that holds the result: the node keeps it, may be NULL)
void make_image_node_ping_pong_free(make_image_node_ping_pong_t *pp, gpu_texture_t *keep) {
	for (i32 i = 0; i < 2; ++i) {
		if (pp->targets[i] != NULL && pp->targets[i] != keep) {
			gpu_delete_texture(pp->targets[i]);
		}
		pp->targets[i] = NULL;
	}
}

#define MAKE_IMAGE_NODE_CAPTURE_ALIGN 16 // Every capture of a type with several starts at a multiple of this (any format can follow any other)

// The render target format of a capture format (IMAGE_CAPTURE_*)
static gpu_texture_format_t make_image_node_capture_gpu_format(i32 format) {
	switch (format) {
	case IMAGE_CAPTURE_RGBA32F:
		return GPU_TEXTURE_FORMAT_RGBA128;
	case IMAGE_CAPTURE_R32F:
		return GPU_TEXTURE_FORMAT_R32;
	case IMAGE_CAPTURE_R16F:
		return GPU_TEXTURE_FORMAT_R16;
	case IMAGE_CAPTURE_R8:
		return GPU_TEXTURE_FORMAT_R8;
	}
	return GPU_TEXTURE_FORMAT_RGBA32;
}

// The format name of the shader context (render_path_get_tex_format) of a render target format
static char *make_image_node_gpu_format_name(gpu_texture_format_t format) {
	switch (format) {
	case GPU_TEXTURE_FORMAT_RGBA128:
		return "RGBA128";
	case GPU_TEXTURE_FORMAT_RGBA64:
		return "RGBA64";
	case GPU_TEXTURE_FORMAT_R32:
		return "R32";
	case GPU_TEXTURE_FORMAT_R16:
		return "R16";
	case GPU_TEXTURE_FORMAT_R8:
		return "R8";
	}
	return "RGBA32";
}

// The format name of the shader context of a capture format
static char *make_image_node_capture_format_name(i32 format) {
	return make_image_node_gpu_format_name(make_image_node_capture_gpu_format(format));
}

// Channels of a capture format: 4 for the RGBA formats, 1 for the single channel formats (the value is in the first)
i32 make_image_node_capture_channels(i32 format) {
	return format == IMAGE_CAPTURE_R32F || format == IMAGE_CAPTURE_R16F || format == IMAGE_CAPTURE_R8 ? 1 : 4;
}

// Bytes of a capture of size x size pixels in a format
size_t make_image_node_capture_bytes(i32 size, i32 format) {
	return (size_t)size * size * gpu_texture_format_size(make_image_node_capture_gpu_format(format));
}

// Start of the capture that follows a capture of size and format that starts at offset (for process of a type with several captures)
size_t make_image_node_capture_next(size_t offset, i32 size, i32 format) {
	size_t end = offset + make_image_node_capture_bytes(size, format);
	return (end + MAKE_IMAGE_NODE_CAPTURE_ALIGN - 1) / MAKE_IMAGE_NODE_CAPTURE_ALIGN * MAKE_IMAGE_NODE_CAPTURE_ALIGN;
}

// Renders expr(node) over the image square into a size x size target of a capture format (IMAGE_CAPTURE_*) and reads it back.
// Returns the pixels (the caller frees them), or NULL when the readback fails.
static u8 *make_image_node_capture_pixels(ui_node_t *node, char *(*expr)(ui_node_t *), i32 size, i32 capture_format) {
	gpu_texture_format_t format = make_image_node_capture_gpu_format(capture_format);
	make_image_node_capture_dst = size;
	shader_context_t *scon      = make_image_node_capture_shader(node, expr, make_image_node_capture_format_name(capture_format), NULL);
	make_image_node_capture_dst = 0;
	gpu_texture_t *target = gpu_create_render_target(size, size, format);
	make_image_node_gpu_pass(scon, &target, 1, NULL, 0, NULL);
	make_image_node_gpu_shader_free(scon);

	// The readback fills a plane of ours (gpu_get_texture_pixels uses the buffer of the texture when it has one), mapped from the OS
	// when big, so freeing it gives the memory back. gpu_delete_texture frees the buffer struct and the NULL buffer pointer.
	size_t bytes  = (size_t)size * size * gpu_texture_format_size(format);
	u8    *plane  = make_image_node_plane_alloc(bytes);
	target->buffer = plane != NULL ? calloc(1, sizeof(buffer_t)) : NULL;
	if (target->buffer != NULL) {
		target->buffer->buffer = plane;
	}
	buffer_t *b = target->buffer != NULL ? gpu_get_texture_pixels(target) : NULL;
	if (b == NULL) {
		if (target->buffer != NULL) {
			target->buffer->buffer = NULL;
		}
		make_image_node_plane_free(plane, bytes);
		gpu_delete_texture(target);
		return NULL;
	}
	u8 *pixels = b->buffer;
	b->buffer  = NULL; // Kept, gpu_delete_texture frees the rest
	gpu_delete_texture(target);
	return pixels;
}

// Default of input i / setting i in the definition of the node type, 0 when the definition has no such item. The safety net of the
// accessors below for a node that has fewer inputs or settings than its definition.
static f32 make_image_node_def_input_default(ui_node_t *node, i32 i) {
	ui_node_t *def = nodes_material_get_node_t(node->type);
	return def != NULL && i >= 0 && i < def->inputs->length ? def->inputs->buffer[i]->default_value->buffer[0] : 0.0f;
}

static f32 make_image_node_def_button_default(ui_node_t *node, i32 i) {
	ui_node_t *def = nodes_material_get_node_t(node->type);
	return def != NULL && i >= 0 && i < def->buttons->length ? def->buttons->buffer[i]->default_value->buffer[0] : 0.0f;
}

// Completes a node saved by an older build with the inputs and settings its definition has now. A saved node keeps the length it was
// created with, the code indexes by the enums of the definition. The fork only ever appends inputs and settings at the end of a
// definition (never reorders, inserts or removes: that breaks old saves, which is accepted), so the missing items are the tail of the
// definition, appended with its defaults. Links address inputs by index, so they stay valid. nodes is the canvas the node is in (new
// sockets get free ids). Called when the node is traversed for its capture and when the node editor lays it out; cheap when nothing is missing.
// Returns true when something was added.
bool make_image_node_reconcile(ui_node_t *node, ui_node_t_array_t *nodes) {
	if (node->inputs == NULL || node->buttons == NULL) {
		return false;
	}
	ui_node_t *def = nodes_material_get_node_t(node->type);
	if (def == NULL || (node->inputs->length >= def->inputs->length && node->buttons->length >= def->buttons->length)) {
		return false;
	}
	for (i32 i = node->inputs->length; i < def->inputs->length; ++i) {
		ui_node_socket_t *d   = def->inputs->buffer[i];
		ui_node_socket_t *soc = ALLOC_INIT(ui_node_socket_t, {0});
		soc->id               = ui_get_socket_id(nodes);
		soc->node_id          = node->id;
		soc->name             = string_copy(d->name);
		soc->type             = string_copy(d->type);
		soc->color            = d->color;
		soc->default_value    = f32_array_create_from_array(d->default_value);
		soc->min              = d->min;
		soc->max              = d->max;
		soc->precision        = d->precision;
		soc->display          = d->display;
		any_array_push(node->inputs, soc);
	}
	for (i32 i = node->buttons->length; i < def->buttons->length; ++i) {
		ui_node_button_t *d   = def->buttons->buffer[i];
		ui_node_button_t *but = ALLOC_INIT(ui_node_button_t, {0});
		but->name             = string_copy(d->name);
		but->type             = string_copy(d->type);
		but->output           = d->output;
		but->default_value    = f32_array_create_from_array(d->default_value);
		if (d->data != NULL) {
			but->data = u8_array_create_from_array(d->data);
		}
		but->min       = d->min;
		but->max       = d->max;
		but->precision = d->precision;
		but->height    = d->height;
		any_array_push(node->buttons, but);
	}
	return true;
}

static char *make_image_node_input_expr(ui_node_t *node) {
	char *v = parser_material_parse_value_input(node->inputs->buffer[make_image_node_capture_input], false);
	return string_tmp("float4(%s, %s, %s, 1.0)", v, v, v);
}

// Which capture of the node is being written, 0 to capture_count - 1; for the capture callback of a type with several
i32 make_image_node_capture_index() {
	return make_image_node_capture_k;
}

// The parameters of the node being captured (what the type's params callback filled in), for the capture callback
void *make_image_node_capture_params() {
	return make_image_node_capture_params_ptr;
}

// Width and height of capture k: the type's capture_size (0 or no callback: the size of the node)
static i32 make_image_node_capture_size_of(image_node_type_t *t, ui_node_t *node, i32 size, i32 k) {
	i32 ks = t->capture_size != NULL ? t->capture_size(node, k) : 0;
	return ks > 0 ? ks : size;
}

// Format of capture k (IMAGE_CAPTURE_*)
static i32 make_image_node_capture_format_of(image_node_type_t *t, ui_node_t *node, i32 k) {
	return t->capture_format_at != NULL ? t->capture_format_at(node, k) : t->capture_format;
}

#define MAKE_IMAGE_NODE_MAX_THREADS 32

typedef struct make_image_node_rows_job {
	void (*fn)(void *ctx, i32 begin, i32 end);
	void *ctx;
	i32   begin;
	i32   end;
} make_image_node_rows_job_t;

static void make_image_node_rows_thread(void *param) {
	make_image_node_rows_job_t *j = param;
	j->fn(j->ctx, j->begin, j->end);
}

// Runs fn over [0, count) split into contiguous ranges, one per hardware thread (the calling thread takes the first), and waits for
// all of them. For the cpu passes of the image nodes (rows or columns are independent): fn must only write inside its own range.
void make_image_node_parallel(i32 count, void (*fn)(void *ctx, i32 begin, i32 end), void *ctx) {
	i32 threads = iron_hardware_threads();
	threads     = threads > MAKE_IMAGE_NODE_MAX_THREADS ? MAKE_IMAGE_NODE_MAX_THREADS : threads;
	threads     = threads > count ? count : threads;
	if (threads <= 1) {
		fn(ctx, 0, count);
		return;
	}
	make_image_node_rows_job_t jobs[MAKE_IMAGE_NODE_MAX_THREADS];
	iron_thread_t              handles[MAKE_IMAGE_NODE_MAX_THREADS];
	for (i32 i = 0; i < threads; ++i) {
		jobs[i] = (make_image_node_rows_job_t){.fn = fn, .ctx = ctx, .begin = (i32)((i64)count * i / threads), .end = (i32)((i64)count * (i + 1) / threads)};
	}
	for (i32 i = 1; i < threads; ++i) {
		iron_thread_init(&handles[i], make_image_node_rows_thread, &jobs[i]);
	}
	fn(ctx, jobs[0].begin, jobs[0].end);
	for (i32 i = 1; i < threads; ++i) {
		iron_thread_wait_and_destroy(&handles[i]);
	}
}

// Value of a number input of the node being captured, for the whole image: the default value when unlinked,
// else the linked graph evaluated once at the image centre. Called from the type's params callback.
f32 make_image_node_input_value(ui_node_t *node, i32 input) {
	if (input < 0 || input >= node->inputs->length) {
		return make_image_node_def_input_default(node, input); // A node saved with fewer inputs than its definition (see make_image_node_reconcile)
	}
	ui_node_socket_t *socket = node->inputs->buffer[input];
	if (make_image_node_capturing == NULL || parser_material_get_input_link(socket) == NULL) {
		return socket->default_value->buffer[0];
	}
	make_image_node_capture_input = input;
	f32 *pixels                   = (f32 *)make_image_node_capture_pixels(node, make_image_node_input_expr, 1, IMAGE_CAPTURE_R32F);
	if (pixels == NULL) {
		return socket->default_value->buffer[0];
	}
	f32 v = pixels[0]; // A 1 x 1 target: its pixel centre is the image centre
	make_image_node_plane_free(pixels, 4);
	if (!isfinite(v)) {
		return socket->default_value->buffer[0]; // A NaN (Square Root of a negative value) or infinity would turn the whole result into one
	}
	return v; // Huge finite values stay, the nodes clamp them
}

// True when nothing that a capture reads has changed since the node's last capture: the graph of the material (util_version_graph,
// bumped by every canvas change: editor, undo, groups, scripts, imports), its canvas object and the image assets. The node is
// not captured again then, a capture is a render, a readback of up to 268 MB and a hash. An image node upstream that just baked
// is not covered here: make_image_node_run_material captures the nodes downstream of a bake itself.
static bool make_image_node_unchanged(make_image_node_state_t *s) {
	return s->captured && !s->needs_capture && s->captured_canvas == g_context->material->canvas &&
	       s->captured_graph_version == util_version_graph(g_context->material) && s->captured_asset_version == util_version_assets();
}

// Signature of a bake by reference: the xxHash64 of the shader source (every literal of the inputs and settings is in it, a linked
// input is its graph), the size, the params, the asset version and the baked signature of every image node the shader samples (an
// upstream node that baked other pixels changes it, one that did not does not)
static u64 make_image_node_reference_signature(make_image_node_state_t *s, u64 source_hash, i32 size, void *params, i32 params_size) {
	u64 h = make_image_node_hash(source_hash, &size, sizeof(size));
	h     = make_image_node_hash(h, params, params_size);
	u32 assets = util_version_assets();
	h          = make_image_node_hash(h, &assets, sizeof(assets));
	for (i32 i = 0; s->upstream != NULL && i < s->upstream->length; ++i) {
		make_image_node_state_t *u  = make_image_node_states != NULL ? any_map_get(make_image_node_states, s->upstream->buffer[i]) : NULL;
		u64                      up = u != NULL && u->has_baked ? u->baked_signature : 0;
		h                           = make_image_node_hash(h, s->upstream->buffer[i], strlen(s->upstream->buffer[i]));
		h                           = make_image_node_hash(h, &up, sizeof(up));
	}
	return h;
}

#define MAKE_IMAGE_NODE_MAX_GPU_CAPTURES 8

// Capture of a node whose result is a render target (image_node_type_t.result_by_reference): the shader of every capture is built and
// compiled (the expression of the node, capture k = make_image_node_capture_index), the signature compared with what is baked, failed or
// pending, and only a new signature renders. A node without gpu_bake has one capture, drawn into a render target of the result format, which
// IS the result. With gpu_bake the captures are drawn into render targets of their capture formats and sizes (no readback) and gpu_bake
// runs the passes of the node and returns the result target. The result waits for the debounce as pending_target (make_image_node_bake_reference
// makes it the result). params is freed here. Returns true when the node is pending.
static bool make_image_node_capture_reference(make_image_node_state_t *s, char *id, ui_node_t *node, i32 size, void *params, bool force) {
	image_node_type_t *t           = s->type;
	f64                start       = sys_time();
	i32                count       = t->gpu_bake != NULL && t->capture_count != NULL ? t->capture_count(node) : 1;
	count                          = count < 1 ? 1 : count > MAKE_IMAGE_NODE_MAX_GPU_CAPTURES ? MAKE_IMAGE_NODE_MAX_GPU_CAPTURES : count;
	shader_context_t *scons[MAKE_IMAGE_NODE_MAX_GPU_CAPTURES];
	gpu_texture_format_t formats[MAKE_IMAGE_NODE_MAX_GPU_CAPTURES];
	i32               sizes[MAKE_IMAGE_NODE_MAX_GPU_CAPTURES];
	u64               source_hash = 0;
	for (i32 k = 0; k < count; ++k) {
		make_image_node_capture_k   = k;
		sizes[k]                    = make_image_node_capture_size_of(t, node, size, k);
		formats[k]                  = t->gpu_bake != NULL ? make_image_node_capture_gpu_format(make_image_node_capture_format_of(t, node, k)) : t->result_format;
		u64 one                     = 0;
		make_image_node_capture_dst = sizes[k];
		scons[k]                    = make_image_node_capture_shader(node, t->capture, make_image_node_gpu_format_name(formats[k]), &one);
		make_image_node_capture_dst = 0;
		source_hash                 = make_image_node_hash(source_hash, &one, sizeof(one));
	}
	make_image_node_capture_k = 0;
	u64 sig                   = make_image_node_reference_signature(s, source_hash, size, params, t->params_size);
	s->captured               = true;
	s->captured_canvas        = g_context->material->canvas;
	s->captured_graph_version = util_version_graph(g_context->material);
	s->captured_asset_version = util_version_assets();

	bool has_result = g_context->node_previews != NULL && any_map_get(g_context->node_previews, id) != NULL;
	bool baked      = has_result && s->has_baked && sig == s->baked_signature;
	if (!force && (baked || (s->failed && sig == s->failed_signature) || (s->pending && sig == s->pending_signature))) {
		if (baked) {
			make_image_node_clear_pending(s); // Back to what is baked, e.g. a change undone within the debounce
		}
		for (i32 k = 0; k < count; ++k) {
			make_image_node_gpu_shader_free(scons[k]);
		}
		free(params);
		return false;
	}
	make_image_node_clear_pending(s);
	gpu_texture_t *targets[MAKE_IMAGE_NODE_MAX_GPU_CAPTURES];
	f64            pass_start = sys_time();
	for (i32 k = 0; k < count; ++k) {
		targets[k] = gpu_create_render_target(sizes[k], sizes[k], formats[k]);
		if (targets[k] != NULL) {
			make_image_node_gpu_pass(scons[k], &targets[k], 1, NULL, 0, NULL);
		}
		make_image_node_gpu_shader_free(scons[k]);
	}
	gpu_texture_t *result = targets[0];
	bool           failed = false;
	for (i32 k = 0; k < count; ++k) {
		failed = failed || targets[k] == NULL;
	}
	if (t->gpu_bake != NULL && !failed) {
		result = t->gpu_bake(node, size, params, targets, count);
		for (i32 k = 0; k < count; ++k) {
			if (targets[k] != result) {
				gpu_delete_texture(targets[k]); // The captures are inputs of the passes, the result is what gpu_bake returned
			}
		}
		failed = result == NULL;
	}
	else if (failed) {
		for (i32 k = 0; k < count; ++k) {
			if (targets[k] != NULL) {
				gpu_delete_texture(targets[k]);
			}
		}
	}
	free(params);
	if (failed) {
		console_error(tr("Image node bake: out of memory"));
		s->failed           = true;
		s->failed_signature = sig; // Not retried for this signature, like a bake that failed
		return false; // Keep what is baked
	}
	if (make_image_node_trace) {
		gpu_execute_and_wait(); // The time of the render, not of its submission
	}
	s->gpu_pass_ms   = (sys_time() - pass_start) * 1000.0;
	s->gpu_render_ms = (sys_time() - start) * 1000.0;
	s->gpu_renders++;
	if (make_image_node_trace) {
		iron_log("[image node] render %s %d x %d %.3f ms (passes %.3f ms)", id, size, size, s->gpu_render_ms, s->gpu_pass_ms);
	}
	s->pending_target    = result;
	s->pending_size      = size;
	s->pending_signature = sig;
	s->pending           = true;
	return true;
}

// Captures the node in image space. A new capture or parameter set makes the node pending, returns true then.
// force keeps it pending even when it matches the baked result.
static bool make_image_node_capture(make_image_node_state_t *s, char *id, ui_node_t *node, ui_node_canvas_t *group, ui_node_t_array_t *parents, bool force) {
	image_node_type_t *t = s->type;
	s->needs_capture     = false;
	if (t == NULL) {
		return false;
	}
	make_image_node_clear_upstream(s);
	make_image_node_captures++;
	if (make_image_node_trace) {
		iron_log("[image node] capture %d %s time %.3f", make_image_node_captures, id, sys_time());
	}
	s->cycle_seen                   = false;
	make_image_node_capturing       = s;
	make_image_node_capture_group   = group;
	make_image_node_capture_parents = parents;
	i32   size                      = t->size(node);
	// The parameters first: the capture callbacks may read them (make_image_node_capture_params), and a linked scalar input is
	// captured here, not inside the shader of another capture
	void *params = calloc(1, t->params_size > 0 ? t->params_size : 1);
	if (params != NULL) {
		parser_material_init();
		make_image_node_parser_scope();
	}
	if (params != NULL && t->params != NULL) {
		t->params(node, params);
	}
	make_image_node_capture_params_ptr = params;
	if (params != NULL && t->result_by_reference) {
		bool pending                       = make_image_node_capture_reference(s, id, node, size, params, force);
		make_image_node_capture_params_ptr = NULL;
		make_image_node_capturing          = NULL;
		make_image_node_capture_group      = NULL;
		make_image_node_capture_parents    = NULL;
		if (!s->cycle_seen) {
			s->cycle_told = false; // The loop is gone, tells again when it returns
		}
		return pending;
	}
	// The count after the scope: a capture_count that reads the links of the node (Advanced Blur: a Mask plane only when linked)
	// would read the links of whatever the parser was pointed at last, and process (which reads the same links through params)
	// would find a different number of planes than the capture made
	i32 count = t->capture_count != NULL ? t->capture_count(node) : 1;
	u8    *pixels                      = NULL;
	size_t total                       = 0; // Bytes of all captures, each of its own size and format, starting at multiples of 16
	size_t offset                      = 0;
	size_t start = 0;
	for (i32 k = 0; k < count && params != NULL; ++k) {
		i32 ks = make_image_node_capture_size_of(t, node, size, k);
		i32 kf = make_image_node_capture_format_of(t, node, k);
		total  = start + make_image_node_capture_bytes(ks, kf);
		start  = make_image_node_capture_next(start, ks, kf);
	}
	for (i32 k = 0; k < count && params != NULL; ++k) {
		make_image_node_capture_k = k;
		i32    ks                 = make_image_node_capture_size_of(t, node, size, k);
		i32    kf                 = make_image_node_capture_format_of(t, node, k);
		size_t one                = make_image_node_capture_bytes(ks, kf);
		u8    *p                  = make_image_node_capture_pixels(node, t->capture, ks, kf);
		if (p == NULL) {
			make_image_node_plane_free(pixels, total);
			pixels = NULL;
			break;
		}
		if (count == 1) {
			pixels = p;
			break;
		}
		if (pixels == NULL) {
			pixels = make_image_node_plane_alloc(total);
			if (pixels == NULL) {
				make_image_node_plane_free(p, one);
				break;
			}
		}
		memcpy(pixels + offset, p, one);
		offset = make_image_node_capture_next(offset, ks, kf);
		make_image_node_plane_free(p, one);
	}
	make_image_node_capture_k = 0;
	make_image_node_capture_params_ptr = NULL;
	if (pixels == NULL) {
		free(params);
		params = NULL;
	}
	make_image_node_capturing       = NULL;
	make_image_node_capture_group   = NULL;
	make_image_node_capture_parents = NULL;
	if (!s->cycle_seen) {
		s->cycle_told = false; // The loop is gone, tells again when it returns
	}
	if (pixels == NULL) {
		return false; // Capture failed, keep what is baked
	}
	s->captured               = true;
	s->captured_canvas        = g_context->material->canvas;
	s->captured_graph_version = util_version_graph(g_context->material);
	s->captured_asset_version = util_version_assets();
	u64 sig = make_image_node_signature(pixels, total, size, params, t->params_size);

	bool has_result = g_context->node_previews != NULL && any_map_get(g_context->node_previews, id) != NULL;
	if (!force && has_result && s->has_baked && sig == s->baked_signature) {
		make_image_node_clear_pending(s); // Back to what is baked, e.g. a change undone within the debounce
		make_image_node_plane_free(pixels, total);
		free(params);
		return false;
	}
	if (!force && s->failed && sig == s->failed_signature) {
		make_image_node_plane_free(pixels, total); // Failed already, retried after a change or Regenerate Image Nodes
		free(params);
		return false;
	}
	if (!force && s->pending && sig == s->pending_signature) {
		make_image_node_plane_free(pixels, total); // Already waiting for this one
		free(params);
		return false;
	}
	make_image_node_clear_pending(s);
	s->pending_pixels    = pixels;
	s->pending_bytes     = total;
	s->pending_params    = params;
	s->pending_size      = size;
	s->pending_signature = sig;
	s->pending           = true;
	return true;
}

// Uploads size * size * 4 floats as the node's result, in the type's format. Returns false when memory runs out.
// A half float result (RGBA64) is converted in place: result is not usable after this call (no staging copy of half the size).
static bool make_image_node_upload(char *id, f32 *result, i32 size, gpu_texture_format_t format) {
	size_t n = (size_t)size * size * 4;
	if (format != GPU_TEXTURE_FORMAT_RGBA128) {
		format = GPU_TEXTURE_FORMAT_RGBA64;
		for (size_t i = 0; i < n; ++i) {
			u16 h = float_to_half_fast(result[i]); // The write at byte 2i never reaches the read at byte 4i of a later texel
			memcpy((u8 *)result + 2 * i, &h, sizeof(h));
		}
	}
	buffer_t       b   = {.buffer = (u8 *)result, .length = (i32)(n * (format == GPU_TEXTURE_FORMAT_RGBA128 ? sizeof(f32) : sizeof(u16)))};
	gpu_texture_t *tmp = gpu_create_texture_from_bytes_raw(&b, size, size, format);
	if (tmp == NULL) {
		return false;
	}

	gpu_texture_t *image = any_map_get(g_context->node_previews, id);
#ifdef IRON_METAL
	// Metal samples the uploaded texture as it is and reads back from it like from a render target: it becomes the result,
	// no render target of the same size next to it and no copy (the other backends flip the uv of a texture from bytes, see below)
	if (image != NULL) {
		gpu_delete_texture(image);
	}
	any_map_set(g_context->node_previews, string_copy(id), tmp);
	return true;
#else
	if (image == NULL || image->width != size || image->height != size || image->format != format) {
		if (image != NULL) {
			gpu_delete_texture(image);
		}
		image = gpu_create_render_target(size, size, format);
		any_map_set(g_context->node_previews, string_copy(id), image);
	}
	// Drawn into a render target like the captured input, so the orientation matches on every backend
	draw_begin(image, false, 0);
	draw_set_pipeline(format == GPU_TEXTURE_FORMAT_RGBA128 ? pipes_copy128 : pipes_copy64);
	draw_image(tmp, 0, 0);
	draw_set_pipeline(NULL);
	draw_end();
	gpu_delete_texture(tmp);
	return true;
#endif
}

// Deletes the result texture of an image node and its map entry, the node renders black again
static void make_image_node_delete_result(char *id) {
	if (g_context->node_previews == NULL) {
		return;
	}
	string_array_t *keys = map_keys(g_context->node_previews);
	for (i32 i = 0; i < keys->length; ++i) {
		char *key = keys->buffer[i];
		if (string_equals(key, id)) {
			gpu_texture_t *image = any_map_get(g_context->node_previews, key);
			if (image != NULL) {
				gpu_delete_texture(image);
			}
			map_delete(g_context->node_previews, key);
			free(key);
			break;
		}
	}
	array_free(keys);
	free(keys);
}

// A node whose result is its render target (make_image_node_capture_reference): the rendered target replaces the result, nothing else to do
static void make_image_node_bake_reference(make_image_node_state_t *s, char *id) {
	gpu_texture_t *old = any_map_get(g_context->node_previews, id);
	if (old != NULL && old != s->pending_target) {
		gpu_delete_texture(old);
	}
	any_map_set(g_context->node_previews, string_copy(id), s->pending_target);
	s->pending_target  = NULL; // The result owns it now
	s->has_baked       = true;
	s->baked_signature = s->pending_signature;
	s->failed          = false;
	make_image_node_clear_pending(s);
}

// Processes the pending capture of s into its result texture
static void make_image_node_bake(make_image_node_state_t *s, char *id) {
	if (!s->pending || s->type == NULL) {
		return;
	}
	if (g_context->node_previews == NULL) {
		g_context->node_previews = any_map_create();
	}
	if (s->type->result_by_reference) {
		make_image_node_bake_reference(s, id);
		return;
	}
	image_node_type_t *t      = s->type;
	i32                size   = s->pending_size;
	size_t             result_bytes = (size_t)size * size * 4 * sizeof(f32);
	f32               *result       = make_image_node_plane_alloc(result_bytes);
	i32                status       = -1;
	if (result != NULL) {
		status = t->process(s->pending_pixels, size, s->pending_params, result);
		make_image_node_plane_free(s->pending_pixels, s->pending_bytes); // The capture is not needed during the upload
		free(s->pending_params);
		s->pending_pixels = NULL;
		s->pending_bytes  = 0;
		s->pending_params = NULL;
		if (!make_image_node_upload(id, result, size, t->result_format)) {
			status = -1;
		}
		make_image_node_plane_free(result, result_bytes);
	}
	if (status < 0) {
		console_error(tr("Image node bake: out of memory"));
		make_image_node_delete_result(id); // Black result, not the previous texture
		s->has_baked        = false;
		s->failed           = true;
		s->failed_signature = s->pending_signature;
	}
	else {
		s->has_baked       = true;
		s->baked_signature = s->pending_signature;
		s->failed          = false;
	}
	make_image_node_clear_pending(s);
}

// Shows a new result: re-parses the material (downstream Blur, Warp and image nodes follow), refills fill layers
static void make_image_node_refresh(slot_material_t *m, ui_node_t *node) {
	if (m == g_context->material) {
		make_material_parse_paint_material(true); // Captures again, finds the baked signatures
		if (layers_is_fill_material()) {
			layers_update_fill_layers();
		}
		util_render_make_material_preview();
		if (ui_nodes_canvas_type == CANVAS_TYPE_MATERIAL && array_index_of(ui_nodes_get_canvas(true)->nodes, node) >= 0) {
			ui_nodes_node_changed = node; // Node previews downstream
		}
	}
	else {
		slot_material_t *_material = g_context->material;
		g_context->material        = m;
		layers_update_fill_layers();
		g_context->material = _material;
		make_material_parse_paint_material(false);
	}
	make_image_node_redraw_nodes();
	if (ui_view2d_hwnd != NULL) {
		ui_view2d_hwnd->redraws = 2;
	}
	g_context->ddirty = 2;
}

// Lists the image nodes of nodes (the current material, groups included) with their states
static void make_image_node_collect(any_array_t *entries, ui_node_t_array_t *nodes, ui_node_canvas_t *group, ui_node_t_array_t *parents) {
	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *node = nodes->buffer[i];
		if (make_image_node_type_of(node->type) != NULL) {
			make_image_node_entry_t *e = calloc(1, sizeof(make_image_node_entry_t));
			e->node                    = node;
			e->group                   = group;
			e->parents                 = any_array_create_from_raw((void *[]){}, 0);
			for (i32 j = 0; j < parents->length; ++j) {
				any_array_push(e->parents, parents->buffer[j]);
			}
			e->id    = string_copy(make_material_node_preview_id(node, parents));
			e->state = make_image_node_get_state(e->id, node);
			any_array_push(entries, e);
		}
		else if (string_equals(node->type, "GROUP")) {
			for (i32 j = 0; j < g_project->_->material_groups->length; ++j) {
				node_group_t *g = g_project->_->material_groups->buffer[j];
				if (string_equals(g->canvas->name, node->name)) {
					any_array_push(parents, node);
					make_image_node_collect(entries, g->canvas->nodes, g->canvas, parents);
					array_pop(parents);
					break;
				}
			}
		}
	}
}

static void make_image_node_free_entries(any_array_t *entries) {
	for (i32 i = 0; i < entries->length; ++i) {
		make_image_node_entry_t *e = entries->buffer[i];
		array_free(e->parents);
		free(e->parents);
		free(e->id);
		free(e);
	}
	array_free(entries);
	free(entries);
}

// True when the image node with preview id from depends, through the recorded upstream, on the state target (or is it)
static bool make_image_node_reaches(char *from, make_image_node_state_t *target, string_array_t *seen) {
	make_image_node_state_t *s = make_image_node_states != NULL ? any_map_get(make_image_node_states, from) : NULL;
	if (s == NULL || string_array_index_of(seen, from) >= 0) {
		return false;
	}
	if (s == target) {
		return true;
	}
	any_array_push(seen, from);
	for (i32 i = 0; s->upstream != NULL && i < s->upstream->length; ++i) {
		if (make_image_node_reaches(s->upstream->buffer[i], target, seen)) {
			return true;
		}
	}
	return false;
}

static bool make_image_node_depends_on(char *from, make_image_node_state_t *target) {
	string_array_t *seen = any_array_create_from_raw((void *[]){}, 0);
	bool            r    = make_image_node_reaches(from, target, seen);
	array_free(seen);
	free(seen);
	return r;
}

// True when the node s samples (the preview id upstream_id) is part of a loop with s: it depends on s again, or is s.
// Such an input is read as black by a capture: baking a node from its own result never settles.
static bool make_image_node_edge_cyclic(make_image_node_state_t *s, char *upstream_id) {
	return make_image_node_depends_on(upstream_id, s);
}

// Tells the Console once per loop (not once per node of it) that the capture of s reads the input of upstream_id as black
static void make_image_node_tell_cycle(make_image_node_state_t *s, char *upstream_id) {
	s->cycle_seen = true;
	if (s->cycle_told) {
		return;
	}
	bool            told = false; // A node of the loop told already: it depends on s and the node of upstream_id depends on it
	string_array_t *keys = make_image_node_states_keys();
	for (i32 i = 0; i < keys->length && !told; ++i) {
		make_image_node_state_t *m = any_map_get(make_image_node_states, keys->buffer[i]);
		told                       = m->cycle_told && make_image_node_depends_on(keys->buffer[i], s) && make_image_node_depends_on(upstream_id, m);
	}
	make_image_node_free_keys(keys);
	s->cycle_told = true;
	if (!told) {
		console_error(tr("Image node loop: looped input reads black"));
	}
}

// True while an image node the capture of e samples has not been handled in this run (a node of a loop with e does not count)
static bool make_image_node_waits(make_image_node_entry_t *e, any_array_t *entries) {
	string_array_t *upstream = e->state->upstream;
	if (upstream == NULL) {
		return false;
	}
	for (i32 i = 0; i < entries->length; ++i) {
		make_image_node_entry_t *f = entries->buffer[i];
		if (f != e && !f->done && string_array_index_of(upstream, f->id) >= 0 && !make_image_node_edge_cyclic(e->state, f->id)) {
			return true;
		}
	}
	return false;
}

// Captures and bakes the image nodes of the current material that wait, inner nodes of a chain first:
// a node whose capture samples a node baked in this run is captured again before its own bake.
// Returns the first node baked, NULL when nothing was baked.
static ui_node_t *make_image_node_run_material(slot_material_t *m, bool force_all) {
	any_array_t       *entries = any_array_create_from_raw((void *[]){}, 0);
	ui_node_t_array_t *parents = any_array_create_from_raw((void *[]){}, 0);
	make_image_node_collect(entries, m->canvas->nodes, NULL, parents);
	array_free(parents);
	free(parents);

	string_array_t *baked   = any_array_create_from_raw((void *[]){}, 0); // Preview ids baked in this run
	ui_node_t      *changed = NULL;
	bool            cycle   = false;
	for (i32 left = entries->length; left > 0;) {
		bool progress = false;
		for (i32 i = 0; i < entries->length; ++i) {
			make_image_node_entry_t *e = entries->buffer[i];
			if (e->done || (!cycle && make_image_node_waits(e, entries))) {
				continue;
			}
			make_image_node_state_t *s              = e->state;
			bool                     upstream_baked = false;
			for (i32 j = 0; s->upstream != NULL && j < s->upstream->length; ++j) {
				upstream_baked = upstream_baked || string_array_index_of(baked, s->upstream->buffer[j]) >= 0;
			}
			if (force_all || s->needs_capture || upstream_baked) {
				make_image_node_capture(s, e->id, e->node, e->group, e->parents, force_all);
				if (!cycle && make_image_node_waits(e, entries)) {
					// The capture just recorded the upstream of a node never captured before: wait for it and capture again.
					// Not progress, so a cycle still trips the cycle flag at the end of a pass without a finished node.
					s->needs_capture = true;
					continue;
				}
			}
			if (s->pending) {
				make_image_node_bake(s, e->id);
				any_array_push(baked, e->id);
				if (changed == NULL) {
					changed = e->node;
				}
			}
			e->done  = true;
			progress = true;
			left--;
		}
		cycle = !progress; // Links never form a cycle, but never loop forever
	}
	array_free(baked);
	free(baked);
	make_image_node_free_entries(entries);
	return changed;
}

static slot_material_t *make_image_node_material(i32 id) {
	for (i32 i = 0; i < g_project->_->materials->length; ++i) {
		slot_material_t *m = g_project->_->materials->buffer[i];
		if (m->id == id) {
			return m;
		}
	}
	return NULL;
}

// Bakes the materials whose image nodes wait (every material when force_all) and refreshes the ones that changed
static void make_image_node_run(bool force_all) {
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}

	i32_array_t *materials = i32_array_create(0);
	for (i32 i = 0; i < make_image_node_state_slots(); ++i) {
		make_image_node_state_t *s = make_image_node_state_at(i);
		if (s != NULL && (s->pending || s->needs_capture) && i32_array_index_of(materials, s->material_id) == -1) {
			i32_array_push(materials, s->material_id);
		}
	}
	for (i32 i = 0; force_all && i < g_project->_->materials->length; ++i) {
		slot_material_t *m = g_project->_->materials->buffer[i];
		if (i32_array_index_of(materials, m->id) == -1) {
			i32_array_push(materials, m->id);
		}
	}

	slot_material_t   *_material = g_context->material;
	ui_node_t_array_t *changed   = any_array_create_from_raw((void *[]){}, 0); // Per material, the first node baked or NULL
	for (i32 i = 0; i < materials->length; ++i) {
		slot_material_t *m    = make_image_node_material(materials->buffer[i]);
		ui_node_t       *node = NULL;
		if (m != NULL) {
			g_context->material = m;
			node                = make_image_node_run_material(m, force_all);
			g_context->material = _material;
		}
		any_array_push(changed, node);
	}

	// What is still waiting lost its node or its material
	for (i32 i = 0; i < make_image_node_state_slots(); ++i) {
		make_image_node_state_t *s = make_image_node_state_at(i);
		if (s != NULL) {
			s->needs_capture = false;
			make_image_node_clear_pending(s);
		}
	}

	// The refresh captures again and may schedule what changed meanwhile
	for (i32 i = 0; i < materials->length; ++i) {
		slot_material_t *m = make_image_node_material(materials->buffer[i]);
		if (m != NULL && changed->buffer[i] != NULL) {
			make_image_node_refresh(m, changed->buffer[i]);
		}
	}
	array_free(materials);
	free(materials);
	array_free(changed);
	free(changed);

	if (in_use) {
		draw_begin(current, false, 0);
	}
}

static bool make_image_node_any_waiting() {
	if (make_image_node_force_all) {
		return true;
	}
	for (i32 i = 0; i < make_image_node_state_slots(); ++i) {
		make_image_node_state_t *s = make_image_node_state_at(i);
		if (s != NULL && (s->pending || s->needs_capture)) {
			return true;
		}
	}
	return false;
}

static void make_image_node_update(void *_) {
	if (!make_image_node_any_waiting()) {
		sys_remove_update(make_image_node_update);
		make_image_node_scheduled = false;
		return;
	}
	// A forced bake (Regenerate image nodes) does not wait for the debounce, only for the mouse and Bake Texture
	if (make_image_node_busy() || (!make_image_node_force_all && sys_time() - make_image_node_last_change < MAKE_IMAGE_NODE_DEBOUNCE)) {
		return;
	}
	sys_remove_update(make_image_node_update);
	make_image_node_scheduled = false;
	bool force_all            = make_image_node_force_all;
	make_image_node_force_all = false;
	make_image_node_run(force_all);
	make_image_node_redraw_nodes();
}

// Node preview traversal hook (make_material_bake_node_preview), linked = refresh during a paint stroke
void make_image_node_on_traverse(ui_node_t *node, ui_node_canvas_t *group, ui_node_t_array_t *parents, bool linked) {
	make_image_node_reconcile(node, group != NULL ? group->nodes : g_context->material->canvas->nodes);
	char *id = make_material_node_preview_id(node, parents);
	any_array_push(g_context->node_previews_used, id); // Keeps the baked result
	make_image_node_state_t *s = make_image_node_get_state(id, node);
	if (linked) {
		return; // A stroke changes layers, never the image space input (Layer nodes are neutral there)
	}
	if (make_image_node_unchanged(s)) {
		return; // Selecting a layer or any other re-parse of an unchanged graph does not capture
	}
	if (make_image_node_busy()) {
		// No readback per frame of a slider drag, or while Bake Texture runs
		s->needs_capture = true;
		make_image_node_schedule();
		return;
	}
	if (make_image_node_capture(s, id, node, group, parents, false)) {
		make_image_node_schedule();
	}
}

// Frees the states of material m, except the preview ids listed in keep (all when keep is NULL)
void make_image_node_delete_states(slot_material_t *m, string_array_t *keep) {
	if (make_image_node_states == NULL) {
		return;
	}
	string_array_t *keys = make_image_node_states_keys();
	for (i32 i = 0; i < keys->length; ++i) {
		char                    *key = keys->buffer[i];
		make_image_node_state_t *s   = any_map_get(make_image_node_states, key);
		if (s->material_id == m->id && (keep == NULL || string_array_index_of(keep, key) == -1)) {
			make_image_node_free_state(s);
			map_delete(make_image_node_states, key);
			free(key);
		}
	}
	make_image_node_free_keys(keys);
}

// State of an image node of the current material (the node being drawn in the editor), NULL before its first traversal. The
// preview id is built like the traversal does, with the group nodes of the open groups as parents (the first instance of a
// group used more than once).
static make_image_node_state_t *make_image_node_state_of(ui_node_t *node, char **id) {
	if (make_image_node_states == NULL) {
		return NULL;
	}
	ui_node_t *chain[16];
	i32        depth = 0;
	ui_node_t_array_t parents = {.buffer = chain, .length = 0, .capacity = 16};
	ui_node_t_array_t *nodes = g_context->material->canvas->nodes;
	for (i32 i = 0; i < ui_nodes_group_stack->length && depth < 16; ++i) {
		char      *name  = ui_nodes_group_stack->buffer[i]->canvas->name;
		ui_node_t *group = NULL;
		for (i32 j = 0; j < nodes->length && group == NULL; ++j) {
			ui_node_t *n = nodes->buffer[j];
			group        = string_equals(n->type, "GROUP") && string_equals(n->name, name) ? n : NULL;
		}
		if (group == NULL) {
			return NULL;
		}
		chain[depth++] = group;
		nodes          = ui_nodes_group_stack->buffer[i]->canvas->nodes;
	}
	parents.length = depth;
	*id            = make_material_node_preview_id(node, &parents);
	return any_map_get(make_image_node_states, *id);
}

// Captures made since the start (a capture is a render, a readback and a hash of the node's inputs)
i32 make_image_node_capture_count() {
	return make_image_node_captures;
}

// Test hook for the nodes with a result by reference (the stats natives of the test build): 0 renders of the node since the start,
// 1 time in ms of its last capture that rendered (shader, hash and pass), 2 the pass of that render alone; both to completion of the GPU
// when ARMORPAINT_IMAGE_NODE_TRACE is set, else to the submission. -1 when the node has no state
f64 make_image_node_gpu_stat(ui_node_t *node, i32 which) {
	char                    *id = NULL;
	make_image_node_state_t *s  = make_image_node_state_of(node, &id);
	if (s == NULL) {
		return -1.0;
	}
	return which == 0 ? (f64)s->gpu_renders : which == 1 ? s->gpu_render_ms : which == 2 ? s->gpu_pass_ms : -1.0;
}

// Regenerate image nodes: captures and bakes every image node of the project on the next update, also the ones whose
// signature did not change (an Image Texture whose file was reloaded)
void make_image_node_regenerate() {
	make_image_node_force_all = true;
	make_image_node_schedule();
}

// Taps per axis of a capture reading the result of the image node at the filter: 1 unless the node is at least twice as large as
// the target of the capture (a larger inner node, e.g. a 4096 Shape into a 512 node, would be point sampled and lose its
// antialiased edge), then size / target, at most 4 (16 taps). Nearest reads (discrete results) and the 1 x 1 capture of a scalar
// input keep their single tap.
static i32 make_image_node_box_taps(ui_node_t *node, i32 filter) {
	i32                dst  = make_image_node_capture_dst;
	image_node_type_t *type = make_image_node_type_of(node->type);
	if (!parser_material_image_space || dst <= 1 || filter == IMAGE_FILTER_NEAREST || type == NULL) {
		return 1;
	}
	i32 source = type->size(node);
	if (source < 2 * dst) {
		return 1;
	}
	i32 n = source / dst;
	return n > 4 ? 4 : n;
}

// Writes the fetch of the node's baked result at the float2 expression uv, once per variable. Repeating: the uv is wrapped
// and snapped to the texel centre, so the one linear sampler never blends two texels. Black until the first bake.
static void make_image_node_write_fetch(char *var, char *tex_name, char *uv, i32 filter) {
	parser_material_write(parser_material_kong, string_tmp("float2 %s_uv = %s;", var, uv));
	if (filter == IMAGE_FILTER_CUBIC) {
		// Cubic B-spline over 4 x 4 texel centres (wrapped, like the bilinear below): C2 and without overshoot, at the cost of 16 fetches
		parser_material_write(parser_material_kong, string_tmp("float2 %s_p = %s_uv * constants.%s_size - 0.5;", var, var, tex_name));
		parser_material_write(parser_material_kong, string_tmp("float2 %s_f = floor(%s_p);", var, var));
		parser_material_write(parser_material_kong, string_tmp("float2 %s_t = %s_p - %s_f;", var, var, var));
		parser_material_write(parser_material_kong, string_tmp("float2 %s_t2 = %s_t * %s_t;", var, var, var));
		parser_material_write(parser_material_kong, string_tmp("float2 %s_t3 = %s_t2 * %s_t;", var, var, var));
		parser_material_write(parser_material_kong, string_tmp("float2 %s_w0 = (1.0 - %s_t) * (1.0 - %s_t) * (1.0 - %s_t) / 6.0;", var, var, var, var));
		parser_material_write(parser_material_kong, string_tmp("float2 %s_w1 = (3.0 * %s_t3 - 6.0 * %s_t2 + 4.0) / 6.0;", var, var, var));
		parser_material_write(parser_material_kong, string_tmp("float2 %s_w2 = (-3.0 * %s_t3 + 3.0 * %s_t2 + 3.0 * %s_t + 1.0) / 6.0;", var, var, var, var));
		parser_material_write(parser_material_kong, string_tmp("float2 %s_w3 = %s_t3 / 6.0;", var, var));
		for (i32 j = 0; j < 4; ++j) {
			for (i32 i = 0; i < 4; ++i) {
				parser_material_write(parser_material_kong, string_tmp("float2 %s_q%d%d = (%s_f + float2(%d.0, %d.0) + 0.5) / constants.%s_size;", var, j, i, var,
				                                                       i - 1, j - 1, tex_name));
				parser_material_write(parser_material_kong,
				                      string_tmp("%s_q%d%d = (floor((%s_q%d%d - floor(%s_q%d%d)) * constants.%s_size) + 0.5) / constants.%s_size;", var, j, i, var, j,
				                                 i, var, j, i, tex_name, tex_name));
			}
			parser_material_write(parser_material_kong,
			                      string_tmp("float4 %s_r%d = sample_lod(%s, sampler_linear, %s_q%d0, 0.0) * %s_w0.x + sample_lod(%s, sampler_linear, %s_q%d1, 0.0) * %s_w1.x + "
			                                 "sample_lod(%s, sampler_linear, %s_q%d2, 0.0) * %s_w2.x + sample_lod(%s, sampler_linear, %s_q%d3, 0.0) * %s_w3.x;",
			                                 var, j, tex_name, var, j, var, tex_name, var, j, var, tex_name, var, j, var, tex_name, var, j, var));
		}
		parser_material_write(parser_material_kong,
		                      string_tmp("float4 %s = %s_r0 * %s_w0.y + %s_r1 * %s_w1.y + %s_r2 * %s_w2.y + %s_r3 * %s_w3.y;", var, var, var, var, var, var, var,
		                                 var, var));
		return;
	}
	if (filter == IMAGE_FILTER_LINEAR) {
		// Bilinear by hand: four fetches, each exactly at a texel centre of the wrapped position, so the linear sampler
		// never blends two texels itself and the blend wraps at the border
		parser_material_write(parser_material_kong, string_tmp("float2 %s_p = %s_uv * constants.%s_size - 0.5;", var, var, tex_name));
		parser_material_write(parser_material_kong, string_tmp("float2 %s_f = floor(%s_p);", var, var));
		parser_material_write(parser_material_kong, string_tmp("float2 %s_t = %s_p - %s_f;", var, var, var));
		for (i32 k = 0; k < 4; ++k) {
			char *off = k == 0 ? "float2(0.0, 0.0)" : k == 1 ? "float2(1.0, 0.0)" : k == 2 ? "float2(0.0, 1.0)" : "float2(1.0, 1.0)";
			parser_material_write(parser_material_kong, string_tmp("float2 %s_q%d = (%s_f + %s + 0.5) / constants.%s_size;", var, k, var, off, tex_name));
			parser_material_write(parser_material_kong, string_tmp("%s_q%d = (floor((%s_q%d - floor(%s_q%d)) * constants.%s_size) + 0.5) / constants.%s_size;", var,
			                                                       k, var, k, var, k, tex_name, tex_name));
			parser_material_write(parser_material_kong, string_tmp("float4 %s_s%d = sample_lod(%s, sampler_linear, %s_q%d, 0.0);", var, k, tex_name, var, k));
		}
		parser_material_write(parser_material_kong, string_tmp("float4 %s = lerp(lerp(%s_s0, %s_s1, %s_t.x), lerp(%s_s2, %s_s3, %s_t.x), %s_t.y);", var, var, var,
		                                                       var, var, var, var, var));
		return;
	}
	parser_material_write(parser_material_kong, string_tmp("%s_uv = (floor((%s_uv - floor(%s_uv)) * constants.%s_size) + 0.5) / constants.%s_size;", var, var,
	                                                       var, tex_name, tex_name));
	parser_material_write(parser_material_kong, string_tmp("float4 %s = sample_lod(%s, sampler_linear, %s_uv, 0.0);", var, tex_name, var));
}

// Records the dependency of a capture on the node, binds its result texture and size constant; returns the texture name,
// NULL when the capture reads the node as black because they depend on each other
static char *make_image_node_bind(ui_node_t *node, char *id) {
	if (make_image_node_capturing != NULL && string_array_index_of(make_image_node_capturing->upstream, id) == -1) {
		any_array_push(make_image_node_capturing->upstream, string_copy(id)); // Chained: bakes before the node being captured
	}
	if (make_image_node_capturing != NULL && make_image_node_edge_cyclic(make_image_node_capturing, id)) {
		make_image_node_tell_cycle(make_image_node_capturing, id);
		return NULL;
	}
	char *tex_name = string_tmp("teximage_%s", id);
	node_shader_add_texture(parser_material_kong, tex_name, string_tmp("_%s", tex_name));
	node_shader_add_constant(parser_material_kong, string_tmp("float2 %s_size", tex_name), string_tmp("_size(_%s)", tex_name));
	return tex_name;
}

// The fetch of the node's baked result at the current texture coordinate (the triplanar projections blended like
// Image Texture), once per node; returns the float4 variable. Image nodes have no Vector input: the placement on the
// uv is done by a Tile UV node at the end of the chain (make_image_node_sample_at). In image space (a capture) tex_coord
// is the image uv, so chained nodes read each other's result 1:1.
char *make_image_node_sample(ui_node_t *node) {
	char *id  = make_material_node_preview_id(node, NULL);
	char *var = string_tmp("%s_image", parser_material_store_var_name(node));
	char *tex_name = make_image_node_bind(node, id);
	if (string_array_index_of(parser_material_parsed, var) >= 0) {
		return var;
	}
	any_array_push(parser_material_parsed, var);
	if (tex_name == NULL) {
		parser_material_write(parser_material_kong, string_tmp("float4 %s = float4(0.0, 0.0, 0.0, 1.0);", var));
		return var;
	}

	image_node_type_t *type   = make_image_node_type_of(node->type);
	// Smooth results are read cubic on the mesh; a capture (chained nodes, image space) reads 1:1 and stays bilinear
	i32 filter = type != NULL && type->smooth_sample ? (parser_material_image_space ? IMAGE_FILTER_LINEAR : IMAGE_FILTER_CUBIC) : IMAGE_FILTER_NEAREST;
	if (parser_material_triplanar && !parser_material_image_space) { // Like Image Texture: one fetch per projection, blended
		node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
		make_image_node_write_fetch(string_tmp("%s0", var), tex_name, "tex_coord", filter);
		make_image_node_write_fetch(string_tmp("%s1", var), tex_name, "tex_coord1", filter);
		make_image_node_write_fetch(string_tmp("%s2", var), tex_name, "tex_coord2", filter);
		parser_material_write(parser_material_kong,
		                      string_tmp("float4 %s = %s0 * tex_coord_blend.x + %s1 * tex_coord_blend.y + %s2 * tex_coord_blend.z;", var, var, var, var));
	}
	else {
		if (!parser_material_image_space) {
			node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
		}
		i32 n = make_image_node_box_taps(node, filter);
		if (n > 1) {
			// The source is at least twice as large as the target: n x n taps spread over the footprint of one target pixel (a box
			// filter), each one hardware bilinear at the wrapped uv, one statement per tap (a shader block is limited in statements;
			// the hand written wrapping bilinear of make_image_node_write_fetch is about 15 statements)
			for (i32 j = 0; j < n; ++j) {
				for (i32 i = 0; i < n; ++i) {
					char *uv = string_tmp("tex_coord + float2(%f, %f) / %f", ((i + 0.5) / n - 0.5), ((j + 0.5) / n - 0.5), (f64)make_image_node_capture_dst);
					parser_material_write(parser_material_kong, string_tmp("float2 %s_u%d = %s;", var, j * n + i, uv));
					parser_material_write(parser_material_kong, string_tmp("float4 %s_b%d = sample_lod(%s, sampler_linear, %s_u%d - floor(%s_u%d), 0.0);", var, j * n + i, tex_name, var, j * n + i, var, j * n + i));
				}
			}
			char *sum = string_tmp("%s_b0", var);
			for (i32 k = 1; k < n * n; ++k) {
				sum = string_tmp("%s + %s_b%d", sum, var, k);
			}
			parser_material_write(parser_material_kong, string_tmp("float4 %s = (%s) / %d.0;", var, sum, n * n));
		}
		else {
			make_image_node_write_fetch(var, tex_name, "tex_coord", filter);
		}
	}
	return var;
}

// The fetch of the node's baked result at the float2 expression uv (repeating), once per node; returns the float4
// variable. Used by Tile UV, which places its own result on the uv. filter: IMAGE_FILTER_NEAREST, _LINEAR or _CUBIC.
char *make_image_node_sample_at(ui_node_t *node, char *uv, i32 filter) {
	char *id  = make_material_node_preview_id(node, NULL);
	char *var = string_tmp("%s_tile", parser_material_store_var_name(node));
	char *tex_name = make_image_node_bind(node, id);
	if (string_array_index_of(parser_material_parsed, var) >= 0) {
		return var;
	}
	any_array_push(parser_material_parsed, var);
	if (tex_name == NULL) {
		parser_material_write(parser_material_kong, string_tmp("float4 %s = float4(0.0, 0.0, 0.0, 1.0);", var));
		return var;
	}
	make_image_node_write_fetch(var, tex_name, uv, filter);
	return var;
}

// Body row of an image node, after its settings: Baking on the highlight color while a bake is pending, Baked once
// the result is there, empty before the first bake. The node has no Bake button: it bakes on its own.
static void make_image_node_state_row(i32 node_id) {
	ui_node_t *node = ui_get_node(ui_nodes_get_canvas(true)->nodes, node_id);
	if (node == NULL) {
		return;
	}
	char                    *id = NULL;
	make_image_node_state_t *s  = make_image_node_state_of(node, &id);
	if (s != NULL && (s->pending || s->needs_capture || make_image_node_force_all)) {
		ui_text(tr("Baking"), UI_ALIGN_CENTER, g_theme->HIGHLIGHT_COL);
		return;
	}
	bool baked = s != NULL && s->has_baked && g_context->node_previews != NULL && any_map_get(g_context->node_previews, id) != NULL;
	i32  _TEXT_COL    = g_theme->TEXT_COL;
	g_theme->TEXT_COL = g_theme->LABEL_COL;
	ui_text(baked ? tr("Baked") : "", UI_ALIGN_CENTER, 0);
	g_theme->TEXT_COL = _TEXT_COL;
}

// Value of a setting of the node (the default value of button i)
f32 make_image_node_button(ui_node_t *node, i32 i) {
	if (i < 0 || i >= node->buttons->length) {
		return make_image_node_def_button_default(node, i); // A node saved with fewer settings than its definition (see make_image_node_reconcile)
	}
	return node->buttons->buffer[i]->default_value->buffer[0];
}

// Width and height of the image: the Size setting (button) 512, 1024, 2048 or 4096
i32 make_image_node_size(ui_node_t *node, i32 button) {
	i32 i = (i32)make_image_node_button(node, button);
	return 512 << (i < 0 ? 0 : i > 3 ? 3 : i);
}

// Socket of an image node definition
ui_node_socket_t *make_image_node_socket(char *name, char *type, u32 color, f32_array_t *default_value, f32 min, f32 max, f32 precision) {
	return ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                     .node_id       = 0,
	                                     .name          = name,
	                                     .type          = type,
	                                     .color         = color,
	                                     .default_value = default_value,
	                                     .min           = min,
	                                     .max           = max,
	                                     .precision     = precision,
	                                     .display       = 0});
}

// The Vector input of a pass-through generator (Gradient Generator, Shape Generator) and of Tile UV: a VECTOR socket named Vector, purple, default 0, 0, 0.
// Appended LAST in the input list of the definition (Tile UV precedent), so the indices of the other inputs of an older save stay valid.
// Baked image nodes (they work in image space on an Image input) have none. Unlinked it is the image uv, see make_image_node_coord.
ui_node_socket_t *make_image_node_vector_socket() {
	return make_image_node_socket(_tr("Vector"), "VECTOR", NODES_MATERIAL_VECTOR_COLOR, f32_array_create_xyz(0.0, 0.0, 0.0), 0.0, 1.0, 100);
}

// ---- Coordinate of a pass-through generator (block extracted by the host test automation/host/image_node_coord) ----
// One rule for the coordinate generators: the pattern is a function of a float2 coordinate that comes from the Vector input.
//   unlinked Vector: the image uv, tex_coord (the uv of the mesh in a material, the image uv of a capture): NOT upstream's bposition, so the picture
//                    of a node without a link is the one it always drew;
//   linked Vector:   the xy of that expression (Mapping, Texture Coordinate UV / Generated / Object: a 3D vector uses xy).
//   Wrap Edges on:   the result repeats across the border: the coordinate is reduced with x - floor(x) (a Mapping scale of 3 gives 3 copies);
//   Wrap Edges off:  the raw coordinate (Blender style, nothing repeats).
// Both are returned: a derivative (the pixel size of the Shape Generator) must be taken of the UNWRAPPED coordinate, the wrapped one jumps at the seam.
// A linked Vector is parsed once and copied to a variable (the linked graph is not repeated in the text).

// The coordinate of one projection from `source` (a float2/float3 expression with .x and .y); name is the prefix of the variables written
static char *make_image_node_coord_of(char *name, char *source, bool linked, bool wrap, char **unwrapped) {
	char *raw = source;
	if (linked) {
		raw = string_tmp("%s_uv", name);
		if (string_array_index_of(parser_material_parsed, raw) == -1) {
			any_array_push(parser_material_parsed, raw);
			parser_material_write(parser_material_kong, string_tmp("float2 %s = %s.xy;", raw, source));
		}
	}
	if (unwrapped != NULL) {
		*unwrapped = raw;
	}
	if (!wrap) {
		return raw;
	}
	char *w = string_tmp("%s_uvw", name);
	if (string_array_index_of(parser_material_parsed, w) == -1) {
		any_array_push(parser_material_parsed, w);
		parser_material_write(parser_material_kong, string_tmp("float2 %s = float2(%s.x - floor(%s.x), %s.y - floor(%s.y));", w, raw, raw, raw, raw));
	}
	return w;
}

static ui_node_socket_t *make_image_node_vector_input(ui_node_t *node, i32 vector_input) {
	// A node saved with fewer inputs than its definition (an older save, see make_image_node_reconcile): the Vector is unlinked
	return vector_input >= 0 && vector_input < node->inputs->length ? node->inputs->buffer[vector_input] : NULL;
}

// The float2 coordinate expression of the node as a shader text, see above. Out: the unwrapped coordinate (may be NULL).
char *make_image_node_coord(ui_node_t *node, i32 vector_input, bool wrap, char **unwrapped) {
	ui_node_socket_t *vector = make_image_node_vector_input(node, vector_input);
	char             *name   = parser_material_store_var_name(node);
	if (vector != NULL && parser_material_get_input_link(vector) != NULL) {
		return make_image_node_coord_of(name, parser_material_parse_vector_input(vector), true, wrap, unwrapped);
	}
	if (!parser_material_image_space) {
		node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
	}
	return make_image_node_coord_of(name, "tex_coord", false, wrap, unwrapped);
}

// Evaluates the pattern of the node (value_at: a float expression for a given coordinate) and returns the name of the float variable that holds it,
// written once under `store`. value_at(node, uv, uv_unwrapped, store) gets the coordinate and its unwrapped form (for derivatives) and the name to
// prefix its own variables with. Unlinked Vector under triplanar projection (a mesh, not a capture): one evaluation per projection (tex_coord,
// tex_coord1, tex_coord2) blended with tex_coord_blend, like Image Texture. A linked Vector is one expression and is evaluated once, as upstream does.
char *make_image_node_eval_coord(ui_node_t *node, i32 vector_input, bool wrap, char *store, char *(*value_at)(ui_node_t *node, char *uv, char *uv_unwrapped, char *store)) {
	if (string_array_index_of(parser_material_parsed, store) >= 0) {
		return store;
	}
	any_array_push(parser_material_parsed, store);
	ui_node_socket_t *vector = make_image_node_vector_input(node, vector_input);
	bool              linked = vector != NULL && parser_material_get_input_link(vector) != NULL;
	if (!linked && parser_material_triplanar && !parser_material_image_space) {
		node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
		char *sources[3] = {"tex_coord", "tex_coord1", "tex_coord2"};
		for (i32 k = 0; k < 3; ++k) {
			char *part      = string_tmp("%s%d", store, k);
			char *unwrapped = NULL;
			char *uv        = make_image_node_coord_of(part, sources[k], false, wrap, &unwrapped);
			parser_material_write(parser_material_kong, string_tmp("float %s = %s;", part, value_at(node, uv, unwrapped, part)));
		}
		parser_material_write(parser_material_kong, string_tmp("float %s = %s0 * tex_coord_blend.x + %s1 * tex_coord_blend.y + %s2 * tex_coord_blend.z;", store, store, store, store));
		return store;
	}
	char *unwrapped = NULL;
	char *uv        = make_image_node_coord(node, vector_input, wrap, &unwrapped);
	parser_material_write(parser_material_kong, string_tmp("float %s = %s;", store, value_at(node, uv, unwrapped, store)));
	return store;
}
// ---- end of the coordinate block ----

// Setting (button) of an image node definition, data holds the items of an ENUM
ui_node_button_t *make_image_node_setting(char *name, char *type, f32 default_value, char *data) {
	return ALLOC_INIT(ui_node_button_t, {.name          = name,
	                                     .type          = type,
	                                     .output        = -1,
	                                     .default_value = f32_array_create_x(default_value),
	                                     .data          = data != NULL ? u8_array_create_from_string(data) : NULL,
	                                     .min           = 0.0,
	                                     .max           = 1.0,
	                                     .precision     = 100,
	                                     .height        = 0});
}

// Bilinear sample of a single channel size x size image at pixel position (x, y), wrapping
f32 make_image_node_sample_bilinear(f32 *img, i32 size, f32 x, f32 y) {
	f32 fx = floorf(x);
	f32 fy = floorf(y);
	f32 tx = x - fx;
	f32 ty = y - fy;
	i32 x0 = ((i32)fx % size + size) % size;
	i32 y0 = ((i32)fy % size + size) % size;
	i32 x1 = (x0 + 1) % size;
	i32 y1 = (y0 + 1) % size;
	f32 a  = img[(size_t)y0 * size + x0] * (1.0f - tx) + img[(size_t)y0 * size + x1] * tx;
	f32 b  = img[(size_t)y1 * size + x0] * (1.0f - tx) + img[(size_t)y1 * size + x1] * tx;
	return a * (1.0f - ty) + b * ty;
}

// The state row button of an image node definition, placed right after the Size setting (Size first, then the state row, then the other settings)
ui_node_button_t *make_image_node_state_button() {
	return ALLOC_INIT(ui_node_button_t, {.name          = "make_image_node_state_row",
	                                     .type          = "CUSTOM",
	                                     .output        = -1,
	                                     .default_value = f32_array_create_x(0),
	                                     .data          = NULL,
	                                     .min           = 0.0,
	                                     .max           = 1.0,
	                                     .precision     = 100,
	                                     .height        = 1});
}

// Registers the sections of an image node type. Every row of the node is in a section, in this order of the list (= flag bits 8, 9, ...):
// 0 Texture: the Size setting and the state row, the first two buttons of every image node, closed by default. It is drawn LAST, below the
//   inputs and the other sections (after_inputs): the list index and the flag bit stay 0, only the place on the node differs (decided 2026-11-20).
// 1 General: a mixed section (one header) with all the other buttons and the first n_general_inputs inputs, open by default.
// then the own sections (own, may be NULL) of the node: ranges of the other inputs; without own sections the remaining inputs
// (from n_general_inputs on) form one open section Parameters.
// n_general_inputs < 0 leaves the node without General and Parameters (Seamless: Image and Blend Width stay plain rows).
// The ranges are by index in the node def, so the button and input order of the def must be: Size, state row, settings; general inputs first.
void make_image_node_sections(ui_node_t *def, ui_node_section_t *own, i32 own_count, i32 n_general_inputs) {
	i32                n_buttons = def->buttons->length;
	i32                n_inputs  = def->inputs->length;
	bool               general   = n_general_inputs >= 0;
	bool               params    = general && own_count == 0 && n_inputs > n_general_inputs;
	ui_node_section_t *list      = calloc(own_count + 4, sizeof(ui_node_section_t));
	i32                n         = 0;
	list[n++]                    = (ui_node_section_t){.title = _tr("Texture"), .kind = UI_NODE_ITEM_BUTTON, .first = 0, .count = 2, .closed_by_default = true, .after_inputs = true};
	if (general) {
		list[n++] = (ui_node_section_t){.title             = _tr("General"),
		                                .kind              = UI_NODE_ITEM_BUTTON,
		                                .first             = 2,
		                                .count             = n_buttons - 2,
		                                .closed_by_default = false,
		                                .inputs_first      = 0,
		                                .inputs_count      = n_general_inputs};
	}
	for (i32 i = 0; i < own_count; ++i) {
		list[n++] = own[i];
	}
	if (params) {
		list[n++] = (ui_node_section_t){.title = _tr("Parameters"), .kind = UI_NODE_ITEM_INPUT, .first = n_general_inputs, .count = n_inputs - n_general_inputs, .closed_by_default = false};
	}
	any_map_set(ui_nodes_sections, def->type, list); // The list ends with the zeroed entry
}

#ifdef ARMORPAINT_TEST_NATIVES
// Test hook of the GPU pass helper: passes of 16 x 16 RGBA128 over a ping-pong pair, two targets written at once. A first pass draws
// a ramp (u, v, 0.25, 1) from a uniform and a constant 0.5 texture; two passes read the previous result (in0) and the constant (in1) and write
// out0 = in0 * 2 + (0.125, 0, 0, 0) + in1 (uniforms float s, float4 o) and out1 = (1 - in0.r, in0.g, in0.b, 1). Returns the value at
// pixel (x, y), channel 0 to 3, of which = 0: the ping-pong result after the second pass, 1: the second target of that pass, 2: the
// ping-pong result after the first pass; -1 for an argument out of range, -2 when a texture could not be read.
f64 make_image_node_test_gpu_pass(i32 which, i32 x, i32 y, i32 channel) {
	const i32 size = 16;
	if (which < 0 || which > 2 || x < 0 || y < 0 || x >= size || y >= size || channel < 0 || channel > 3) {
		return -1.0;
	}
	char             *one[]           = {"RGBA128"};
	char             *two[]           = {"RGBA128", "RGBA128"};
	char             *ramp_uniforms[] = {"float4 k"};
	char             *step_inputs[]   = {"in0", "in1"};
	char             *step_uniforms[] = {"float s", "float4 o"};
	shader_context_t *ramp = make_image_node_gpu_shader("output = float4(tex_coord.x, tex_coord.y, constants.k.z, 1.0);", NULL, 0, ramp_uniforms, 1, one, 1);
	shader_context_t *half = make_image_node_gpu_shader("output = float4(0.5, 0.5, 0.5, 1.0);", NULL, 0, NULL, 0, one, 1);
	shader_context_t *step = make_image_node_gpu_shader("float4 a = sample_lod(in0, sampler_linear, tex_coord, 0.0); float4 b = sample_lod(in1, sampler_linear, tex_coord, 0.0); "
	                                                    "output[0] = a * constants.s + constants.o + b; output[1] = float4(1.0 - a.r, a.g, a.b, 1.0);",
	                                                    step_inputs, 2, step_uniforms, 2, two, 2);
	gpu_texture_t *ramp_rt = gpu_create_render_target(size, size, GPU_TEXTURE_FORMAT_RGBA128);
	gpu_texture_t *half_rt = gpu_create_render_target(size, size, GPU_TEXTURE_FORMAT_RGBA128);
	gpu_texture_t *second  = gpu_create_render_target(size, size, GPU_TEXTURE_FORMAT_RGBA128);
	f32            k[4]    = {0.0f, 0.0f, 0.25f, 0.0f};
	make_image_node_gpu_pass(ramp, &ramp_rt, 1, NULL, 0, k);
	make_image_node_gpu_pass(half, &half_rt, 1, NULL, 0, NULL);
	make_image_node_ping_pong_t pp = make_image_node_ping_pong_create(size, GPU_TEXTURE_FORMAT_RGBA128);
	f32                         u[5] = {2.0f, 0.125f, 0.0f, 0.0f, 0.0f};
	gpu_texture_t              *read = NULL;
	for (i32 pass = 0; pass < 2; ++pass) {
		gpu_texture_t *src        = pass == 0 ? ramp_rt : make_image_node_ping_pong_src(&pp);
		gpu_texture_t *targets[2] = {make_image_node_ping_pong_dst(&pp), second};
		gpu_texture_t *inputs[2]  = {src, half_rt};
		make_image_node_gpu_pass(step, targets, 2, inputs, 2, u);
		make_image_node_ping_pong_swap(&pp);
		if (pass == 0 && which == 2) {
			read = make_image_node_ping_pong_src(&pp);
			break;
		}
	}
	read          = read != NULL ? read : which == 1 ? second : make_image_node_ping_pong_src(&pp);
	buffer_t *b   = gpu_get_texture_pixels(read);
	f64       v   = b != NULL ? ((f32 *)b->buffer)[((size_t)y * size + x) * 4 + channel] : -2.0;
	make_image_node_gpu_shader_free(ramp);
	make_image_node_gpu_shader_free(half);
	make_image_node_gpu_shader_free(step);
	gpu_delete_texture(ramp_rt);
	gpu_delete_texture(half_rt);
	gpu_delete_texture(second);
	make_image_node_ping_pong_free(&pp, NULL);
	return v;
}
// A test only image node (TEX_TEST_GPU_BAKE, registered by make_image_node_test_register) that uses the multi capture gpu_bake path: capture 0 is the Image input
// (RGBA32F at Size), capture 1 a constant 0.25 (R32F, 8 x 8); gpu_bake runs two passes over a ping-pong pair: pass 1 writes in0 * 2 + in1, pass 2 reads it and
// writes it times the uniform 0.5, so the result is Image + 0.125 (exact in float). The node is not in any menu.
static char *make_image_node_test_bake_capture(ui_node_t *node) {
	if (make_image_node_capture_index() == 1) {
		return "float4(0.25, 0.25, 0.25, 1.0)";
	}
	char *v = parser_material_parse_value_input(node->inputs->buffer[0], false);
	return string_tmp("float4(%s, %s, %s, 1.0)", v, v, v);
}

static i32 make_image_node_test_bake_size(ui_node_t *node) {
	return make_image_node_size(node, 0);
}

static i32 make_image_node_test_bake_count(ui_node_t *node) {
	return 2;
}

static i32 make_image_node_test_bake_capture_size(ui_node_t *node, i32 k) {
	return k == 1 ? 8 : 0;
}

static i32 make_image_node_test_bake_capture_format(ui_node_t *node, i32 k) {
	return k == 1 ? IMAGE_CAPTURE_R32F : IMAGE_CAPTURE_RGBA32F;
}

static gpu_texture_t *make_image_node_test_bake_run(ui_node_t *node, i32 size, void *params, gpu_texture_t **captures, i32 count) {
	char *one[]     = {"RGBA128"};
	char *inputs2[] = {"in0", "in1"};
	char *inputs1[] = {"in0"};
	char *uniforms[] = {"float s"};
	shader_context_t *first  = make_image_node_gpu_shader("output = float4(sample_lod(in0, sampler_linear, tex_coord, 0.0).r * 2.0 + sample_lod(in1, sampler_linear, tex_coord, 0.0).r, 0.0, 0.0, 1.0);",
	                                                      inputs2, 2, NULL, 0, one, 1);
	shader_context_t *second = make_image_node_gpu_shader("output = float4(sample_lod(in0, sampler_linear, tex_coord, 0.0).r * constants.s, 0.0, 0.0, 1.0);", inputs1, 1, uniforms, 1, one, 1);
	make_image_node_ping_pong_t pp = make_image_node_ping_pong_create(size, GPU_TEXTURE_FORMAT_RGBA128);
	gpu_texture_t *targets1[1]     = {make_image_node_ping_pong_dst(&pp)};
	make_image_node_gpu_pass(first, targets1, 1, captures, 2, NULL);
	make_image_node_ping_pong_swap(&pp);
	gpu_texture_t *targets2[1] = {make_image_node_ping_pong_dst(&pp)};
	gpu_texture_t *inputs[1]   = {make_image_node_ping_pong_src(&pp)};
	f32            half        = 0.5f;
	make_image_node_gpu_pass(second, targets2, 1, inputs, 1, &half);
	gpu_texture_t *result = targets2[0];
	make_image_node_gpu_shader_free(first);
	make_image_node_gpu_shader_free(second);
	make_image_node_ping_pong_free(&pp, result);
	return result;
}

static image_node_type_t make_image_node_test_bake_type = {
    .type                = "TEX_TEST_GPU_BAKE",
    .result_format       = GPU_TEXTURE_FORMAT_RGBA128,
    .result_by_reference = true,
    .params_size         = 0,
    .size                = make_image_node_test_bake_size,
    .capture             = make_image_node_test_bake_capture,
    .capture_count       = make_image_node_test_bake_count,
    .capture_size        = make_image_node_test_bake_capture_size,
    .capture_format_at   = make_image_node_test_bake_capture_format,
    .gpu_bake            = make_image_node_test_bake_run,
};

static char *make_image_node_test_bake_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

// Registers TEX_TEST_GPU_BAKE (once): the node is created with script_material_create_node_at. Input 0 Image (VALUE), output 0 Image, buttons Size and the state row.
void make_image_node_test_register() {
	static bool registered = false;
	if (registered) {
		return;
	}
	registered = true;
	ui_node_t *def = ALLOC_INIT(ui_node_t, {.id      = 0,
	                                         .name    = "Test GPU Bake",
	                                         .type    = "TEX_TEST_GPU_BAKE",
	                                         .x       = 0,
	                                         .y       = 0,
	                                         .color   = NODES_MATERIAL_IMAGE_COLOR,
	                                         .inputs  = any_array_create_from_raw((void *[]){make_image_node_socket("Image", "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.5), 0.0, 1.0, 100)}, 1),
	                                         .outputs = any_array_create_from_raw((void *[]){make_image_node_socket("Image", "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100)}, 1),
	                                         .buttons = any_array_create_from_raw((void *[]){make_image_node_setting("Size", "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS), make_image_node_state_button()}, 2),
	                                         .width   = 0,
	                                         .flags   = 0});
	any_array_push(nodes_material_pattern, def);
	any_map_set(parser_material_node_values, "TEX_TEST_GPU_BAKE", make_image_node_test_bake_value);
	make_image_node_register(&make_image_node_test_bake_type);
}
#endif
