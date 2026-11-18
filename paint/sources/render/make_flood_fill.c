#include "../global.h"

// Flood Fill bake on the cpu, in generator space.
// Every node preview traversal captures the node's mask input: the graph above the node evaluated over the node's own
// square ([0, 1] of tex_coord, Size x Size pixels, no mesh, no uv layout), read back and thresholded. A changed mask or
// parameter set makes the node pending; 0.5 s after the last change the islands are labelled (util/flood_fill_label.c)
// and uploaded as one RGBA64 texture into g_context->node_previews, sampled by the node like an Image Texture as
// _texflood_<preview id>. Nothing is saved with the project.

#define MAKE_FLOOD_FILL_DEBOUNCE 0.5

extern ui_node_t    *ui_nodes_node_changed;
extern gpu_buffer_t *util_render_screen_aligned_full_vb;
extern gpu_buffer_t *util_render_screen_aligned_full_ib;

typedef struct make_flood_fill_state {
	i32                 material_id;
	ui_node_t          *node; // Only compared, never dereferenced from here
	bool                has_baked;
	u64                 baked_signature;
	i32                 islands; // Kept islands of the last bake
	bool                pending; // pending_mask waits for the debounce
	u64                 pending_signature;
	u8                 *pending_mask;
	flood_fill_params_t pending_params;
	bool                needs_capture; // Graph changed during a drag, capture when the debounce fires
} make_flood_fill_state_t;

static any_map_t *make_flood_fill_states      = NULL; // Preview id to state
static f64        make_flood_fill_last_change = 0.0;
static bool       make_flood_fill_scheduled   = false;
static ui_node_t *make_flood_fill_force_node  = NULL; // Bake button, baked on the next update

static void make_flood_fill_update(void *_);

static void make_flood_fill_redraw_nodes() {
	if (ui_nodes_hwnd != NULL) {
		ui_nodes_hwnd->redraws = 2;
	}
}

static make_flood_fill_state_t *make_flood_fill_get_state(char *id) {
	if (make_flood_fill_states == NULL) {
		make_flood_fill_states = any_map_create();
	}
	make_flood_fill_state_t *s = any_map_get(make_flood_fill_states, id);
	if (s == NULL) {
		s = calloc(1, sizeof(make_flood_fill_state_t));
		any_map_set(make_flood_fill_states, string_copy(id), s);
	}
	return s;
}

static void make_flood_fill_clear_pending(make_flood_fill_state_t *s) {
	free(s->pending_mask);
	s->pending_mask = NULL;
	s->pending      = false;
}

static void make_flood_fill_schedule() {
	make_flood_fill_last_change = sys_time();
	make_flood_fill_redraw_nodes(); // Baking state of the node
	if (!make_flood_fill_scheduled) {
		make_flood_fill_scheduled = true;
		sys_notify_on_update(make_flood_fill_update, NULL);
	}
}

// 64 bit FNV-1a over the inside mask and the labelling parameters
static u64 make_flood_fill_signature(u8 *mask, flood_fill_params_t *p) {
	u64    h = 14695981039346656037ull;
	size_t n = (size_t)p->width * p->height;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		u64 w;
		memcpy(&w, mask + i, 8);
		h ^= w;
		h *= 1099511628211ull;
	}
	for (; i < n; ++i) {
		h ^= mask[i];
		h *= 1099511628211ull;
	}
	i32 v[6] = {p->width, p->height, p->connectivity8, p->wrap, (i32)p->seed, p->min_size};
	for (i32 j = 0; j < 6; ++j) {
		h ^= (u64)(u32)v[j];
		h *= 1099511628211ull;
	}
	return h;
}

// Shader evaluating the mask input of node in generator space: a full screen triangle with tex_coord = the generator uv,
// [0, 1] across the target, no mesh (parser_material_generator gives mesh based inputs a neutral value).
// The node passes its input through while parsed. NULL when it does not compile.
static shader_context_t *make_flood_fill_capture_shader(ui_node_t *node, ui_node_canvas_t *group, ui_node_t_array_t *parents) {
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
	                                                                     "RGBA32",
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
	if (group != NULL) {
		parser_material_push_group(group);
		parser_material_parents = parents;
	}
	ui_node_link_t_array_t *links = parser_material_links;
	ui_node_link_t *link = ALLOC_INIT(ui_node_link_t, {.id = ui_next_link_id(links), .from_id = node->id, .from_socket = 0, .to_id = -1, .to_socket = -1});
	any_array_push(links, link);
	parser_material_con                   = con;
	parser_material_kong                  = kong;
	parser_material_flood_passthrough     = node;
	parser_material_generator             = true;
	parser_material_transform_color_space = false;
	char *res                             = parser_material_write_result(link); // ID output, the mask while passed through
	parser_material_transform_color_space = true;
	parser_material_generator             = false;
	parser_material_flood_passthrough     = NULL;
	array_remove(links, link);

	kong->frag_out = "float4";
	node_shader_write_frag(kong, string_tmp("output = float4(%s, %s, %s, 1.0);", res, res, res));
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

// Renders the mask input of node over the generator square at size x size and thresholds it,
// returns size * size bytes, 1 = inside, or NULL when the shader does not compile
static u8 *make_flood_fill_capture_mask(ui_node_t *node, ui_node_canvas_t *group, ui_node_t_array_t *parents, i32 size) {
	shader_context_t *scon = make_flood_fill_capture_shader(node, group, parents);
	if (scon == NULL) {
		return NULL;
	}
	if (util_render_screen_aligned_full_vb == NULL) {
		util_render_create_screen_aligned_full_data();
	}
	gpu_texture_t         *target = gpu_create_render_target(size, size, GPU_TEXTURE_FORMAT_RGBA32);
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

	// The shader writes float4(mask, mask, mask, 1), any color channel holds it whatever the byte order
	u8  *pixels    = gpu_get_texture_pixels(target)->buffer;
	f32  threshold = flood_fill_node_threshold(node) * 255.0;
	bool invert    = flood_fill_node_invert(node);
	u8  *mask      = malloc((size_t)size * size);
	for (size_t i = 0; i < (size_t)size * size; ++i) {
		mask[i] = (pixels[i * 4] > threshold) != invert;
	}
	gpu_delete_texture(target); // Frees the readback copy too
	return mask;
}

// Captures the node's mask. A new mask or parameter set makes the node pending, returns true then.
// force keeps it pending even when it matches the baked result.
static bool make_flood_fill_capture(make_flood_fill_state_t *s, char *id, ui_node_t *node, ui_node_canvas_t *group, ui_node_t_array_t *parents, bool force) {
	i32 size         = flood_fill_node_size(node);
	u8 *mask         = make_flood_fill_capture_mask(node, group, parents, size);
	s->needs_capture = false;
	if (mask == NULL) {
		return false; // Shader error, keep what is baked
	}
	flood_fill_params_t p   = flood_fill_node_params(node, size, size);
	u64                 sig = make_flood_fill_signature(mask, &p);

	bool has_result = g_context->node_previews != NULL && any_map_get(g_context->node_previews, id) != NULL;
	if (!force && has_result && s->has_baked && sig == s->baked_signature) {
		make_flood_fill_clear_pending(s); // Back to what is baked, e.g. a change undone within the debounce
		free(mask);
		return false;
	}
	if (!force && s->pending && sig == s->pending_signature) {
		free(mask); // Already waiting for this one
		return false;
	}
	free(s->pending_mask);
	s->pending_mask      = mask;
	s->pending_params    = p;
	s->pending_signature = sig;
	s->pending           = true;
	return true;
}

// Uploads width * height * 4 floats (ID, Position x, Position y, Area) as the node's RGBA64 result
static void make_flood_fill_upload(char *id, f32 *out, i32 w, i32 h) {
	size_t n  = (size_t)w * h * 4;
	u16   *hb = malloc(n * sizeof(u16));
	for (size_t i = 0; i < n; ++i) {
		f32 v = out[i];
		if (i % 4 == 3 && v > 0.0 && v < 1.0 / FLOOD_FILL_AREA_FLOOR) {
			v = 1.0 / FLOOD_FILL_AREA_FLOOR; // Area above zero marks a kept island, Mask is derived from it
		}
		hb[i] = float_to_half_fast(v);
	}
	buffer_t       b   = {.buffer = (u8 *)hb, .length = (i32)(n * sizeof(u16))};
	gpu_texture_t *tmp = gpu_create_texture_from_bytes_raw(&b, w, h, GPU_TEXTURE_FORMAT_RGBA64);
	free(hb);

	gpu_texture_t *image = any_map_get(g_context->node_previews, id);
	if (image == NULL || image->width != w || image->height != h || image->format != GPU_TEXTURE_FORMAT_RGBA64) {
		if (image != NULL) {
			gpu_delete_texture(image);
		}
		image = gpu_create_render_target(w, h, GPU_TEXTURE_FORMAT_RGBA64);
		any_map_set(g_context->node_previews, string_copy(id), image);
	}
	// Drawn into a render target like the captured input, so the orientation matches on every backend
	draw_begin(image, false, 0);
	draw_set_pipeline(pipes_copy64);
	draw_image(tmp, 0, 0);
	draw_set_pipeline(NULL);
	draw_end();
	gpu_delete_texture(tmp);
}

// Labels the pending mask of s into its result texture
static void make_flood_fill_bake(make_flood_fill_state_t *s, char *id) {
	if (!s->pending) {
		return;
	}
	if (g_context->node_previews == NULL) {
		g_context->node_previews = any_map_create();
	}
	flood_fill_params_t *p   = &s->pending_params;
	f32                 *out = malloc((size_t)p->width * p->height * 4 * sizeof(f32));
	if (out != NULL) {
		s->islands = flood_fill_label(s->pending_mask, p, out);
		make_flood_fill_upload(id, out, p->width, p->height);
		free(out);
		if (s->islands < 0) {
			console_error(tr("Flood Fill: out of memory"));
			s->has_baked = false; // Black result, the next traversal tries again
		}
		else {
			s->has_baked       = true;
			s->baked_signature = s->pending_signature;
		}
	}
	else {
		console_error(tr("Flood Fill: out of memory"));
	}
	make_flood_fill_clear_pending(s);
}

// Shows a new result: re-parses the material (downstream Blur, Warp and Flood Fill follow), refills fill layers
static void make_flood_fill_refresh(slot_material_t *m, ui_node_t *node) {
	if (m == g_context->material) {
		make_material_parse_paint_material(true); // Captures again, finds the baked signature
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
	make_flood_fill_redraw_nodes();
	if (ui_view2d_hwnd != NULL) {
		ui_view2d_hwnd->redraws = 2;
	}
	g_context->ddirty = 2;
}

// Captures the Flood Fill nodes of the current material flagged needs_capture, and force_node (kept pending even when unchanged)
static void make_flood_fill_traverse(ui_node_t_array_t *nodes, ui_node_canvas_t *group, ui_node_t_array_t *parents, ui_node_t *force_node) {
	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *node = nodes->buffer[i];
		if (string_equals(node->type, "TEX_FLOOD_FILL")) {
			char                    *id = make_material_node_preview_id(node, parents);
			make_flood_fill_state_t *s  = any_map_get(make_flood_fill_states, id);
			if (node == force_node) {
				s              = make_flood_fill_get_state(id);
				s->material_id = g_context->material->id;
				s->node        = node;
				make_flood_fill_capture(s, id, node, group, parents, true);
			}
			else if (s != NULL && s->needs_capture) {
				make_flood_fill_capture(s, id, node, group, parents, false);
			}
		}
		else if (string_equals(node->type, "GROUP")) {
			for (i32 j = 0; j < g_project->_->material_groups->length; ++j) {
				node_group_t *g = g_project->_->material_groups->buffer[j];
				if (string_equals(g->canvas->name, node->name)) {
					any_array_push(parents, node);
					make_flood_fill_traverse(g->canvas->nodes, g->canvas, parents, force_node);
					array_pop(parents);
					break;
				}
			}
		}
	}
}

static slot_material_t *make_flood_fill_material(i32 id) {
	for (i32 i = 0; i < g_project->_->materials->length; ++i) {
		slot_material_t *m = g_project->_->materials->buffer[i];
		if (m->id == id) {
			return m;
		}
	}
	return NULL;
}

// Captures what drags left stale, bakes every pending node and refreshes the materials that changed
static void make_flood_fill_run(ui_node_t *force_node) {
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}

	if (make_flood_fill_states == NULL) {
		make_flood_fill_states = any_map_create();
	}
	string_array_t    *keys      = map_keys(make_flood_fill_states);
	i32_array_t       *materials = i32_array_create(0); // Ids of the materials to capture, then to refresh
	ui_node_t_array_t *changed   = any_array_create_from_raw((void *[]){}, 0);
	ui_node_t_array_t *parents   = any_array_create_from_raw((void *[]){}, 0);
	slot_material_t   *_material = g_context->material;
	for (i32 i = 0; i < keys->length; ++i) {
		make_flood_fill_state_t *s = any_map_get(make_flood_fill_states, keys->buffer[i]);
		if (s->needs_capture && i32_array_index_of(materials, s->material_id) == -1) {
			i32_array_push(materials, s->material_id);
		}
	}
	if (force_node != NULL && i32_array_index_of(materials, _material->id) == -1) {
		i32_array_push(materials, _material->id);
	}
	for (i32 i = 0; i < materials->length; ++i) {
		slot_material_t *m = make_flood_fill_material(materials->buffer[i]);
		if (m == NULL) {
			continue;
		}
		g_context->material = m;
		make_flood_fill_traverse(m->canvas->nodes, NULL, parents, m == _material ? force_node : NULL);
	}
	g_context->material = _material;

	// Listed again, the force capture may have added a state
	array_free(keys);
	free(keys);
	keys              = map_keys(make_flood_fill_states);
	materials->length = 0;
	for (i32 i = 0; i < keys->length; ++i) {
		char                    *id = keys->buffer[i];
		make_flood_fill_state_t *s  = any_map_get(make_flood_fill_states, id);
		s->needs_capture            = false; // Node not found in its material
		if (!s->pending) {
			continue;
		}
		make_flood_fill_bake(s, id);
		if (i32_array_index_of(materials, s->material_id) == -1) {
			i32_array_push(materials, s->material_id);
			any_array_push(changed, s->node);
		}
	}
	for (i32 i = 0; i < materials->length; ++i) {
		slot_material_t *m = make_flood_fill_material(materials->buffer[i]);
		if (m != NULL) {
			make_flood_fill_refresh(m, changed->buffer[i]);
		}
	}
	array_free(keys);
	free(keys);
	array_free(materials);
	free(materials);
	array_free(changed);
	free(changed);
	array_free(parents);
	free(parents);
	if (in_use) {
		draw_begin(current, false, 0);
	}
}

static bool make_flood_fill_any_pending() {
	if (make_flood_fill_states == NULL) {
		return false;
	}
	bool            any  = false;
	string_array_t *keys = map_keys(make_flood_fill_states);
	for (i32 i = 0; i < keys->length; ++i) {
		make_flood_fill_state_t *s = any_map_get(make_flood_fill_states, keys->buffer[i]);
		any                        = any || s->pending || s->needs_capture;
	}
	array_free(keys);
	free(keys);
	return any;
}

static void make_flood_fill_update(void *_) {
	ui_node_t *force_node = make_flood_fill_force_node;
	if (force_node == NULL && !make_flood_fill_any_pending()) {
		sys_remove_update(make_flood_fill_update);
		make_flood_fill_scheduled = false;
		return;
	}
	// Bake Texture or a drag running (pdirty is no signal: a fill layer leaves it at 1)
	bool busy = bake_texture_node_baking || g_ui->input_down;
	if (busy || (force_node == NULL && sys_time() - make_flood_fill_last_change < MAKE_FLOOD_FILL_DEBOUNCE)) {
		return;
	}
	sys_remove_update(make_flood_fill_update);
	make_flood_fill_scheduled  = false;
	make_flood_fill_force_node = NULL;
	make_flood_fill_run(force_node);
	make_flood_fill_redraw_nodes();
}

// Node preview traversal hook (make_material_bake_node_preview), linked = refresh during a paint stroke
void make_flood_fill_on_traverse(ui_node_t *node, ui_node_canvas_t *group, ui_node_t_array_t *parents, bool linked) {
	char *id = make_material_node_preview_id(node, parents);
	any_array_push(g_context->node_previews_used, id); // Keeps the baked result
	make_flood_fill_state_t *s = make_flood_fill_get_state(id);
	s->material_id             = g_context->material->id;
	s->node                    = node;
	if (linked) {
		return; // A stroke changes layers, never the generator input (Layer nodes are neutral there)
	}
	if (bake_texture_node_baking || g_ui->input_down) {
		// No readback per frame of a slider drag, or while Bake Texture runs
		s->needs_capture = true;
		make_flood_fill_schedule();
		return;
	}
	if (make_flood_fill_capture(s, id, node, group, parents, false)) {
		make_flood_fill_schedule();
	}
}

// Frees the states of material m, except the preview ids listed in keep (all when keep is NULL)
void make_flood_fill_delete_states(slot_material_t *m, string_array_t *keep) {
	if (make_flood_fill_states == NULL) {
		return;
	}
	string_array_t *keys = map_keys(make_flood_fill_states);
	for (i32 i = 0; i < keys->length; ++i) {
		char                    *key = keys->buffer[i];
		make_flood_fill_state_t *s   = any_map_get(make_flood_fill_states, key);
		if (s->material_id == m->id && (keep == NULL || string_array_index_of(keep, key) == -1)) {
			make_flood_fill_clear_pending(s);
			free(s);
			map_delete(make_flood_fill_states, key);
			free(key);
		}
	}
	array_free(keys);
	free(keys);
}

// True while the node waits for its bake
bool make_flood_fill_is_pending(ui_node_t *node) {
	if (make_flood_fill_force_node == node) {
		return true;
	}
	if (make_flood_fill_states == NULL) {
		return false;
	}
	bool            pending = false;
	string_array_t *keys    = map_keys(make_flood_fill_states);
	for (i32 i = 0; i < keys->length; ++i) {
		make_flood_fill_state_t *s = any_map_get(make_flood_fill_states, keys->buffer[i]);
		if (s->node == node && s->material_id == g_context->material->id) {
			pending = s->pending || s->needs_capture;
		}
	}
	array_free(keys);
	free(keys);
	return pending;
}

// Bake button: captures and bakes the node on the next update, even when nothing changed
void make_flood_fill_bake_now(ui_node_t *node) {
	make_flood_fill_force_node = node;
	make_flood_fill_schedule();
}
