#include "../global.h"

// Shared pass of the image nodes (Pattern and Filter menus).
// An image node evaluates its inputs in image space: the graph above the node drawn over the node's own square
// ([0, 1] of tex_coord, Size x Size pixels, no mesh, no uv layout, parser_material_image_space), read back and
// processed on the cpu by the node's type (image_node_type_t, make_image_node_register). The result is uploaded into
// g_context->node_previews under the node's preview id and sampled by the node like an Image Texture at the
// current texture coordinate, repeating (make_image_node_sample; Tile UV places an image on the uv with
// make_image_node_sample_at; bound as _teximage_<preview id>). Image nodes have no Vector input.
// Nothing is saved with the project.
// Every node preview traversal captures the node; a changed capture or parameter set makes it pending, and 0.5 s
// after the last change, with no mouse button held and no Bake Texture running, the pending nodes are baked:
// chained image nodes in dependency order, the inner node first.

#define MAKE_IMAGE_NODE_DEBOUNCE 0.5
static bool make_image_node_trace = false; // ARMORPAINT_IMAGE_NODE_TRACE in the environment: every capture is logged with its time
static i32 make_image_node_captures = 0; // Captures made since the start, see make_image_node_capture_count
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
static i32                      make_image_node_capture_dst     = 0; // Width of the target of the running capture, see make_image_node_box_taps

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
static u64 make_image_node_signature(u8 *pixels, i32 pixel_bytes, i32 size, void *params, i32 params_size) {
	u64 h = make_image_node_hash(0, pixels, (size_t)size * size * pixel_bytes);
	h     = make_image_node_hash(h, &size, sizeof(size));
	return make_image_node_hash(h, params, params_size);
}

// Shader evaluating expr(node) over the image square: a full screen triangle with tex_coord = the image uv,
// [0, 1] across the target, no mesh (parser_material_image_space gives mesh based inputs a neutral value).
// The node writes the expression while make_image_node_capturing is set.
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
	gpu_create_shaders_from_kong(node_shader_get(kong), &con->data->vertex_shader, &con->data->fragment_shader, &con->data->_->vertex_shader_size,
	                             &con->data->_->fragment_shader_size);
	shader_context_load(con->data);
	shader_context_t *scon = con->data;
	node_shader_context_free(con);
	free(data);
	return scon;
}

// Renders expr(node) over the image square into a size x size target of format and reads it back.
// Returns the pixels (the caller frees them), or NULL when the readback fails.
static u8 *make_image_node_capture_pixels(ui_node_t *node, char *(*expr)(ui_node_t *), i32 size, gpu_texture_format_t format) {
	make_image_node_capture_dst = size;
	shader_context_t *scon      = make_image_node_capture_shader(node, expr, format == GPU_TEXTURE_FORMAT_RGBA128 ? "RGBA128" : "RGBA32");
	make_image_node_capture_dst = 0;
	if (util_render_screen_aligned_full_vb == NULL) {
		util_render_create_screen_aligned_full_data();
	}
	gpu_texture_t         *target = gpu_create_render_target(size, size, format);
	static string_array_t *empty  = NULL;
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

static char *make_image_node_input_expr(ui_node_t *node) {
	char *v = parser_material_parse_value_input(node->inputs->buffer[make_image_node_capture_input], false);
	return string_tmp("float4(%s, %s, %s, 1.0)", v, v, v);
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
	ui_node_socket_t *socket = node->inputs->buffer[input];
	if (make_image_node_capturing == NULL || parser_material_get_input_link(socket) == NULL) {
		return socket->default_value->buffer[0];
	}
	make_image_node_capture_input = input;
	f32 *pixels                   = (f32 *)make_image_node_capture_pixels(node, make_image_node_input_expr, 1, GPU_TEXTURE_FORMAT_RGBA128);
	if (pixels == NULL) {
		return socket->default_value->buffer[0];
	}
	f32 v = pixels[0]; // A 1 x 1 target: its pixel centre is the image centre
	make_image_node_plane_free(pixels, 16);
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
	if (make_image_node_trace) {
		iron_log("[image node] capture %d %s time %.3f", make_image_node_captures, id, sys_time());
	}
	s->cycle_seen                   = false;
	make_image_node_capturing       = s;
	make_image_node_capture_group   = group;
	make_image_node_capture_parents = parents;
	i32   size                      = t->size(node);
	i32   pixel_bytes               = t->capture_float ? 16 : 4;
	size_t pixels_bytes             = (size_t)size * size * pixel_bytes;
	u8   *pixels                    = make_image_node_capture_pixels(node, t->capture, size, t->capture_float ? GPU_TEXTURE_FORMAT_RGBA128 : GPU_TEXTURE_FORMAT_RGBA32);
	void *params                    = NULL;
	if (pixels != NULL) {
		params = calloc(1, t->params_size > 0 ? t->params_size : 1);
		if (params == NULL) {
			make_image_node_plane_free(pixels, pixels_bytes);
			pixels = NULL;
		}
		else {
			t->params(node, params); // May capture a linked scalar input
		}
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
	u64 sig = make_image_node_signature(pixels, pixel_bytes, size, params, t->params_size);

	bool has_result = g_context->node_previews != NULL && any_map_get(g_context->node_previews, id) != NULL;
	if (!force && has_result && s->has_baked && sig == s->baked_signature) {
		make_image_node_clear_pending(s); // Back to what is baked, e.g. a change undone within the debounce
		make_image_node_plane_free(pixels, pixels_bytes);
		free(params);
		return false;
	}
	if (!force && s->failed && sig == s->failed_signature) {
		make_image_node_plane_free(pixels, pixels_bytes); // Failed already, retried after a change or Regenerate Image Nodes
		free(params);
		return false;
	}
	if (!force && s->pending && sig == s->pending_signature) {
		make_image_node_plane_free(pixels, pixels_bytes); // Already waiting for this one
		free(params);
		return false;
	}
	make_image_node_clear_pending(s);
	s->pending_pixels    = pixels;
	s->pending_bytes     = pixels_bytes;
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

// Regenerate image nodes: captures and bakes every image node of the project on the next update, also the ones whose
// signature did not change (an Image Texture whose file was reloaded)
void make_image_node_regenerate() {
	make_image_node_force_all = true;
	make_image_node_schedule();
}

// Taps per axis of a capture reading an image node of size source: 1 unless the source is at least twice as large as the
// target of the capture (a larger inner node, e.g. a 4096 Shape into a 512 node, would be point sampled and lose its
// antialiased edge), then source / target, at most 4 (16 taps). The 1 x 1 capture of a scalar input keeps its single tap.
static i32 make_image_node_box_taps(i32 source) {
	i32 dst = make_image_node_capture_dst;
	if (!parser_material_image_space || dst <= 1 || source < 2 * dst) {
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
		i32 n = filter == IMAGE_FILTER_NEAREST || type == NULL ? 1 : make_image_node_box_taps(type->size(node));
		if (n > 1) {
			// The source is at least twice as large as the target: n x n taps spread over the footprint of one target pixel
			// (a box filter), each tap read like the single one
			for (i32 j = 0; j < n; ++j) {
				for (i32 i = 0; i < n; ++i) {
					char *uv = string_tmp("tex_coord + float2(%f, %f) / %f", ((i + 0.5) / n - 0.5), ((j + 0.5) / n - 0.5), (f64)make_image_node_capture_dst);
					make_image_node_write_fetch(string_tmp("%s_b%d", var, j * n + i), tex_name, uv, filter);
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
// 0 Texture: the Size setting and the state row, the first two buttons of every image node, closed by default.
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
	list[n++]                    = (ui_node_section_t){.title = _tr("Texture"), .kind = UI_NODE_ITEM_BUTTON, .first = 0, .count = 2, .closed_by_default = true};
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
