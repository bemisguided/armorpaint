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
#ifdef ARMORPAINT_TEST_NATIVES
static i32 make_image_node_renders         = 0; // Renders of capture shaders since the start
static f64 make_image_node_readback_bytes  = 0; // Bytes read back since the start
#endif
static i32 make_image_node_capture_dst = 0; // Width of the target of the running capture, see make_image_node_box_taps
static u64 make_image_node_capture_text_hash = 0; // Hash of the text of the shader the last make_image_node_capture_shader built
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
	u8                *pending_pixels; // pending_size * pending_size RGBA8
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
	void              *summary;       // What the type's summarize made of the last bake (type->summary_size bytes), NULL for a type without
#ifdef ARMORPAINT_TEST_NATIVES
	i32                result_count;  // What the type's process returned for the last bake (Flood Fill: islands), for the test natives
	f64                stat_capture_ms;      // Time of the last capture (renders, readbacks, params, hash), see make_image_node_stat
	f64                stat_capture_bytes;   // Bytes of its captures (what process gets)
	f64                stat_readback_bytes;  // Bytes read back by all its renders, a linked scalar input included
	i32                stat_renders;         // Renders of the last capture, a linked scalar input included (each compiles a shader)
	f64                stat_bake_ms;         // Time of the last bake (process and upload)
#endif
	// Pass-through with capture (image_node_type_t.pass_through_capture): the capture render target is node_previews[id]
	bool               capture_valid;   // The target holds the render of the shader with capture_hash
	u64                capture_hash;    // Hash of the text of the shader that rendered it
	bool               force_capture;   // An image node it samples changed: render again even when the text is the same
	bool               direct;          // The image input is a baked node's result bound directly: no capture of its own
	i32                capture_renders; // Renders of the capture since the state exists (make_image_node_pass_through_info)
	i32                capture_skips;   // Captures that found the shader text unchanged and kept the target
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
static i32                      make_image_node_capture_input   = 0; // Input of make_image_node_input_value
static i32                      make_image_node_capture_k       = 0; // Capture of a type with several, see make_image_node_capture_index
static void                    *make_image_node_capture_params_ptr = NULL; // See make_image_node_capture_params
static u64                      make_image_node_capture_skip_hash = 0; // Capture shader text hash that needs no render (0: always render), see make_image_node_capture_shader

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
	s->pending_pixels = NULL;
	s->pending_bytes  = 0;
	s->pending_params = NULL;
	s->pending        = false;
}

static void make_image_node_free_state(make_image_node_state_t *s) {
	make_image_node_clear_pending(s);
	free(s->summary);
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

// 64 bit signature over the captured pixels (bytes), the size and the processing parameters: xxHash64 chained over the three
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

// Shader evaluating expr(node) over the image square: a full screen triangle with tex_coord = the image uv,
// [0, 1] across the target, no mesh (parser_material_image_space gives mesh based inputs a neutral value).
// The node writes the expression while make_image_node_capturing is set. The hash of the shader text is left in
// make_image_node_capture_text_hash; when make_image_node_capture_skip_hash is set and equals it NULL is returned without
// compiling (the pass-through captures keep their render when the text did not change).
static shader_context_t *make_image_node_capture_shader(ui_node_t *node, char *(*expr)(ui_node_t *), char *format) {
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
	                                                             .color_attachments = any_array_create_from_raw(
	                                                                 (void *[]){
	                                                                     format,
	                                                                 },
	                                                                 1)});
	material_t            *data  = ALLOC_INIT(material_t, {.name = "Material", .canvas = g_context->material->canvas});
	node_shader_context_t *con   = node_shader_context_create(data, props);
	node_shader_t         *kong  = node_shader_context_make_kong(con);
	// util_render_screen_aligned_full_vb: one triangle covering the target, uv (0, 0) at the top left like the paint uvs
	node_shader_write_attrib_vert(kong, "output.pos = float4(input.pos.xy * 3.0, 0.0, 1.0);");
	node_shader_add_out(kong, "float2 tex_coord");
	node_shader_write_attrib_vert(kong, "output.tex_coord = float2(output.pos.x * 0.5 + 0.5, 0.5 - output.pos.y * 0.5);");
	node_shader_write_attrib_frag(kong, "float2 tex_coord = input.tex_coord;");

	parser_material_init();
	make_image_node_parser_scope();
	parser_material_con                   = con;
	parser_material_kong                  = kong;
	parser_material_image_space           = true;
	parser_material_transform_color_space = false;
	char *res                             = expr(node);
	parser_material_transform_color_space = true;
	parser_material_image_space           = false;

	kong->frag_out = "float4";
	node_shader_write_frag(kong, string_tmp("output = %s;", res));
	parser_material_finalize(con);
	con->data->shader_from_source = true;
	char *text = node_shader_get(kong);
	make_image_node_capture_text_hash = make_image_node_hash(0, text, string_length(text));
	if (make_image_node_capture_skip_hash != 0 && make_image_node_capture_text_hash == make_image_node_capture_skip_hash) {
		// Same text as the render the caller kept: no compile, no render
		node_shader_context_free(con);
		free(data);
		return NULL;
	}
	gpu_create_shaders_from_kong(text, &con->data->vertex_shader, &con->data->fragment_shader, &con->data->_->vertex_shader_size,
	                             &con->data->_->fragment_shader_size);
	shader_context_load(con->data);
	shader_context_t *scon = con->data;
	node_shader_context_free(con);
	free(data);
	return scon;
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

// The format name of the shader context (render_path_get_tex_format)
static char *make_image_node_capture_format_name(i32 format) {
	switch (format) {
	case IMAGE_CAPTURE_RGBA32F:
		return "RGBA128";
	case IMAGE_CAPTURE_R32F:
		return "R32";
	case IMAGE_CAPTURE_R16F:
		return "R16";
	case IMAGE_CAPTURE_R8:
		return "R8";
	}
	return "RGBA32";
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

// Draws the capture shader over the whole target (a full screen triangle) and frees the shader context
static void make_image_node_draw_capture(shader_context_t *scon, gpu_texture_t *target) {
	static string_array_t *empty = NULL;
	if (empty == NULL) {
		empty = any_array_create_from_raw(
		    (void *[]){
		        "",
		    },
		    1);
	}
	_gpu_begin(target, NULL, NULL, GPU_CLEAR_COLOR, 0x00000000, 0.0);
	gpu_set_pipeline(scon->_->pipe);
	uniforms_set_context_consts(scon, empty);
	uniforms_set_obj_consts(scon, g_context->paint_object->base); // Time, picker and other constants the graph reads
	gpu_set_vertex_buffer(util_render_screen_aligned_full_vb);
	gpu_set_index_buffer(util_render_screen_aligned_full_ib);
	gpu_draw();
	gpu_end();
	_mesh_object_last_pipeline = NULL; // Mesh draws set their pipeline again
	make_material_delete_context(scon);
}

// Renders expr(node) over the image square into a size x size target of a capture format (IMAGE_CAPTURE_*) and reads it back.
// Returns the pixels (the caller frees them), or NULL when the readback fails.
static u8 *make_image_node_capture_pixels(ui_node_t *node, char *(*expr)(ui_node_t *), i32 size, i32 capture_format) {
	gpu_texture_format_t format = make_image_node_capture_gpu_format(capture_format);
#ifdef ARMORPAINT_TEST_NATIVES
	make_image_node_renders++;
	make_image_node_readback_bytes += (f64)make_image_node_capture_bytes(size, capture_format);
#endif
	make_image_node_capture_dst = size;
	shader_context_t *scon      = make_image_node_capture_shader(node, expr, make_image_node_capture_format_name(capture_format));
	make_image_node_capture_dst = 0;
	if (util_render_screen_aligned_full_vb == NULL) {
		util_render_create_screen_aligned_full_data();
	}
	gpu_texture_t *target = gpu_create_render_target(size, size, format);
	make_image_node_draw_capture(scon, target);

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

// Copies of the items of a definition for a saved node (the shared definition is never used directly). Sockets get free ids in nodes.
static ui_node_socket_t *make_image_node_socket_copy(ui_node_socket_t *d, ui_node_t *node, ui_node_t_array_t *nodes) {
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
	return soc;
}

static ui_node_button_t *make_image_node_button_copy(ui_node_button_t *d) {
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
	return but;
}

// Completes a node saved by an older build with the inputs, outputs and settings its definition has now. A saved node keeps the length it
// was created with, the code indexes by the enums of the definition. The fork only ever appends items at the end of a definition, so the
// missing items are the tail of the definition, appended with its defaults. Links address inputs by index, so they stay valid.
// A saved node with MORE items than its definition (a node type that was rebuilt with fewer, e.g. Noise Generator, a shader node
// with a Vector input and two outputs before it became an image node) has a layout the code cannot read: it is reset to the
// definition (every item default, new socket ids) and the links of the node are removed. Nothing else of the node is kept.
// canvas is the canvas the node is in (new sockets get free ids, the links of a reset node are removed from it). Called when the node is
// traversed for its capture and when the node editor lays it out; cheap when nothing is wrong.
// Returns true when something was changed.
bool make_image_node_reconcile(ui_node_t *node, ui_node_canvas_t *canvas) {
	if (node->inputs == NULL || node->outputs == NULL || node->buttons == NULL) {
		return false;
	}
	ui_node_t *def = nodes_material_get_node_t(node->type);
	if (def == NULL) {
		return false;
	}
	bool longer = node->inputs->length > def->inputs->length || node->outputs->length > def->outputs->length || node->buttons->length > def->buttons->length;
	if (!longer && node->inputs->length == def->inputs->length && node->outputs->length == def->outputs->length && node->buttons->length == def->buttons->length) {
		return false;
	}
	if (longer) {
		i32 i = 0;
		while (i < canvas->links->length) {
			ui_node_link_t *l = canvas->links->buffer[i];
			if (l->from_id == node->id || l->to_id == node->id) {
				for (i32 j = i; j < canvas->links->length - 1; ++j) {
					canvas->links->buffer[j] = canvas->links->buffer[j + 1];
				}
				canvas->links->buffer[canvas->links->length - 1] = NULL;
				canvas->links->length--;
			}
			else {
				i++;
			}
		}
		node->inputs->length  = 0;
		node->outputs->length = 0;
		node->buttons->length = 0;
	}
	for (i32 i = node->inputs->length; i < def->inputs->length; ++i) {
		any_array_push(node->inputs, make_image_node_socket_copy(def->inputs->buffer[i], node, canvas->nodes));
	}
	for (i32 i = node->outputs->length; i < def->outputs->length; ++i) {
		any_array_push(node->outputs, make_image_node_socket_copy(def->outputs->buffer[i], node, canvas->nodes));
	}
	for (i32 i = node->buttons->length; i < def->buttons->length; ++i) {
		any_array_push(node->buttons, make_image_node_button_copy(def->buttons->buffer[i]));
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

// Texel index after the tiling mode (IMAGE_TILING_*), -1 outside the image (None)
static i32 make_image_node_wrap(i32 i, i32 size, i32 tiling) {
	if (i >= 0 && i < size) {
		return i;
	}
	switch (tiling) {
	case IMAGE_TILING_TILE:
		i %= size;
		return i < 0 ? i + size : i;
	case IMAGE_TILING_MIRROR:
		i %= size * 2;
		i = i < 0 ? i + size * 2 : i;
		return i < size ? i : size * 2 - 1 - i;
	case IMAGE_TILING_CLAMP:
		return i < 0 ? 0 : size - 1;
	}
	return -1;
}

static void make_image_node_fetch(f32 *pixels, i32 size, i32 x, i32 y, i32 tiling, f32 *out) {
	x = make_image_node_wrap(x, size, tiling);
	y = make_image_node_wrap(y, size, tiling);
	if (x < 0 || y < 0) {
		out[0] = out[1] = out[2] = out[3] = 0.0f;
		return;
	}
	f32 *s = pixels + ((size_t)y * size + x) * 4;
	out[0] = s[0];
	out[1] = s[1];
	out[2] = s[2];
	out[3] = s[3];
}

// Catmull-Rom weights for the four texels around a fraction t
static void make_image_node_cubic_weights(f32 t, f32 *w) {
	f32 t2 = t * t;
	f32 t3 = t2 * t;
	w[0]   = 0.5f * (-t3 + 2.0f * t2 - t);
	w[1]   = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
	w[2]   = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
	w[3]   = 0.5f * (t3 - t2);
}

// Cpu read of a square RGBA float image (size x size texels) at (u, v), image fractions 0..1, into out (4 floats).
// filter: IMAGE_FILTER_NEAREST, _LINEAR or _CUBIC (Catmull-Rom: a read on a texel centre gives that texel back exactly).
// tiling: IMAGE_TILING_* for the texels outside the image. For the cpu passes of the image nodes (Transform, ...).
void make_image_node_sample_rgba(f32 *pixels, i32 size, f32 u, f32 v, i32 filter, i32 tiling, f32 *out) {
	f32 fx = u * (f32)size;
	f32 fy = v * (f32)size;
	if (filter == IMAGE_FILTER_NEAREST) {
		make_image_node_fetch(pixels, size, (i32)floorf(fx), (i32)floorf(fy), tiling, out);
		return;
	}
	fx -= 0.5f;
	fy -= 0.5f;
	f32 flx = floorf(fx);
	f32 fly = floorf(fy);
	f32 tx  = fx - flx;
	f32 ty  = fy - fly;
	// Float error must not blur a read that lands on a texel centre
	tx      = tx < 1e-4f ? 0.0f : tx > 1.0f - 1e-4f ? 1.0f : tx;
	ty      = ty < 1e-4f ? 0.0f : ty > 1.0f - 1e-4f ? 1.0f : ty;
	i32 ix  = (i32)flx;
	i32 iy  = (i32)fly;
	f32 wx[4];
	f32 wy[4];
	i32 n0;
	if (filter == IMAGE_FILTER_LINEAR) {
		wx[0] = 1.0f - tx;
		wx[1] = tx;
		wy[0] = 1.0f - ty;
		wy[1] = ty;
		n0    = 0;
	}
	else {
		make_image_node_cubic_weights(tx, wx);
		make_image_node_cubic_weights(ty, wy);
		n0 = -1;
	}
	i32 n  = filter == IMAGE_FILTER_LINEAR ? 2 : 4;
	out[0] = out[1] = out[2] = out[3] = 0.0f;
	for (i32 j = 0; j < n; ++j) {
		if (wy[j] == 0.0f) {
			continue;
		}
		for (i32 i = 0; i < n; ++i) {
			if (wx[i] == 0.0f) {
				continue;
			}
			f32 t[4];
			make_image_node_fetch(pixels, size, ix + n0 + i, iy + n0 + j, tiling, t);
			f32 w = wx[i] * wy[j];
			for (i32 k = 0; k < 4; ++k) {
				out[k] += t[k] * w;
			}
		}
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
#ifdef ARMORPAINT_TEST_NATIVES
	f64 stat_t0       = sys_time();
	i32 stat_renders0 = make_image_node_renders;
	f64 stat_bytes0   = make_image_node_readback_bytes;
#endif
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
#ifdef ARMORPAINT_TEST_NATIVES
	s->stat_capture_bytes  = (f64)total;
	s->stat_renders        = make_image_node_renders - stat_renders0;
	s->stat_readback_bytes = make_image_node_readback_bytes - stat_bytes0;
#endif
	s->captured               = true;
	s->captured_canvas        = g_context->material->canvas;
	s->captured_graph_version = util_version_graph(g_context->material);
	s->captured_asset_version = util_version_assets();
	u64 sig = make_image_node_signature(pixels, total, size, params, t->params_size);
#ifdef ARMORPAINT_TEST_NATIVES
	s->stat_capture_ms = (sys_time() - stat_t0) * 1000.0;
#endif

	bool has_result = t->summary_only || (g_context->node_previews != NULL && any_map_get(g_context->node_previews, id) != NULL);
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

// Processes the pending capture of s into its result texture
static void make_image_node_bake(make_image_node_state_t *s, char *id) {
	if (!s->pending || s->type == NULL) {
		return;
	}
	if (g_context->node_previews == NULL) {
		g_context->node_previews = any_map_create();
	}
	image_node_type_t *t      = s->type;
	i32                size   = s->pending_size;
	if (t->summary_only) { // No result texture: only the summary of the capture is kept
		if (s->summary == NULL) {
			s->summary = calloc(1, (size_t)t->summary_size);
		}
		if (s->summary != NULL) {
			t->summarize(s->pending_pixels, size, s->pending_params, s->summary);
		}
		s->has_baked       = true;
		s->baked_signature = s->pending_signature;
		s->failed          = false;
		make_image_node_clear_pending(s);
		return;
	}
	size_t             plane        = (size_t)size * size * 4;
	size_t             result_bytes = plane * (t->second_result ? 2 : 1) * sizeof(f32);
	f32               *result       = make_image_node_plane_alloc(result_bytes);
	i32                status       = -1;
	if (result != NULL) {
		if (t->summarize != NULL && t->summary_size > 0) {
			if (s->summary == NULL) {
				s->summary = calloc(1, (size_t)t->summary_size);
			}
			if (s->summary != NULL) {
				t->summarize(s->pending_pixels, size, s->pending_params, s->summary);
			}
		}
#ifdef ARMORPAINT_TEST_NATIVES
		f64 stat_t0 = sys_time();
#endif
		status = t->process(s->pending_pixels, size, s->pending_params, result);
#ifdef ARMORPAINT_TEST_NATIVES
		s->result_count = status;
		s->stat_bake_ms = (sys_time() - stat_t0) * 1000.0;
#endif
		make_image_node_plane_free(s->pending_pixels, s->pending_bytes); // The capture is not needed during the upload
		free(s->pending_params);
		s->pending_pixels = NULL;
		s->pending_bytes  = 0;
		s->pending_params = NULL;
		if (!make_image_node_upload(id, result, size, t->result_format)) {
			status = -1;
		}
		else if (t->second_result && !make_image_node_upload(string_tmp("%s_2", id), result + plane, size, GPU_TEXTURE_FORMAT_RGBA64)) {
			status = -1;
		}
		make_image_node_plane_free(result, result_bytes);
	}
	if (status < 0) {
		console_error(tr("Image node bake: out of memory"));
		make_image_node_delete_result(id); // Black result, not the previous texture
		make_image_node_delete_result(string_tmp("%s_2", id)); // The second result of a node that has one
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

// ---- Pass-through with capture (image_node_type_t.pass_through_capture, see types.h) ----
// The capture render target of the node is node_previews[preview id], bound by the node's shader like a baked result (_teximage_<id>).

// The baked image node whose result the image input of node is linked straight to (no node between), NULL when the input is not
// linked, is linked to anything else, or to another output than the Image output (output 0, RGBA: a baked node writes the rgb of its
// result there, so the result texture IS what a capture of the input would hold). Reads the links of the parser scope.
static ui_node_t *make_image_node_direct_source(ui_node_t *node, i32 input) {
	if (input < 0 || input >= node->inputs->length) {
		return NULL;
	}
	ui_node_link_t *l    = parser_material_get_input_link(node->inputs->buffer[input]);
	ui_node_t      *from = l != NULL ? parser_material_get_node(l->from_id) : NULL;
	if (from == NULL || l->from_socket != 0 || from->outputs->length == 0 || !string_equals(from->outputs->buffer[0]->type, "RGBA")) {
		return NULL;
	}
	image_node_type_t *t = make_image_node_type_of(from->type);
	return t != NULL && !t->pass_through_capture && t->process != NULL ? from : NULL;
}

// Marks the nodes that sampled the capture of s (through the recorded upstream) to capture again, whatever their shader text
static void make_image_node_invalidate_dependents(make_image_node_state_t *s) {
	string_array_t *keys = make_image_node_states_keys();
	for (i32 i = 0; i < keys->length; ++i) {
		make_image_node_state_t *m = any_map_get(make_image_node_states, keys->buffer[i]);
		if (m != s && m->type != NULL && make_image_node_depends_on(keys->buffer[i], s)) {
			m->needs_capture = true;
			m->force_capture = true;
		}
	}
	make_image_node_free_keys(keys);
}

// Renders the image input of a pass-through node into its capture target, unless nothing it reads changed. Returns true when the
// node's capture changed (rendered, or switched between a capture and a bound baked result), so what samples it must refresh.
// force renders even when the shader text is the same (Regenerate Image Nodes, an image node it samples changed).
static bool make_image_node_capture_pass_through(make_image_node_state_t *s, char *id, ui_node_t *node, ui_node_canvas_t *group, ui_node_t_array_t *parents, bool force) {
	image_node_type_t *t = s->type;
	s->needs_capture     = false;
	force                = force || s->force_capture;
	s->force_capture     = false;
	make_image_node_clear_upstream(s);
	s->cycle_seen                   = false;
	make_image_node_capturing       = s;
	make_image_node_capture_group   = group;
	make_image_node_capture_parents = parents;
	parser_material_init();
	make_image_node_parser_scope();

	bool changed = false;
	if (make_image_node_direct_source(node, t->image_input) != NULL) {
		// The baked result is bound by the node's shader: no capture of its own
		if (g_context->node_previews != NULL && any_map_get(g_context->node_previews, id) != NULL) {
			make_image_node_delete_result(id);
		}
		changed          = !s->direct || s->capture_valid;
		s->direct        = true;
		s->capture_valid = false;
	}
	else {
		i32            size   = t->size(node);
		gpu_texture_t *target = g_context->node_previews != NULL ? any_map_get(g_context->node_previews, id) : NULL;
		bool           keep   = !force && s->capture_valid && !s->direct && target != NULL && target->width == size && s->captured_asset_version == util_version_assets();
		make_image_node_capture_skip_hash = keep ? s->capture_hash : 0;
		make_image_node_capture_dst       = size;
		shader_context_t *scon            = make_image_node_capture_shader(node, t->capture, make_image_node_capture_format_name(t->capture_format));
		make_image_node_capture_dst       = 0;
		make_image_node_capture_skip_hash = 0;
		if (scon == NULL) {
			s->capture_skips++; // Same shader text: the render is still right (a changed parameter of the node is not part of it)
		}
		else {
			if (util_render_screen_aligned_full_vb == NULL) {
				util_render_create_screen_aligned_full_data();
			}
			if (g_context->node_previews == NULL) {
				g_context->node_previews = any_map_create();
			}
			gpu_texture_format_t format = make_image_node_capture_gpu_format(t->capture_format);
			if (target == NULL || target->width != size || target->height != size || target->format != format) {
				if (target != NULL) {
					gpu_delete_texture(target);
				}
				target = gpu_create_render_target(size, size, format);
				any_map_set(g_context->node_previews, string_copy(id), target);
			}
			make_image_node_draw_capture(scon, target);
			s->capture_hash  = make_image_node_capture_text_hash;
			s->capture_valid = true;
			s->direct        = false;
			s->capture_renders++;
			make_image_node_captures++;
			if (make_image_node_trace) {
				iron_log("[image node] pass-through capture %d %s time %.3f", make_image_node_captures, id, sys_time());
			}
			changed = true;
		}
	}
	make_image_node_capturing       = NULL;
	make_image_node_capture_group   = NULL;
	make_image_node_capture_parents = NULL;
	if (!s->cycle_seen) {
		s->cycle_told = false;
	}
	s->captured               = true;
	s->captured_canvas        = g_context->material->canvas;
	s->captured_graph_version = util_version_graph(g_context->material);
	s->captured_asset_version = util_version_assets();
	if (changed) {
		make_image_node_invalidate_dependents(s);
	}
	return changed;
}
// ---- end of pass-through with capture ----

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
			bool pass_through = s->type != NULL && s->type->pass_through_capture;
			bool rendered     = false;
			if (force_all || s->needs_capture || upstream_baked) {
				if (pass_through) {
					rendered = make_image_node_capture_pass_through(s, e->id, e->node, e->group, e->parents, force_all || upstream_baked);
				}
				else {
					make_image_node_capture(s, e->id, e->node, e->group, e->parents, force_all);
				}
				if (!cycle && make_image_node_waits(e, entries)) {
					// The capture just recorded the upstream of a node never captured before: wait for it and capture again.
					// Not progress, so a cycle still trips the cycle flag at the end of a pass without a finished node.
					s->needs_capture = true;
					continue;
				}
			}
			if (rendered) {
				any_array_push(baked, e->id); // What samples the capture captures again, the material refreshes
				if (changed == NULL) {
					changed = e->node;
				}
			}
			else if (s->pending) {
				make_image_node_bake(s, e->id);
				any_array_push(baked, e->id);
				if (changed == NULL && !s->type->summary_only) { // A summary only node changes no shader: no re-parse
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
	make_image_node_reconcile(node, group != NULL ? group : g_context->material->canvas);
	char *id = make_material_node_preview_id(node, parents);
	any_array_push(g_context->node_previews_used, id); // Keeps the baked result
	make_image_node_state_t *s = make_image_node_get_state(id, node);
	if (s->type != NULL && s->type->second_result) {
		any_array_push(g_context->node_previews_used, string_tmp("%s_2", id));
	}
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
	if (s->type != NULL && s->type->pass_through_capture) {
		// Rendered now when the shader of the input changed (nothing to bake, so no debounce); what sampled the old capture captures again
		if (make_image_node_capture_pass_through(s, id, node, group, parents, false)) {
			make_image_node_redraw_nodes();
			if (make_image_node_any_waiting()) {
				make_image_node_schedule();
			}
		}
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

// What the node type's summarize made of the last bake (the Levels histogram), NULL while the node is not baked or has none
void *make_image_node_summary(ui_node_t *node) {
	char                    *id;
	make_image_node_state_t *s = make_image_node_state_of(node, &id);
	return s != NULL && s->has_baked ? s->summary : NULL;
}

#ifdef ARMORPAINT_TEST_NATIVES
// Test driver hooks (minic_test_natives.c)
bool make_image_node_is_pending(ui_node_t *node) {
	char                    *id;
	make_image_node_state_t *s = make_image_node_state_of(node, &id);
	return s != NULL && (s->pending || s->needs_capture || make_image_node_force_all);
}

gpu_texture_t *make_image_node_result(ui_node_t *node) {
	char                    *id;
	make_image_node_state_t *s = make_image_node_state_of(node, &id);
	if (s == NULL || !s->has_baked || g_context->node_previews == NULL) {
		return NULL;
	}
	return any_map_get(g_context->node_previews, id);
}

// Numbers of the node's last capture and bake: 0 capture bytes (what process gets), 1 readback bytes of all its renders, 2 renders,
// 3 capture ms, 4 process ms; -1 for an unknown node or number
f64 make_image_node_stat(ui_node_t *node, i32 which) {
	char                    *id;
	make_image_node_state_t *s = make_image_node_state_of(node, &id);
	if (s == NULL) {
		return -1.0;
	}
	switch (which) {
	case 0:
		return s->stat_capture_bytes;
	case 1:
		return s->stat_readback_bytes;
	case 2:
		return (f64)s->stat_renders;
	case 3:
		return s->stat_capture_ms;
	case 4:
		return s->stat_bake_ms;
	}
	return -1.0;
}

static char *make_image_node_test_expr(ui_node_t *node) {
	return "float4(tex_coord.x, tex_coord.y, 0.25, 1.0)";
}

// Round trip of a capture format for the test natives: renders float4(u, v, 0.25, 1) over a size x size square in the capture format,
// reads it back and returns the value at pixel (x, y) in channel (0 to 3). channel -1 returns the channels of the format,
// -2 the bytes of the capture, -3 the bytes of the next capture after one of this size and format (the 16 byte alignment).
// A channel the format does not have returns -1.
f64 make_image_node_test_capture(i32 format, i32 size, i32 x, i32 y, i32 channel) {
	i32 channels = make_image_node_capture_channels(format);
	if (channel == -1) {
		return (f64)channels;
	}
	if (channel == -2) {
		return (f64)make_image_node_capture_bytes(size, format);
	}
	if (channel == -3) {
		return (f64)make_image_node_capture_next(0, size, format);
	}
	if (channel < 0 || channel >= channels || x < 0 || y < 0 || x >= size || y >= size) {
		return -1.0;
	}
	u8 *p = make_image_node_capture_pixels(NULL, make_image_node_test_expr, size, format);
	if (p == NULL) {
		return -2.0;
	}
	size_t i = (size_t)y * size + x;
	f64    v = 0.0;
	switch (format) {
	case IMAGE_CAPTURE_RGBA8:
		v = p[i * 4 + channel] / 255.0;
		break;
	case IMAGE_CAPTURE_RGBA32F:
		v = ((f32 *)p)[i * 4 + channel];
		break;
	case IMAGE_CAPTURE_R32F:
		v = ((f32 *)p)[i];
		break;
	case IMAGE_CAPTURE_R16F:
		v = render_path_raytrace_half_to_float(((u16 *)p)[i]);
		break;
	case IMAGE_CAPTURE_R8:
		v = p[i] / 255.0;
		break;
	}
	make_image_node_plane_free(p, make_image_node_capture_bytes(size, format));
	return v;
}

// xxHash64 of a result texture readback, for the test natives (script_image_node_result_hash)
u64 make_image_node_result_hash(void *bytes, size_t n) {
	return make_image_node_hash(0, bytes, n);
}

i32 make_image_node_result_count(ui_node_t *node) {
	char                    *id;
	make_image_node_state_t *s = make_image_node_state_of(node, &id);
	return s != NULL && s->has_baked ? s->result_count : -1;
}

void make_image_node_bake_now(ui_node_t *node) {
	make_image_node_regenerate();
}
#endif

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
static char *make_image_node_bind(ui_node_t *node, char *id, char *texture_id) {
	if (make_image_node_capturing != NULL && string_array_index_of(make_image_node_capturing->upstream, id) == -1) {
		any_array_push(make_image_node_capturing->upstream, string_copy(id)); // Chained: bakes before the node being captured
	}
	if (make_image_node_capturing != NULL && make_image_node_edge_cyclic(make_image_node_capturing, id)) {
		make_image_node_tell_cycle(make_image_node_capturing, id);
		return NULL;
	}
	char *tex_name = string_tmp("teximage_%s", texture_id);
	node_shader_add_texture(parser_material_kong, tex_name, string_tmp("_%s", tex_name));
	node_shader_add_constant(parser_material_kong, string_tmp("float2 %s_size", tex_name), string_tmp("_size(_%s)", tex_name));
	return tex_name;
}

// The fetch of the node's baked result at the current texture coordinate (the triplanar projections blended like
// Image Texture), once per node; returns the float4 variable. Image nodes have no Vector input: the placement on the
// uv is done by a Tile UV node at the end of the chain (make_image_node_sample_at). In image space (a capture) tex_coord
// is the image uv, so chained nodes read each other's result 1:1.
static char *make_image_node_sample_texture(ui_node_t *node, char *texture_id, char *var_suffix, i32 filter) {
	char *id       = make_material_node_preview_id(node, NULL);
	char *var      = string_tmp("%s_%s", parser_material_store_var_name(node), var_suffix);
	char *tex_name = make_image_node_bind(node, id, texture_id);
	if (string_array_index_of(parser_material_parsed, var) >= 0) {
		return var;
	}
	any_array_push(parser_material_parsed, var);
	if (tex_name == NULL) {
		parser_material_write(parser_material_kong, string_tmp("float4 %s = float4(0.0, 0.0, 0.0, 1.0);", var));
		return var;
	}

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

char *make_image_node_sample(ui_node_t *node) {
	image_node_type_t *type = make_image_node_type_of(node->type);
	// Smooth results are read cubic on the mesh; a capture (chained nodes, image space) reads 1:1 and stays bilinear
	i32 filter = type != NULL && type->smooth_sample ? (parser_material_image_space ? IMAGE_FILTER_LINEAR : IMAGE_FILTER_CUBIC) : IMAGE_FILTER_NEAREST;
	return make_image_node_sample_texture(node, make_material_node_preview_id(node, NULL), "image", filter);
}

// The same for the second result texture of a node with second_result, read at the nearest texel
char *make_image_node_sample_second(ui_node_t *node) {
	return make_image_node_sample_texture(node, string_tmp("%s_2", make_material_node_preview_id(node, NULL)), "second", IMAGE_FILTER_NEAREST);
}

// The fetch of the node's baked result at the float2 expression uv (repeating), once per node; returns the float4
// variable. Used by Tile UV, which places its own result on the uv. filter: IMAGE_FILTER_NEAREST, _LINEAR or _CUBIC.
char *make_image_node_sample_at(ui_node_t *node, char *uv, i32 filter) {
	char *id  = make_material_node_preview_id(node, NULL);
	char *var = string_tmp("%s_tile", parser_material_store_var_name(node));
	char *tex_name = make_image_node_bind(node, id, id);
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

// Pass-through with capture: the fetch of the node's image input at the float2 expression uv (repeating), once per node; returns the
// float4 variable (rgba of the input). Reads the capture target of the node (make_image_node_capture_pass_through), or the result
// of the baked image node the input is linked straight to (no capture of a copy). Black until the first capture. filter:
// IMAGE_FILTER_NEAREST, _LINEAR or _CUBIC. Call it from the node functions with the placement as uv; everything else the node
// computes is written next to it in the material shader, as literals or uniforms, never inside the capture.
char *make_image_node_sample_input_at(ui_node_t *node, char *uv, i32 filter) {
	image_node_type_t *type   = make_image_node_type_of(node->type);
	ui_node_t         *source = type != NULL && type->pass_through_capture ? make_image_node_direct_source(node, type->image_input) : NULL;
	char              *id     = make_material_node_preview_id(source != NULL ? source : node, NULL);
	char              *var    = string_tmp("%s_input", parser_material_store_var_name(node));
	char              *tex_name = make_image_node_bind(source != NULL ? source : node, id);
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

// Pass-through with capture, for the tests and the Console trace: which 0 renders of the capture since the node was first traversed,
// 1 width of the capture target (0 when it has none), 2 the input is a baked result bound directly (0/1), 3 captures that found the
// shader text unchanged and rendered nothing; -1 for an unknown node or number
f64 make_image_node_pass_through_info(ui_node_t *node, i32 which) {
	char                    *id;
	make_image_node_state_t *s = make_image_node_state_of(node, &id);
	if (s == NULL) {
		return -1.0;
	}
	switch (which) {
	case 0:
		return (f64)s->capture_renders;
	case 1: {
		gpu_texture_t *t = g_context->node_previews != NULL ? any_map_get(g_context->node_previews, id) : NULL;
		return t != NULL ? (f64)t->width : 0.0;
	}
	case 2:
		return s->direct ? 1.0 : 0.0;
	case 3:
		return (f64)s->capture_skips;
	}
	return -1.0;
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

// The Capture Size setting of a pass-through node (Texture section): 512, 1024 (default), 2048 or 4096, read with make_image_node_size
ui_node_button_t *make_image_node_capture_size_setting() {
	return make_image_node_setting(_tr("Capture Size"), "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS);
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

// Registers the sections of a pass-through node (pass_through_capture): the button order of its definition is Capture Size
// (make_image_node_capture_size_setting), then the other settings. 0 Texture: Capture Size alone, closed by default, drawn below the
// inputs (after_inputs); 1 General: the other settings and the first n_general_inputs inputs, open; then the own sections (own, may be NULL);
// without own sections the remaining inputs form one open section Parameters. No state row, no Size.
void make_image_node_sections_capture(ui_node_t *def, ui_node_section_t *own, i32 own_count, i32 n_general_inputs) {
	i32                n_buttons = def->buttons->length;
	i32                n_inputs  = def->inputs->length;
	bool               params    = own_count == 0 && n_inputs > n_general_inputs;
	ui_node_section_t *list      = calloc(own_count + 4, sizeof(ui_node_section_t));
	i32                n         = 0;
	list[n++]                    = (ui_node_section_t){.title = _tr("Texture"), .kind = UI_NODE_ITEM_BUTTON, .first = 0, .count = 1, .closed_by_default = true, .after_inputs = true};
	list[n++]                    = (ui_node_section_t){.title             = _tr("General"),
	                                                   .kind              = UI_NODE_ITEM_BUTTON,
	                                                   .first             = 1,
	                                                   .count             = n_buttons - 1,
	                                                   .closed_by_default = false,
	                                                   .inputs_first      = 0,
	                                                   .inputs_count      = n_general_inputs};
	for (i32 i = 0; i < own_count; ++i) {
		list[n++] = own[i];
	}
	if (params) {
		list[n++] = (ui_node_section_t){.title = _tr("Parameters"), .kind = UI_NODE_ITEM_INPUT, .first = n_general_inputs, .count = n_inputs - n_general_inputs, .closed_by_default = false};
	}
	any_map_set(ui_nodes_sections, def->type, list); // The list ends with the zeroed entry
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
