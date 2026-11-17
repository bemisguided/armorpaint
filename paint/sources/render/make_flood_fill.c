#include "../global.h"

// Flood Fill bake on the cpu.
// Every node preview traversal captures the node's mask input (rendered with the node passing its input through,
// read back and thresholded). A changed mask or parameter set makes the node pending; 0.5 s after the last change
// the islands are labelled (util/flood_fill_label.c) and uploaded as one RGBA64 texture into g_context->node_previews,
// sampled by the node as _texflood_<preview id>. Nothing is saved with the project.

#define MAKE_FLOOD_FILL_DEBOUNCE 0.5

extern ui_node_t *ui_nodes_node_changed;

typedef struct make_flood_fill_state {
	i32                 material_id;
	ui_node_t          *node; // Only compared, never dereferenced from here
	bool                has_baked;
	u64                 baked_signature;
	bool                pending; // pending_mask waits for the debounce
	u64                 pending_signature;
	u8                 *pending_mask;
	flood_fill_params_t pending_params;
	bool                needs_capture; // Input may have changed during a stroke, capture when the debounce fires
} make_flood_fill_state_t;

static any_map_t *make_flood_fill_states      = NULL; // Preview id to state
static f64        make_flood_fill_last_change = 0.0;
static bool       make_flood_fill_scheduled   = false;
static ui_node_t *make_flood_fill_force_node  = NULL; // Bake button, baked on the next update

static void make_flood_fill_update(void *_);

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
	ui_nodes_hwnd->redraws      = 2; // Baking state of the node
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

// Renders the mask input of node at w x h and thresholds it, returns w * h bytes, 1 = inside
static u8 *make_flood_fill_capture_mask(ui_node_t *node, ui_node_canvas_t *group, ui_node_t_array_t *parents, i32 w, i32 h) {
	gpu_texture_t *target             = gpu_create_render_target(w, h, GPU_TEXTURE_FORMAT_RGBA32);
	parser_material_flood_passthrough = node;
	util_render_make_node_preview(g_context->material->canvas, node, target, group, parents);
	parser_material_flood_passthrough = NULL;

	// The preview writes float3(mask), any color channel holds it whatever the byte order
	u8  *pixels    = gpu_get_texture_pixels(target)->buffer;
	f32  threshold = flood_fill_node_threshold(node) * 255.0;
	bool invert    = flood_fill_node_invert(node);
	u8  *mask      = malloc((size_t)w * h);
	for (size_t i = 0; i < (size_t)w * h; ++i) {
		bool inside = pixels[i * 4] > threshold;
		mask[i]     = inside != invert;
	}
	gpu_delete_texture(target); // Frees the readback copy too
	return mask;
}

// Captures the node's mask. A new mask or parameter set makes the node pending, returns true then.
// force keeps it pending even when it matches the baked result.
static bool make_flood_fill_capture(make_flood_fill_state_t *s, char *id, ui_node_t *node, ui_node_canvas_t *group, ui_node_t_array_t *parents, bool force) {
	i32                 w    = config_get_texture_res_x();
	i32                 h    = config_get_texture_res_y();
	u8                 *mask = make_flood_fill_capture_mask(node, group, parents, w, h);
	flood_fill_params_t p    = flood_fill_node_params(node, w, h);
	u64                 sig  = make_flood_fill_signature(mask, &p);
	s->needs_capture         = false;

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
		if (flood_fill_label(s->pending_mask, p, out) < 0) {
			console_error(tr("Flood Fill: out of memory"));
		}
		make_flood_fill_upload(id, out, p->width, p->height);
		free(out);
		s->has_baked       = true;
		s->baked_signature = s->pending_signature;
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
	ui_nodes_hwnd->redraws  = 2;
	ui_view2d_hwnd->redraws = 2;
	g_context->ddirty       = 2;
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

// Captures what strokes left stale, bakes every pending node and refreshes the materials that changed
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
	bool busy = g_context->pdirty > 0 || bake_texture_node_baking; // Stroke or Bake Texture running
	if (busy || (force_node == NULL && sys_time() - make_flood_fill_last_change < MAKE_FLOOD_FILL_DEBOUNCE)) {
		return;
	}
	sys_remove_update(make_flood_fill_update);
	make_flood_fill_scheduled  = false;
	make_flood_fill_force_node = NULL;
	make_flood_fill_run(force_node);
	ui_nodes_hwnd->redraws = 2;
}

// Node preview traversal hook (make_material_bake_node_preview), linked = refresh during a paint stroke
void make_flood_fill_on_traverse(ui_node_t *node, ui_node_canvas_t *group, ui_node_t_array_t *parents, bool linked) {
	char *id = make_material_node_preview_id(node, parents);
	any_array_push(g_context->node_previews_used, id); // Keeps the baked result
	make_flood_fill_state_t *s = make_flood_fill_get_state(id);
	s->material_id             = g_context->material->id;
	s->node                    = node;
	if (linked || bake_texture_node_baking) {
		// No readback during strokes or while Bake Texture swaps the layer
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
