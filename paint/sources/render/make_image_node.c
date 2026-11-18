#include "../global.h"

// Shared pass of the image nodes (Image menu).
// An image node evaluates its inputs in image space: the graph above the node drawn over the node's own square
// ([0, 1] of tex_coord, Size x Size pixels, no mesh, no uv layout, parser_material_image_space), read back and
// processed on the cpu by the node's type (image_node_type_t, make_image_node_register). The result is uploaded into
// g_context->node_previews under the node's preview id and sampled by the node like an Image Texture at the
// current texture coordinate, repeating (make_image_node_sample; Tile UV places an image on the uv with
// make_image_node_sample_at; bound as _teximage_<preview id>). Image nodes have no Vector input. Nothing is saved with the project.
// Every node preview traversal captures the node; a changed capture or parameter set makes it pending, and 0.5 s
// after the last change, with no mouse button held and no Bake Texture running, the pending nodes are baked:
// chained image nodes in dependency order, the inner node first.

#define MAKE_IMAGE_NODE_DEBOUNCE 0.5

extern ui_node_t    *ui_nodes_node_changed;
extern gpu_buffer_t *util_render_screen_aligned_full_vb;
extern gpu_buffer_t *util_render_screen_aligned_full_ib;

typedef struct make_image_node_state {
	image_node_type_t *type;
	i32                material_id;
	ui_node_t         *node; // Only compared, never dereferenced from here
	bool               has_baked;
	u64                baked_signature;
	i32                result_count; // What the type's process returned for the last bake (Flood Fill: islands)
	bool               pending;      // pending_pixels wait for the debounce
	u64                pending_signature;
	u8                *pending_pixels; // pending_size * pending_size RGBA8
	i32                pending_size;
	void              *pending_params;
	bool               needs_capture; // Graph changed while busy, capture when the debounce fires
	bool               force;         // Capture and bake on the next update even when unchanged
	string_array_t    *upstream;      // Preview ids of the image nodes the last capture sampled
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

static void make_image_node_update(void *_);
static void make_image_node_state_row(i32 node_id);

void make_image_node_register(image_node_type_t *type) {
	if (make_image_node_types == NULL) {
		make_image_node_types = any_array_create_from_raw((void *[]){}, 0);
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
	s->node        = node;
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
	free(s->pending_pixels);
	free(s->pending_params);
	s->pending_pixels = NULL;
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

static u64 make_image_node_hash(u64 h, u8 *bytes, size_t n) {
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		u64 w;
		memcpy(&w, bytes + i, 8);
		h ^= w;
		h *= 1099511628211ull;
	}
	for (; i < n; ++i) {
		h ^= bytes[i];
		h *= 1099511628211ull;
	}
	return h;
}

// 64 bit FNV-1a over the captured pixels, the size and the processing parameters
static u64 make_image_node_signature(u8 *pixels, i32 pixel_bytes, i32 size, void *params, i32 params_size) {
	u64 h = 14695981039346656037ull;
	h     = make_image_node_hash(h, pixels, (size_t)size * size * pixel_bytes);
	h     = make_image_node_hash(h, (u8 *)&size, sizeof(size));
	return make_image_node_hash(h, params, params_size);
}

// Shader evaluating expr(node) over the image square: a full screen triangle with tex_coord = the image uv,
// [0, 1] across the target, no mesh (parser_material_image_space gives mesh based inputs a neutral value).
// The node writes the expression while make_image_node_capturing is set. NULL when it does not compile.
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
		parser_material_push_group(make_image_node_capture_group);
		parser_material_parents = make_image_node_capture_parents;
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
// Returns the pixels (the caller frees them), or NULL when the shader does not compile.
static u8 *make_image_node_capture_pixels(ui_node_t *node, char *(*expr)(ui_node_t *), i32 size, gpu_texture_format_t format) {
	shader_context_t *scon = make_image_node_capture_shader(node, expr, format == GPU_TEXTURE_FORMAT_RGBA128 ? "RGBA128" : "RGBA32");
	if (scon == NULL) {
		return NULL;
	}
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

	buffer_t *b      = gpu_get_texture_pixels(target);
	u8       *pixels = b->buffer;
	b->buffer        = NULL; // Kept, gpu_delete_texture frees the rest
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
	free(pixels);
	return v;
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
	make_image_node_capturing       = s;
	make_image_node_capture_group   = group;
	make_image_node_capture_parents = parents;
	i32   size                      = t->size(node);
	i32   pixel_bytes               = t->capture_float ? 16 : 4;
	u8   *pixels                    = make_image_node_capture_pixels(node, t->capture, size, t->capture_float ? GPU_TEXTURE_FORMAT_RGBA128 : GPU_TEXTURE_FORMAT_RGBA32);
	void *params                    = NULL;
	if (pixels != NULL) {
		params = calloc(1, t->params_size > 0 ? t->params_size : 1);
		t->params(node, params); // May capture a linked scalar input
	}
	make_image_node_capturing       = NULL;
	make_image_node_capture_group   = NULL;
	make_image_node_capture_parents = NULL;
	if (pixels == NULL) {
		return false; // Shader error, keep what is baked
	}
	u64 sig = make_image_node_signature(pixels, pixel_bytes, size, params, t->params_size);

	bool has_result = g_context->node_previews != NULL && any_map_get(g_context->node_previews, id) != NULL;
	if (!force && has_result && s->has_baked && sig == s->baked_signature) {
		make_image_node_clear_pending(s); // Back to what is baked, e.g. a change undone within the debounce
		free(pixels);
		free(params);
		return false;
	}
	if (!force && s->pending && sig == s->pending_signature) {
		free(pixels); // Already waiting for this one
		free(params);
		return false;
	}
	make_image_node_clear_pending(s);
	s->pending_pixels    = pixels;
	s->pending_params    = params;
	s->pending_size      = size;
	s->pending_signature = sig;
	s->pending           = true;
	return true;
}

// Uploads size * size * 4 floats as the node's result, in the type's format
static void make_image_node_upload(char *id, f32 *result, i32 size, gpu_texture_format_t format) {
	size_t         n   = (size_t)size * size * 4;
	gpu_texture_t *tmp = NULL;
	if (format == GPU_TEXTURE_FORMAT_RGBA128) {
		buffer_t b = {.buffer = (u8 *)result, .length = (i32)(n * sizeof(f32))};
		tmp        = gpu_create_texture_from_bytes_raw(&b, size, size, GPU_TEXTURE_FORMAT_RGBA128);
	}
	else {
		format  = GPU_TEXTURE_FORMAT_RGBA64;
		u16 *hb = malloc(n * sizeof(u16));
		for (size_t i = 0; i < n; ++i) {
			hb[i] = float_to_half_fast(result[i]);
		}
		buffer_t b = {.buffer = (u8 *)hb, .length = (i32)(n * sizeof(u16))};
		tmp        = gpu_create_texture_from_bytes_raw(&b, size, size, GPU_TEXTURE_FORMAT_RGBA64);
		free(hb);
	}

	gpu_texture_t *image = any_map_get(g_context->node_previews, id);
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
	f32               *result = calloc((size_t)size * size * 4, sizeof(f32));
	if (result != NULL) {
		s->result_count = t->process(s->pending_pixels, size, s->pending_params, result);
		make_image_node_upload(id, result, size, t->result_format);
		free(result);
	}
	if (result == NULL || s->result_count < 0) {
		console_error(tr("Image node bake: out of memory"));
		s->has_baked = false; // Black result, the next traversal tries again
	}
	else {
		s->has_baked       = true;
		s->baked_signature = s->pending_signature;
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

// True while an image node the capture of e samples has not been handled in this run
static bool make_image_node_waits(make_image_node_entry_t *e, any_array_t *entries) {
	string_array_t *upstream = e->state->upstream;
	if (upstream == NULL) {
		return false;
	}
	for (i32 i = 0; i < entries->length; ++i) {
		make_image_node_entry_t *f = entries->buffer[i];
		if (f != e && !f->done && string_array_index_of(upstream, f->id) >= 0) {
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
			bool force = s->force || force_all;
			if (force || s->needs_capture || upstream_baked) {
				make_image_node_capture(s, e->id, e->node, e->group, e->parents, force);
			}
			if (s->pending) {
				make_image_node_bake(s, e->id);
				any_array_push(baked, e->id);
				if (changed == NULL) {
					changed = e->node;
				}
			}
			s->force = false;
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

	string_array_t *keys      = make_image_node_states_keys();
	i32_array_t    *materials = i32_array_create(0);
	for (i32 i = 0; i < keys->length; ++i) {
		make_image_node_state_t *s = any_map_get(make_image_node_states, keys->buffer[i]);
		if ((s->pending || s->needs_capture || s->force) && i32_array_index_of(materials, s->material_id) == -1) {
			i32_array_push(materials, s->material_id);
		}
	}
	make_image_node_free_keys(keys);
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
	keys = make_image_node_states_keys();
	for (i32 i = 0; i < keys->length; ++i) {
		make_image_node_state_t *s = any_map_get(make_image_node_states, keys->buffer[i]);
		s->needs_capture           = false;
		s->force                   = false;
		make_image_node_clear_pending(s);
	}
	make_image_node_free_keys(keys);

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
	bool            any  = false;
	string_array_t *keys = make_image_node_states_keys();
	for (i32 i = 0; i < keys->length; ++i) {
		make_image_node_state_t *s = any_map_get(make_image_node_states, keys->buffer[i]);
		any                        = any || s->pending || s->needs_capture || s->force;
	}
	make_image_node_free_keys(keys);
	return any;
}

static bool make_image_node_any_forced() {
	if (make_image_node_force_all) {
		return true;
	}
	bool            any  = false;
	string_array_t *keys = make_image_node_states_keys();
	for (i32 i = 0; i < keys->length; ++i) {
		make_image_node_state_t *s = any_map_get(make_image_node_states, keys->buffer[i]);
		any                        = any || s->force;
	}
	make_image_node_free_keys(keys);
	return any;
}

static void make_image_node_update(void *_) {
	if (!make_image_node_any_waiting()) {
		sys_remove_update(make_image_node_update);
		make_image_node_scheduled = false;
		return;
	}
	// A forced bake (Regenerate image nodes) does not wait for the debounce, only for the mouse and Bake Texture
	if (make_image_node_busy() || (!make_image_node_any_forced() && sys_time() - make_image_node_last_change < MAKE_IMAGE_NODE_DEBOUNCE)) {
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

// Preview id of an image node of the current material, NULL before its first traversal
static char *make_image_node_id_of(ui_node_t *node) {
	if (make_image_node_states == NULL || node == NULL) {
		return NULL;
	}
	char           *found = NULL;
	string_array_t *keys  = make_image_node_states_keys();
	for (i32 i = 0; i < keys->length; ++i) {
		make_image_node_state_t *s = any_map_get(make_image_node_states, keys->buffer[i]);
		if (s->node == node && s->material_id == g_context->material->id) {
			found = keys->buffer[i];
		}
	}
	make_image_node_free_keys(keys);
	return found;
}

// State of an image node of the current material, NULL before its first traversal
static make_image_node_state_t *make_image_node_state_of(ui_node_t *node) {
	char *id = make_image_node_id_of(node);
	return id != NULL ? any_map_get(make_image_node_states, id) : NULL;
}

// True while the node waits for its bake
bool make_image_node_is_pending(ui_node_t *node) {
	make_image_node_state_t *s = make_image_node_state_of(node);
	return s != NULL && (s->pending || s->needs_capture || s->force || make_image_node_force_all);
}

// Baked result of an image node of the current material, NULL when not baked
gpu_texture_t *make_image_node_result(ui_node_t *node) {
	char                    *id = make_image_node_id_of(node);
	make_image_node_state_t *s  = id != NULL ? any_map_get(make_image_node_states, id) : NULL;
	if (s == NULL || !s->has_baked || g_context->node_previews == NULL) {
		return NULL;
	}
	return any_map_get(g_context->node_previews, id);
}

// What the type's process returned for the last bake of the node (Flood Fill: islands), -1 when not baked
i32 make_image_node_result_count(ui_node_t *node) {
	make_image_node_state_t *s = make_image_node_state_of(node);
	return s != NULL && s->has_baked ? s->result_count : -1;
}

// Regenerate image nodes: captures and bakes every image node of the project on the next update, also the ones whose
// signature did not change (an Image Texture whose file was reloaded)
void make_image_node_regenerate() {
	make_image_node_force_all = true;
	make_image_node_schedule();
}

// Captures and bakes the node on the next update, even when nothing changed
void make_image_node_bake_now(ui_node_t *node) {
	make_image_node_state_t *s = make_image_node_state_of(node);
	if (s != NULL) {
		s->force = true;
		make_image_node_schedule();
	}
}

// Writes the fetch of the node's baked result at the float2 expression uv, once per variable. Repeating: the uv is wrapped
// and snapped to the texel centre, so the one linear sampler never blends two texels. Black until the first bake.
static void make_image_node_write_fetch(char *var, char *tex_name, char *uv, i32 filter) {
	parser_material_write(parser_material_kong, string_tmp("float2 %s_uv = %s;", var, uv));
	if (filter == IMAGE_FILTER_CUBIC) {
		// Cubic B-spline over 4 x 4 texel centres (wrapped, like the bilinear below). Chosen over Catmull-Rom: it is C2, so
		// the slope of the result (ddx, ddy of a Bump node) has no jumps at texel borders, and it never overshoots, so no
		// dark or bright halos at the steps of a mask. The price is a blur of about half a texel, below a screen pixel
		// wherever the result is magnified, and 16 fetches per pixel.
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

// Records the dependency of a capture on the node, binds its result texture and size constant; returns the texture name
static char *make_image_node_bind(ui_node_t *node, char *id) {
	if (make_image_node_capturing != NULL && string_array_index_of(make_image_node_capturing->upstream, id) == -1) {
		any_array_push(make_image_node_capturing->upstream, string_copy(id)); // Chained: bakes before the node being captured
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
		make_image_node_write_fetch(var, tex_name, "tex_coord", filter);
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
	if (make_image_node_is_pending(node)) {
		ui_text(tr("Baking"), UI_ALIGN_CENTER, g_theme->HIGHLIGHT_COL);
		return;
	}
	i32 _TEXT_COL     = g_theme->TEXT_COL;
	g_theme->TEXT_COL = g_theme->LABEL_COL;
	ui_text(make_image_node_result(node) != NULL ? tr("Baked") : "", UI_ALIGN_CENTER, 0);
	g_theme->TEXT_COL = _TEXT_COL;
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
