
#include "../global.h"

#if defined(IRON_ANDROID) || defined(IRON_IOS)
i32 layers_max_layers = 18;
#else
i32 layers_max_layers = 255;
#endif

uv_type_t _layers_uv_type;
mat4_t    _layers_decal_mat;
i32       _layers_position;
i32       _layers_base_color;
f32       _layers_occlusion;
f32       _layers_roughness;
f32       _layers_metallic;

static void util_layer_draw_preview(slot_layer_t *l, gpu_texture_t *target) {
	paint_tex_t type = tab_layers_preview_tex;
	if (slot_layer_is_mask(l) || (type == PAINT_TEX_NORMAL && l->texpaint_nor == NULL) || (type >= PAINT_TEX_OCCLUSION && l->texpaint_pack == NULL)) {
		type = PAINT_TEX_BASE;
	}

	if (type == PAINT_TEX_BASE) {
		draw_begin(target, true, 0x00000000);
		// draw_set_pipeline(l.is_mask() ? pipes_copy8 : pipes_copy);
		draw_set_pipeline(pipes_copy); // texpaint_preview is always RGBA32 for now
		draw_scaled_image(l->texpaint, 0, 0, target->width, target->height);
		draw_set_pipeline(NULL);
		draw_end();
		return;
	}

	gpu_texture_t *source  = type == PAINT_TEX_OPACITY ? l->texpaint : type == PAINT_TEX_NORMAL ? l->texpaint_nor : l->texpaint_pack;
	i32            channel = type == PAINT_TEX_OCCLUSION   ? 1
	                         : type == PAINT_TEX_ROUGHNESS ? 2
	                         : type == PAINT_TEX_METALLIC  ? 3
	                         : type == PAINT_TEX_NORMAL    ? 5
	                                                       : 4; // Opacity, height
	draw_begin(target, true, 0xff000000);                    // view2d pipe keeps dst alpha
	draw_set_pipeline(ui_view2d_pipe);
	gpu_set_int(ui_view2d_channel_loc, channel);
	draw_scaled_image(source, 0, 0, target->width, target->height);
	draw_set_pipeline(NULL);
	draw_end();
}

void util_layer_update_preview() {
	if (g_context->layers_preview_dirty) {
		g_context->layers_preview_dirty = false;
		g_context->layer_preview_dirty  = false;
		g_context->mask_preview_last    = NULL;
		// Update all layer previews
		for (i32 i = 0; i < g_project->_->layers->length; ++i) {
			slot_layer_t *l = g_project->_->layers->buffer[i];
			if (slot_layer_is_group(l)) {
				continue;
			}

			gpu_texture_t *target = l->texpaint_preview;
			if (target == NULL) {
				continue;
			}

			util_layer_draw_preview(l, target);
		}
		ui_dock_redraw(TAB_AREA_SIDEBAR0);
	}

	if (g_context->layer != NULL && g_context->layer_preview_dirty && !slot_layer_is_group(g_context->layer)) {
		g_context->layer_preview_dirty = false;
		g_context->mask_preview_last   = NULL;
		// Update layer preview
		slot_layer_t  *l      = g_context->layer;
		gpu_texture_t *target = l->texpaint_preview;
		if (target != NULL) {
			util_layer_draw_preview(l, target);
			ui_dock_redraw(TAB_AREA_SIDEBAR0);
		}
	}
}

void layers_init() {
	slot_layer_clear(g_project->_->layers->buffer[0], color_from_floats(layers_default_base, layers_default_base, layers_default_base, 1.0), NULL, 1.0,
	                 layers_default_rough, 0.0);
}

void layers_resize_undo_steps() {
	if (util_udim_res_x() >= 8192 || util_udim_res_y() >= 8192) { // Save memory for >=8k
		g_config->undo_steps = util_udim_res_x() == 8192 ? 4 : 1;
		while (history_undo_layers->length > g_config->undo_steps) {
			slot_layer_t *l = array_pop(history_undo_layers);
			sys_notify_on_next_frame(&slot_layer_unload, l);
		}
	}
}

void layers_resize() {
	layers_resize_undo_steps();
	for (i32 i = 0; i < g_project->_->layers->length; ++i) {
		slot_layer_t *l = g_project->_->layers->buffer[i];
		slot_layer_resize_and_set_bits(l);
	}
	layers_resize_targets();

	render_path_raytrace_ready = false; // Rebuild baketex
	g_context->ddirty          = 2;
	layers_crop                = false;
}

// Undo layers, live layer and paint targets that follow the texture resolution
void layers_resize_targets() {
	if (history_undo_layers != NULL) {
		for (i32 i = 0; i < history_undo_layers->length; ++i) {
			slot_layer_t *l = history_undo_layers->buffer[i];
			slot_layer_resize_and_set_bits(l);
		}
	}

	slot_layer_t_array_t *saved = history_merge_saved_layers();
	for (i32 i = 0; i < saved->length; ++i) {
		slot_layer_resize_and_set_bits(saved->buffer[i]);
	}

	any_map_t *rts = render_path_render_targets;

	render_target_t *blend0           = any_map_get(rts, "texpaint_blend0");
	gpu_texture_t   *_texpaint_blend0 = blend0->_image;
	gpu_delete_texture(_texpaint_blend0);
	blend0->width  = util_udim_res_x();
	blend0->height = util_udim_res_y();
	blend0->_image = gpu_create_render_target(util_udim_res_x(), util_udim_res_y(), GPU_TEXTURE_FORMAT_R8);

	render_target_t *blend1           = any_map_get(rts, "texpaint_blend1");
	gpu_texture_t   *_texpaint_blend1 = blend1->_image;
	gpu_delete_texture(_texpaint_blend1);
	blend1->width  = util_udim_res_x();
	blend1->height = util_udim_res_y();
	blend1->_image = gpu_create_render_target(util_udim_res_x(), util_udim_res_y(), GPU_TEXTURE_FORMAT_R8);

	g_context->brush_blend_dirty = true;

	render_target_t *blur = any_map_get(rts, "texpaint_blur");
	if (blur != NULL) {
		gpu_texture_t *_texpaint_blur = blur->_image;
		gpu_delete_texture(_texpaint_blur);
		f32 size_x   = math_floor(util_udim_res_x() * 0.95);
		f32 size_y   = math_floor(util_udim_res_y() * 0.95);
		blur->width  = size_x;
		blur->height = size_y;
		blur->_image = gpu_create_render_target(size_x, size_y, GPU_TEXTURE_FORMAT_RGBA32);
	}
	if (render_path_paint_live_layer != NULL) {
		slot_layer_resize_and_set_bits(render_path_paint_live_layer);
	}

	render_target_t *sculpt_ref = any_map_get(rts, "texpaint_sculpt_ref");
	if (sculpt_ref != NULL) {
		gpu_delete_texture(sculpt_ref->_image);
		sculpt_ref->width  = config_get_texture_res_x();
		sculpt_ref->height = config_get_texture_res_y();
		sculpt_ref->_image = gpu_create_render_target(config_get_texture_res_x(), config_get_texture_res_y(), GPU_TEXTURE_FORMAT_RGBA128);
	}

	render_target_t *sculpt_base = any_map_get(rts, "texpaint_sculpt_base");
	if (sculpt_base != NULL) {
		gpu_delete_texture(sculpt_base->_image);
		sculpt_base->width  = config_get_texture_res_x();
		sculpt_base->height = config_get_texture_res_y();
		sculpt_base->_image = gpu_create_render_target(config_get_texture_res_x(), config_get_texture_res_y(), GPU_TEXTURE_FORMAT_RGBA128);
		sculpt_pack_base();
	}
}

void layers_set_bits() {
	for (i32 i = 0; i < g_project->_->layers->length; ++i) {
		slot_layer_t *l = g_project->_->layers->buffer[i];
		slot_layer_resize_and_set_bits(l);
	}
	for (i32 i = 0; i < history_undo_layers->length; ++i) {
		slot_layer_t *l = history_undo_layers->buffer[i];
		slot_layer_resize_and_set_bits(l);
	}
	slot_layer_t_array_t *saved = history_merge_saved_layers();
	for (i32 i = 0; i < saved->length; ++i) {
		slot_layer_resize_and_set_bits(saved->buffer[i]);
	}
}

void layers_make_temp_img() {
	slot_layer_t *l = g_project->_->layers->buffer[0];

	if (layers_temp_image != NULL && (layers_temp_image->width != l->texpaint->width || layers_temp_image->height != l->texpaint->height ||
	                                  layers_temp_image->format != l->texpaint->format)) {
		render_target_t *_temptex0 = any_map_get(render_path_render_targets, "temptex0");
		gpu_delete_texture(_temptex0->_image);
		map_delete(render_path_render_targets, "temptex0");
		layers_temp_image = NULL;
	}

	if (layers_temp_image == NULL) {
		char            *format = base_bits == TEXTURE_BITS_BITS8 ? "RGBA32" : base_bits == TEXTURE_BITS_BITS16 ? "RGBA64" : "RGBA128";
		render_target_t *t      = render_target_create();
		t->name                 = "temptex0";
		t->width                = l->texpaint->width;
		t->height               = l->texpaint->height;
		t->format               = string_copy(format);
		render_target_t *rt     = render_path_create_render_target(t);
		layers_temp_image       = rt->_image;
	}
}

void layers_make_temp_mask_img() {
	if (pipes_temp_mask_image != NULL &&
	    (pipes_temp_mask_image->width != util_udim_res_x() || pipes_temp_mask_image->height != util_udim_res_y())) {
		gpu_texture_t *_temp_mask_image = pipes_temp_mask_image;
		gpu_delete_texture(_temp_mask_image);
		pipes_temp_mask_image = NULL;
	}

	if (pipes_temp_mask_image == NULL) {
		// pipes_temp_mask_image = gpu_create_render_target(config_get_texture_res_x(), config_get_texture_res_y(), GPU_TEXTURE_FORMAT_R8);
		pipes_temp_mask_image = gpu_create_render_target(util_udim_res_x(), util_udim_res_y(), GPU_TEXTURE_FORMAT_RGBA32);
	}
}

void layers_make_export_img() {
	slot_layer_t *l = g_project->_->layers->buffer[0];
	if (layers_expa != NULL &&
	    (layers_expa->width != l->texpaint->width || layers_expa->height != l->texpaint->height || layers_expa->format != l->texpaint->format)) {
		gpu_texture_t *_expa = layers_expa;
		gpu_texture_t *_expb = layers_expb;
		gpu_texture_t *_expc = layers_expc;
		gpu_delete_texture(_expa);
		gpu_delete_texture(_expb);
		gpu_delete_texture(_expc);
		layers_expa = NULL;
		layers_expb = NULL;
		layers_expc = NULL;
		map_delete(render_path_render_targets, "expa");
		map_delete(render_path_render_targets, "expb");
		map_delete(render_path_render_targets, "expc");
	}
	if (layers_expa == NULL) {
		char *format = base_bits == TEXTURE_BITS_BITS8 ? "RGBA32" : base_bits == TEXTURE_BITS_BITS16 ? "RGBA64" : "RGBA128";
		{
			render_target_t *t  = render_target_create();
			t->name             = "expa";
			t->width            = l->texpaint->width;
			t->height           = l->texpaint->height;
			t->format           = string_copy(format);
			render_target_t *rt = render_path_create_render_target(t);
			layers_expa         = rt->_image;
		}
		{
			render_target_t *t  = render_target_create();
			t->name             = "expb";
			t->width            = l->texpaint->width;
			t->height           = l->texpaint->height;
			t->format           = string_copy(format);
			render_target_t *rt = render_path_create_render_target(t);
			layers_expb         = rt->_image;
		}
		{
			render_target_t *t  = render_target_create();
			t->name             = "expc";
			t->width            = l->texpaint->width;
			t->height           = l->texpaint->height;
			t->format           = string_copy(format);
			render_target_t *rt = render_path_create_render_target(t);
			layers_expc         = rt->_image;
		}
	}
}

void layers_commands_merge_pack(gpu_pipeline_t *pipe, gpu_texture_t *i0, gpu_texture_t *i1, gpu_texture_t *i1pack, f32 i1mask_opacity, gpu_texture_t *i1texmask,
                                i32 i1blending) {
	_gpu_begin(i0, NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
	gpu_set_pipeline(pipe);
	gpu_set_texture(pipes_tex0, i1);
	gpu_set_texture(pipes_tex1, i1pack);
	gpu_set_texture(pipes_texmask, i1texmask);
	gpu_set_texture(pipes_texa, layers_temp_image);
	gpu_set_float(pipes_opac, i1mask_opacity);
	gpu_set_float(pipes_tex1w, i1pack->width);
	gpu_set_int(pipes_blending, i1blending);
	gpu_set_vertex_buffer(const_data_screen_aligned_vb);
	gpu_set_index_buffer(const_data_screen_aligned_ib);
	gpu_draw();
	gpu_end();
}

static void layers_enable_translucency_on_next_frame(void *_) {
	make_material_parse_mesh_material();
	make_material_parse_paint_material(false);
	util_render_make_material_preview();
}

// The bottom layer alpha is the surface opacity - once something can lower it, meshes turn see-through and stay so
void layers_enable_translucency() {
	if (g_project->_->translucency) {
		return;
	}
	g_project->_->translucency = true;
	sys_notify_on_next_frame(&layers_enable_translucency_on_next_frame, NULL);
}

bool layers_is_fill_material() {
	if (g_context->tool == TOOL_TYPE_MATERIAL) {
		return true;
	}

	slot_material_t *m = g_context->material;
	for (i32 i = 0; i < g_project->_->layers->length; ++i) {
		slot_layer_t *l = g_project->_->layers->buffer[i];
		if (l->fill_material == m) {
			return true;
		}
	}
	return false;
}

void layers_update_fill_layers() {
	if (bake_texture_node_baking)
		return;

	slot_layer_t  *_layer     = g_context->layer;
	tool_type_t    _tool      = g_context->tool;
	i32            _fill_type = g_context->fill_type;
	gpu_texture_t *current    = NULL;

	if (g_context->tool == TOOL_TYPE_MATERIAL) {
		if (render_path_paint_live_layer == NULL) {
			render_path_paint_live_layer = slot_layer_create("_live", LAYER_SLOT_TYPE_LAYER, NULL);
		}

		current     = _draw_current;
		bool in_use = gpu_in_use;
		if (in_use)
			draw_end();

		g_context->tool      = TOOL_TYPE_FILL;
		g_context->fill_type = FILL_TYPE_OBJECT;
		render_path_paint_set_plane_mesh();
		make_material_parse_paint_material(false);
		g_context->pdirty = 1;
		render_path_paint_use_live_layer(true);
		render_path_paint_commands_paint(false);
		render_path_paint_dilate(true, true);
		render_path_paint_use_live_layer(false);
		g_context->tool      = _tool;
		g_context->fill_type = _fill_type;
		g_context->pdirty    = 0;
		render_path_paint_restore_plane_mesh();
		make_material_parse_paint_material(true);
		ui_view2d_hwnd->redraws = 2;

		if (in_use)
			draw_begin(current, false, 0);
		return;
	}

	bool has_fill_layer = false;
	bool has_fill_mask  = false;
	for (i32 i = 0; i < g_project->_->layers->length; ++i) {
		slot_layer_t *l = g_project->_->layers->buffer[i];
		if (slot_layer_is_layer(l) && l->fill_material == g_context->material) {
			has_fill_layer = true;
		}
	}
	for (i32 i = 0; i < g_project->_->layers->length; ++i) {
		slot_layer_t *l = g_project->_->layers->buffer[i];
		if (slot_layer_is_mask(l) && l->fill_material == g_context->material) {
			has_fill_mask = true;
		}
	}

	if (has_fill_layer || has_fill_mask) {
		current     = _draw_current;
		bool in_use = gpu_in_use;
		if (in_use)
			draw_end();
		g_context->pdirty    = 1;
		g_context->tool      = TOOL_TYPE_FILL;
		g_context->fill_type = FILL_TYPE_OBJECT;

		if (has_fill_layer) {
			bool first  = true;
			bool tested = false;
			for (i32 i = 0; i < g_project->_->layers->length; ++i) {
				slot_layer_t *l = g_project->_->layers->buffer[i];
				if (slot_layer_is_layer(l) && l->fill_material == g_context->material) {
					g_context->layer = l;
					if (first || tested || layer_tiles_tested(l)) { // The tile discard differs per layer
						first  = false;
						tested = layer_tiles_tested(l);
						make_material_parse_paint_material(false);
					}
					layers_update_paint_set();
					if (l->texpaint_sculpt != NULL) {
						i32 tid = l->id;
						i32 hid = history_undo_i - 1 < 0 ? g_config->undo_steps - 1 : history_undo_i - 1;
						sculpt_import_mesh_pack_to_texture(l->texpaint_sculpt);
						render_path_set_target(string_tmp("texpaint_sculpt_undo%d", hid), NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
						render_path_bind_target(string("texpaint_sculpt%d", tid), "tex");
						render_path_draw_shader("Scene/copy_pass/copyRGBA128_pass");
						render_path_sculpt_commands();
					}
					else {
						slot_layer_clear(l, 0x00000000, NULL, 1.0, layers_default_rough, 0.0);
						render_path_paint_commands_paint(false);
						render_path_paint_dilate(true, true);
					}
				}
			}
		}
		if (has_fill_mask) {
			bool first  = true;
			bool tested = false;
			for (i32 i = 0; i < g_project->_->layers->length; ++i) {
				slot_layer_t *l = g_project->_->layers->buffer[i];
				if (slot_layer_is_mask(l) && l->fill_material == g_context->material) {
					g_context->layer = l;
					if (first || tested || layer_tiles_tested(l)) { // The tile discard differs per layer
						first  = false;
						tested = layer_tiles_tested(l);
						make_material_parse_paint_material(false);
					}
					layers_update_paint_set();
					slot_layer_clear(l, 0x00000000, NULL, 1.0, layers_default_rough, 0.0);
					render_path_paint_commands_paint(false);
					render_path_paint_dilate(true, true);
				}
			}
		}
		g_context->pdirty               = 0;
		g_context->ddirty               = 2;
		g_context->rtdirty              = 1;
		g_context->layers_preview_dirty = true;
		if (in_use)
			draw_begin(current, false, 0);
		g_context->layer = _layer;
		layers_update_paint_set();
		g_context->tool      = _tool;
		g_context->fill_type = _fill_type;
		make_material_parse_paint_material(false);
	}
}

bool layers_is_path_material() {
	slot_material_t *m = g_context->material;
	for (i32 i = 0; i < g_project->_->layers->length; ++i) {
		slot_layer_t *l = g_project->_->layers->buffer[i];
		if (slot_layer_is_path(l) && l->path_material == m) {
			return true;
		}
	}
	return false;
}

void layers_update_path_layers() {
	for (i32 i = 0; i < g_project->_->layers->length; ++i) {
		slot_layer_t *l = g_project->_->layers->buffer[i];
		if (slot_layer_is_path(l) && l->path_material == g_context->material) {
			util_layer_repaint_path(l);
		}
	}
}

bool layers_tool_paints_fill_layer(tool_type_t tool) {
	return tool == TOOL_TYPE_BAKE || tool == TOOL_TYPE_PICKER || tool == TOOL_TYPE_MATERIAL || tool == TOOL_TYPE_COLORID;
}

void layers_update_fill_layer(bool parse_paint) {
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use)
		draw_end();

	tool_type_t _tool      = g_context->tool;
	i32         _fill_type = g_context->fill_type;
	g_context->tool        = TOOL_TYPE_FILL;
	g_context->fill_type   = FILL_TYPE_OBJECT;
	g_context->pdirty      = 1;

	if (g_context->layer->texpaint_sculpt != NULL) {
		i32 tid = g_context->layer->id;
		i32 hid = history_undo_i - 1 < 0 ? g_config->undo_steps - 1 : history_undo_i - 1;
		sculpt_import_mesh_pack_to_texture(g_context->layer->texpaint_sculpt);
		render_path_set_target(string_tmp("texpaint_sculpt_undo%d", hid), NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
		render_path_bind_target(string("texpaint_sculpt%d", tid), "tex");
		render_path_draw_shader("Scene/copy_pass/copyRGBA128_pass");
		if (parse_paint) {
			make_material_parse_paint_material(false);
		}
		render_path_sculpt_commands();
	}
	else {
		slot_layer_clear(g_context->layer, 0x00000000, NULL, 1.0, layers_default_rough, 0.0);
		if (parse_paint) {
			make_material_parse_paint_material(false);
		}
		render_path_paint_commands_paint(false);
		render_path_paint_dilate(true, true);
	}

	g_context->tool      = _tool;
	g_context->fill_type = _fill_type;
	if (parse_paint && layers_tool_paints_fill_layer(_tool)) {
		make_material_parse_paint_material(false);
	}
	if (in_use)
		draw_begin(current, false, 0);
}

void layers_update_linked_layers() {
	nodes_mesh_layer_changed(g_context->layer);
	slot_material_t *_material  = g_context->material;
	bool             any_linked = false;
	for (i32 i = 0; i < g_project->_->materials->length; ++i) {
		slot_material_t *m          = g_project->_->materials->buffer[i];
		bool             has_linked = false;
		for (i32 j = 0; j < m->canvas->nodes->length; ++j) {
			ui_node_t *node = m->canvas->nodes->buffer[j];
			if (string_equals(node->type, "LAYER") || string_equals(node->type, "LAYER_MASK")) {
				has_linked = true;
				break;
			}
		}
		if (!has_linked) {
			continue;
		}
		any_linked          = true;
		g_context->material = m;
		make_material_bake_node_previews_linked();
		layers_update_fill_layers();
	}
	g_context->material = _material;
	if (any_linked) {
		make_material_parse_paint_material(false);
	}
}

static i32 layers_filled_frame = -1;

// Refill the layers and masks of materials with a shader node reading the timeline frame when the frame changes,
// on playback, scrubbing or script_timeline_set_frame()
void layers_update_frame_fill_layers() {
	if (tab_timeline_selected_frame == layers_filled_frame || bake_texture_node_baking) {
		return;
	}
	bool first          = layers_filled_frame < 0;
	layers_filled_frame = tab_timeline_selected_frame;
	if (first) {
		// Layers were filled at this frame already
		return;
	}
	slot_material_t *_material = g_context->material;
	tool_type_t      _tool     = g_context->tool;
	bool             any_frame = false;
	for (i32 i = 0; i < g_project->_->materials->length; ++i) {
		slot_material_t *m         = g_project->_->materials->buffer[i];
		bool             has_frame = false;
		for (i32 j = 0; j < m->canvas->nodes->length; ++j) {
			ui_node_t *node = m->canvas->nodes->buffer[j];
			if (string_equals(node->type, "SHADER_GPU") && shader_node_reads_frame(node)) {
				has_frame = true;
				break;
			}
		}
		if (!has_frame) {
			continue;
		}
		any_frame           = true;
		g_context->material = m;
		g_context->tool     = TOOL_TYPE_FILL;
		layers_update_fill_layers();
		g_context->tool = _tool;
	}
	g_context->material = _material;
	if (any_frame) {
		make_material_parse_paint_material(false);
	}
}

static bool layers_paint_set_empty = false;

// Painting draws nothing while the layer covers no visible object
bool layers_paint_blocked() {
	return layers_paint_set_empty;
}

// Geometry the uv maps are made from, see util_uv_paint_geometry
static mesh_data_t *layers_paint_geometry() {
	mesh_object_t *merged = g_context->merged_object;
	if (merged != NULL && merged->base->visible) {
		return merged->data;
	}
	return g_context->paint_object != NULL ? g_context->paint_object->data : NULL;
}

// The merged mesh holds the visible objects the active layer covers
void layers_update_paint_set() {
	slot_layer_t *l         = g_context->layer;
	i32           filter    = g_context->layer_filter_object;
	mesh_data_t  *_geometry = layers_paint_geometry();
	layers_paint_set_empty  = false;

	if (layer_covers_nothing(l)) {
		layers_paint_set_empty = true;
	}
	else if (!layer_is_restricted(l) && filter == 0) {
		if (g_context->merged_object == NULL || g_context->merged_object_is_subset) {
			util_mesh_merge_if_changed(NULL);
		}
		context_select_paint_object(context_main_object());
		if (g_context->merged_object != NULL) {
			g_context->merged_object->base->visible = true;
		}
	}
	else {
		mesh_object_t_array_t *covered = layer_coverage_objects(l);
		mesh_object_t_array_t *set     = any_array_create_from_raw((void *[]){}, 0);
		for (i32 i = 0; i < covered->length; ++i) {
			if (covered->buffer[i]->base->visible) {
				any_array_push(set, covered->buffer[i]);
			}
		}
		// Objects rendered with their own material are not painted, falling back to an uncovered object would paint it
		mesh_object_t_array_t *paintable = util_mesh_paintable(set);
		layers_paint_set_empty           = paintable->length == 0;
		if (!layers_paint_set_empty) {
			util_mesh_merge_if_changed(set);
			context_select_paint_object(context_main_object());
			if (g_context->merged_object != NULL) {
				g_context->merged_object->base->visible = true;
			}
		}
		array_delete(covered);
		array_delete(set);
		array_delete(paintable);
	}
	// The dilate map only depends on the painted geometry, a rebuilt merged mesh resets it in util_mesh_merge
	if (layers_paint_geometry() != _geometry) {
		util_uv_dilatemap_cached = false;
	}
}

void tab_layers_apply_filter(i32 filter) {
	g_context->layer_filter_object = filter;
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		mesh_object_t *p = g_project->_->paint_objects->buffer[i];
		p->base->visible = filter == 0 || p->paint_id == filter;
	}
	if (filter == 0 && g_context->merged_object_is_subset) {
		util_mesh_merge(NULL);
	}
	layers_update_paint_set(); // A filter paints a subset merge of the visible covered objects
	util_uv_uvmap_cached       = false;
	g_context->ddirty          = 2;
	render_path_raytrace_ready = false;
}

void layers_new_layer_clear(slot_layer_t *l) {
	if (array_index_of(g_project->_->layers, l) < 0) {
		return;
	}
	if (slot_layer_is_path(l) && l->path_points->length > 0) {
		return;
	}
	slot_layer_clear(l, 0x00000000, NULL, 1.0, layers_default_rough, 0.0);
}

slot_layer_t *layers_new_layer(bool clear, i32 position, slot_layer_t *parent) {
	if (g_project->_->layers->length > layers_max_layers) {
		return NULL;
	}

	slot_layer_t *l = slot_layer_create("", LAYER_SLOT_TYPE_LAYER, parent);
	slot_layer_coverage_from_filter(l);

	if (position == -1 && slot_layer_is_filter(l)) {
		position = array_index_of(g_project->_->layers, parent);
	}

	if (position == -1) {
		if (slot_layer_is_mask(g_context->layer) || slot_layer_is_filter(g_context->layer))
			context_set_layer(g_context->layer->parent);
		array_insert(g_project->_->layers, array_index_of(g_project->_->layers, g_context->layer) + 1, l);
	}
	else {
		array_insert(g_project->_->layers, position, l);
	}

	context_set_layer(l);

	if (parent == NULL) {
		i32 li = array_index_of(g_project->_->layers, g_context->layer);
		if (li > 0) {
			slot_layer_t *below = g_project->_->layers->buffer[li - 1];
			if (slot_layer_is_layer(below) || slot_layer_is_group(below)) {
				g_context->layer->parent = below->parent;
			}
		}
	}

	if (clear) {
		sys_notify_on_next_frame(&layers_new_layer_clear, l);
	}

	tab_stages_add_layer(l->name);

	g_context->layer_preview_dirty = true;
	return l;
}

void layers_new_mask_clear(slot_layer_t *l) {
	slot_layer_clear(l, 0x00000000, NULL, 1.0, layers_default_rough, 0.0);
}

slot_layer_t *layers_new_mask(bool clear, slot_layer_t *parent, i32 position) {
	if (g_project->_->layers->length > layers_max_layers) {
		return NULL;
	}

	slot_layer_t *l = slot_layer_create("", LAYER_SLOT_TYPE_MASK, parent);
	if (position == -1) {
		position = array_index_of(g_project->_->layers, parent);
	}
	array_insert(g_project->_->layers, position, l);
	context_set_layer(l);
	if (clear) {
		sys_notify_on_next_frame(&layers_new_mask_clear, l);
	}
	tab_stages_add_layer(l->name);
	g_context->layer_preview_dirty = true;
	return l;
}

slot_layer_t *layers_new_group() {
	if (g_project->_->layers->length > layers_max_layers) {
		return NULL;
	}

	slot_layer_t *l = slot_layer_create("", LAYER_SLOT_TYPE_GROUP, NULL);
	any_array_push(g_project->_->layers, l);
	context_set_layer(l);
	tab_stages_add_layer(l->name);
	return l;
}

static slot_layer_t *_layers_path_sculpt_layer = NULL;

void layers_new_path_layer_sculpt_init(void *_) {
	sculpt_init();
	sculpt_init_sculpt_texture(_layers_path_sculpt_layer);
	make_material_parse_mesh_material();
}

slot_layer_t *layers_new_path_layer(bool curved) {
	slot_layer_t *l = layers_new_layer(true, -1, NULL);
	if (l == NULL) {
		return NULL;
	}
	l->path_points        = f32_array_create(0);
	l->path_points_world  = f32_array_create(0);
	l->path_points_camera = f32_array_create(0);
	l->path_points_parent = i32_array_create(0);
	l->path_tool          = -1;
	l->path_curved        = curved;
	l->path_material      = g_context->material;
	char *name            = string_copy(slot_layer_unique_name(l, string_tmp(curved ? "Curve %d" : "Path %d", l->id + 1)));
	tab_stages_rename_layer(l->name, name);
	l->name = name;

	if (g_config->tools == TOOLS_SCULPT) {
		_layers_path_sculpt_layer = l;
		sys_notify_on_next_frame(&layers_new_path_layer_sculpt_init, NULL);
	}

	return l;
}

slot_layer_t *layers_new_text_layer() {
	slot_layer_t *l = layers_new_path_layer(true);
	if (l == NULL) {
		return NULL;
	}
	l->path_text = true;
	l->path_tool = TOOL_TYPE_TEXT;
	char *name   = string_copy(slot_layer_unique_name(l, string_tmp("Text %d", l->id + 1)));
	tab_stages_rename_layer(l->name, name);
	l->name = name;
	return l;
}

void layers_create_fill_layer_on_next_frame(void *_) {
	slot_layer_t *l = layers_new_layer(false, _layers_position, NULL);
	history_new_layer();
	l->uv_type = _layers_uv_type;
	if (!mat4_isnan(_layers_decal_mat)) {
		l->decal_mat = _layers_decal_mat;
	}
	slot_layer_coverage_from_filter(l);

	if (g_config->tools == TOOLS_SCULPT) {
		sculpt_init();
		sculpt_init_sculpt_texture(l);
	}

	history_to_fill_layer();
	slot_layer_to_fill_layer(l);
}

void layers_create_fill_layer(uv_type_t uv_type, mat4_t decal_mat, i32 position) {
	_layers_uv_type   = uv_type;
	_layers_decal_mat = decal_mat;
	_layers_position  = position;
	sys_notify_on_next_frame(&layers_create_fill_layer_on_next_frame, NULL);
}

void tab_materials_button_new_on_next_frame(void *_);

void layers_create_filter_on_next_frame(void *_) {
	if (slot_layer_get_filters(g_context->layer, true) != NULL) {
		return;
	}

	tab_materials_button_new_on_next_frame(NULL);

	slot_layer_t *l = layers_new_layer(false, -1, g_context->layer);
	history_new_layer();
	history_to_fill_layer();
	slot_layer_to_fill_layer(l);

	// Filter material
	g_context->material->canvas->name = string_copy(g_context->layer->name);

	ui_nodes_t       *nodes  = g_context->material->nodes;
	ui_node_canvas_t *canvas = g_context->material->canvas;
	ui_node_t        *nout   = NULL;
	for (i32 i = 0; i < canvas->nodes->length; ++i) {
		ui_node_t *n = canvas->nodes->buffer[i];
		if (string_equals(n->type, "OUTPUT_MATERIAL_PBR")) {
			nout = n;
			break;
		}
	}
	for (i32 i = 0; i < canvas->nodes->length; ++i) {
		ui_node_t *n = canvas->nodes->buffer[i];
		if (string_equals(n->name, "Color")) {
			ui_remove_node(n, canvas);
			break;
		}
	}

	ui_node_t *n                         = nodes_material_create_node("LAYER", NULL);
	n->buttons->buffer[0]->default_value = f32_array_create_x(array_index_of(g_project->_->layers, l->parent));
	n->x                                 = -50;
	n->y                                 = 100;

	for (int i = 0; i < 9; ++i) {
		ui_node_link_t *l =
		    ALLOC_INIT(ui_node_link_t, {.id = ui_next_link_id(canvas->links), .from_id = n->id, .from_socket = i, .to_id = nout->id, .to_socket = i});
		any_array_push(canvas->links, l);
	}

	layers_update_fill_layer(true);
	util_render_make_material_preview();
}

void layers_create_filter() {
	sys_notify_on_next_frame(&layers_create_filter_on_next_frame, NULL);
}

void layers_create_image_mask(asset_t *asset) {
	slot_layer_t *l = g_context->layer;
	if (slot_layer_is_mask(l) || slot_layer_is_group(l)) {
		return;
	}

	history_new_layer();
	slot_layer_t *m = layers_new_mask(false, l, -1);
	slot_layer_clear(m, 0x00000000, project_get_image(asset), 1.0, layers_default_rough, 0.0);
	g_context->layer_preview_dirty = true;
}

void layers_create_image_layer(asset_t *asset) {
	history_new_layer();
	slot_layer_t *m = layers_new_layer(false, -1, NULL);
	slot_layer_clear(m, 0x00000000, project_get_image(asset), 1.0, layers_default_rough, 0.0);
	g_context->layer_preview_dirty = true;
}

void layers_create_color_layer_on_next_frame(void *_) {
	slot_layer_t *l = layers_new_layer(false, _layers_position, NULL);
	history_new_layer();
	l->uv_type = UV_TYPE_UVMAP;
	slot_layer_coverage_from_filter(l);
	slot_layer_clear(l, _layers_base_color, NULL, _layers_occlusion, _layers_roughness, _layers_metallic);
}

void layers_create_color_layer(i32 base_color, f32 occlusion, f32 roughness, f32 metallic, i32 position) {
	_layers_base_color = base_color;
	_layers_occlusion  = occlusion;
	_layers_roughness  = roughness;
	_layers_metallic   = metallic;
	_layers_position   = position;

	sys_notify_on_next_frame(&layers_create_color_layer_on_next_frame, NULL);
}

static void layers_duplicate_children(slot_layer_t *l, slot_layer_t *new_parent) {
	slot_layer_t_array_t *children = slot_layer_get_direct_children(l);
	for (i32 i = 0; i < children->length; ++i) {
		slot_layer_t *c     = children->buffer[i];
		slot_layer_t *new_c = slot_layer_is_group(c) ? layers_new_group() : slot_layer_duplicate(c);
		new_c->parent       = new_parent;
		array_remove(g_project->_->layers, new_c);
		array_insert(g_project->_->layers, array_index_of(g_project->_->layers, new_parent), new_c);
		layers_duplicate_children(c, new_c);
	}
}

void layers_duplicate_layer(slot_layer_t *l) {
	slot_layer_t *new_layer = slot_layer_is_group(l) ? layers_new_group() : slot_layer_duplicate(l);
	new_layer->parent       = l->parent;
	array_remove(g_project->_->layers, new_layer);
	array_insert(g_project->_->layers, array_index_of(g_project->_->layers, l) + 1, new_layer);
	layers_duplicate_children(l, new_layer);
	context_set_layer(new_layer);
}

void layers_apply_masks(slot_layer_t *l) {
	slot_layer_t_array_t *masks = slot_layer_get_masks(l, false);

	if (masks != NULL) {
		for (i32 i = 0; i < masks->length; ++i) {
			masks->buffer[i]->visible ? slot_layer_apply_mask(masks->buffer[i]) : slot_layer_delete(masks->buffer[i]);
		}
		g_context->layer_preview_dirty = true;
	}
}

static void layers_bake_opacity(slot_layer_t *l) {
	if (l->mask_opacity < 1.0 && slot_layer_is_mask(l)) {
		layers_make_temp_img();
		draw_begin(layers_temp_image, false, 0);
		draw_set_pipeline(pipes_copy);
		draw_image(l->texpaint, 0, 0);
		draw_set_pipeline(NULL);
		draw_end();
		render_target_t *white = any_map_get(render_path_render_targets, "empty_white");
		_gpu_begin(l->texpaint, NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
		gpu_set_pipeline(pipes_merge_mask);
		gpu_set_texture(pipes_tex0_merge_mask, white->_image);
		gpu_set_texture(pipes_texa_merge_mask, layers_temp_image);
		gpu_set_float(pipes_opac_merge_mask, 1.0 - l->mask_opacity);
		gpu_set_int(pipes_blending_merge_mask, BLEND_TYPE_MIX);
		gpu_set_vertex_buffer(const_data_screen_aligned_vb);
		gpu_set_index_buffer(const_data_screen_aligned_ib);
		gpu_draw();
		gpu_end();
		l->mask_opacity = 1.0;
	}
	else if (l->mask_opacity < 1.0) {
		render_target_t *black = any_map_get(render_path_render_targets, "empty_black");
		slot_layer_t    *m     = ALLOC_INIT(slot_layer_t, {.texpaint = black->_image, .mask_opacity = 1.0 - l->mask_opacity});
		layers_apply_mask(l, m);
		free(m);
		l->mask_opacity = 1.0;
	}
}

void layers_merge_down() {
	slot_layer_t *l1 = g_context->layer;
	slot_layer_t *l0 = g_project->_->layers->buffer[array_index_of(g_project->_->layers, l1) - slot_layer_count_descendants(l1) - 1];

	if (!slot_layer_shows(l1) || !slot_layer_shows(l0)) {
		slot_layer_t *discard = slot_layer_shows(l1) ? l0 : l1;
		slot_layer_t *keep    = discard == l1 ? l0 : l1;
		slot_layer_delete(discard);
		context_set_layer(keep);
		g_context->layer_preview_dirty = true;
		return;
	}

	if (slot_layer_is_group(l1)) {
		l1 = layers_merge_group(l1);
	}
	else if (slot_layer_has_masks(l1, false)) { // It is a layer
		layers_apply_masks(l1);
		context_set_layer(l1);
	}

	if (slot_layer_is_group(l0)) {
		l0 = layers_merge_group(l0);
	}
	else if (slot_layer_has_masks(l0, false)) { // It is a layer
		layers_apply_masks(l0);
		context_set_layer(l0);
	}

	layers_bake_opacity(l0);
	if (slot_layer_is_layer(l0)) {
		slot_layer_coverage_union(l0, l1);
	}
	layers_merge_layer(l0, l1, false);
	slot_layer_delete(l1);
	context_set_layer(l0);
	g_context->layer_preview_dirty = true;
}

slot_layer_t *layers_merge_group(slot_layer_t *l) {
	if (!slot_layer_is_group(l)) {
		return NULL;
	}

	if (!slot_layer_shows(l)) {
		if (tab_layers_can_delete(l)) {
			slot_layer_delete(l);
			for (slot_layer_t *p = l->parent; p != NULL && slot_layer_get_children(p) == NULL; p = p->parent) {
				slot_layer_delete(p);
			}
		}
		return NULL;
	}

	slot_layer_t_array_t *direct = slot_layer_get_direct_children(l);
	for (i32 i = 0; i < direct->length; ++i) {
		if (!slot_layer_shows(direct->buffer[i])) {
			slot_layer_delete(direct->buffer[i]);
		}
	}

	slot_layer_t_array_t *masks = slot_layer_get_masks(l, false);
	for (i32 i = 0; masks != NULL && i < masks->length; ++i) {
		masks->buffer[i]->visible ? slot_layer_apply_mask(masks->buffer[i]) : slot_layer_delete(masks->buffer[i]);
	}

	direct = slot_layer_get_direct_children(l);
	for (i32 i = 0; i < direct->length; ++i) {
		direct->buffer[i]->mask_opacity *= l->mask_opacity;
		if (slot_layer_is_group(direct->buffer[i])) {
			layers_merge_group(direct->buffer[i]);
		}
	}

	slot_layer_t_array_t *children = slot_layer_get_children(l);

	if (children->length == 1 && slot_layer_has_masks(children->buffer[0], false)) {
		layers_apply_masks(children->buffer[0]);
	}

	for (i32 i = 0; i < children->length - 1; ++i) {
		context_set_layer(children->buffer[children->length - 1 - i]);
		layers_merge_down();
	}

	children->buffer[0]->parent = l->parent;
	children->buffer[0]->name   = l->name;
	slot_layer_coverage_intersect(children->buffer[0], l);
	if (children->buffer[0]->fill_material != NULL) {
		slot_layer_to_paint_layer(children->buffer[0]);
	}
	slot_layer_delete(l);
	return children->buffer[0];
}

void layers_merge_layer(slot_layer_t *l0, slot_layer_t *l1, bool use_mask) {
	if (!l1->visible || slot_layer_is_group(l1)) {
		return;
	}

	layers_make_temp_img();

	draw_begin(layers_temp_image, false, 0); // Copy to temp
	draw_set_pipeline(pipes_copy);
	draw_image(l0->texpaint, 0, 0);
	draw_set_pipeline(NULL);
	draw_end();

	render_target_t      *empty_rt = any_map_get(render_path_render_targets, "empty_white");
	gpu_texture_t        *empty    = empty_rt->_image;
	gpu_texture_t        *mask     = empty;
	slot_layer_t_array_t *l1masks  = use_mask ? slot_layer_get_masks(l1, true) : NULL;
	if (l1masks != NULL) {
		// for (let i: i32 = 1; i < l1masks.length - 1; ++i) {
		// 	merge_layer(l1masks[i + 1], l1masks[i]);
		// }
		layers_make_temp_mask_img();
		_gpu_begin(pipes_temp_mask_image, NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
		gpu_set_pipeline(pipes_merge_mask);
		gpu_set_texture(pipes_tex0_merge_mask, l1masks->buffer[0]->texpaint);
		gpu_set_texture(pipes_texa_merge_mask, empty);
		gpu_set_float(pipes_opac_merge_mask, l1masks->buffer[0]->mask_opacity);
		gpu_set_int(pipes_blending_merge_mask, BLEND_TYPE_MIX);
		gpu_set_vertex_buffer(const_data_screen_aligned_vb);
		gpu_set_index_buffer(const_data_screen_aligned_ib);
		gpu_draw();
		gpu_end();
		mask = pipes_temp_mask_image;
	}

	if (slot_layer_is_mask(l1)) {
		_gpu_begin(l0->texpaint, NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
		gpu_set_pipeline(pipes_merge_mask);
		gpu_set_texture(pipes_tex0_merge_mask, l1->texpaint);
		gpu_set_texture(pipes_texa_merge_mask, layers_temp_image);
		gpu_set_float(pipes_opac_merge_mask, l1->mask_opacity);
		gpu_set_int(pipes_blending_merge_mask, BLEND_TYPE_MULTIPLY);
		gpu_set_vertex_buffer(const_data_screen_aligned_vb);
		gpu_set_index_buffer(const_data_screen_aligned_ib);
		gpu_draw();
		gpu_end();
	}

	if (slot_layer_is_layer(l1)) {
		gpu_texture_t *weight = gpu_create_render_target(l0->texpaint->width, l0->texpaint->height, l0->texpaint->format);
		_gpu_begin(weight, NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
		gpu_set_pipeline(pipes_merge);
		gpu_set_texture(pipes_tex0, l1->texpaint);
		gpu_set_texture(pipes_tex1, empty);
		gpu_set_texture(pipes_texmask, mask);
		gpu_set_texture(pipes_texa, layers_temp_image);
		gpu_set_float(pipes_opac, l1->mask_opacity);
		gpu_set_float(pipes_tex1w, empty->width);
		gpu_set_int(pipes_blending, 105);
		gpu_set_vertex_buffer(const_data_screen_aligned_vb);
		gpu_set_index_buffer(const_data_screen_aligned_ib);
		gpu_draw();
		gpu_end();

		if (l1->paint_base) {
			_gpu_begin(l0->texpaint, NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
			gpu_set_pipeline(pipes_merge);
			gpu_set_texture(pipes_tex0, l1->texpaint);
			gpu_set_texture(pipes_tex1, empty);
			gpu_set_texture(pipes_texmask, mask);
			gpu_set_texture(pipes_texa, layers_temp_image);
			gpu_set_float(pipes_opac, l1->mask_opacity);
			gpu_set_float(pipes_tex1w, empty->width);
			gpu_set_int(pipes_blending, 200 + slot_layer_get_blending(l1));
			gpu_set_vertex_buffer(const_data_screen_aligned_vb);
			gpu_set_index_buffer(const_data_screen_aligned_ib);
			gpu_draw();
			gpu_end();
		}

		if (l0->texpaint_nor != NULL) {
			draw_begin(layers_temp_image, false, 0);
			draw_set_pipeline(pipes_copy);
			draw_image(l0->texpaint_nor, 0, 0);
			draw_set_pipeline(NULL);
			draw_end();

			if (l1->paint_nor) {
				_gpu_begin(l0->texpaint_nor, NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
				gpu_set_pipeline(pipes_merge);
				gpu_set_texture(pipes_tex0, weight);
				gpu_set_texture(pipes_tex1, l1->texpaint_nor);
				gpu_set_texture(pipes_texmask, empty);
				gpu_set_texture(pipes_texa, layers_temp_image);
				gpu_set_float(pipes_opac, 1.0);
				gpu_set_float(pipes_tex1w, l1->texpaint_nor->width);
				gpu_set_int(pipes_blending, l1->paint_nor_blend ? 106 : 101);
				gpu_set_vertex_buffer(const_data_screen_aligned_vb);
				gpu_set_index_buffer(const_data_screen_aligned_ib);
				gpu_draw();
				gpu_end();
			}
		}

		if (l0->texpaint_pack != NULL) {
			draw_begin(layers_temp_image, false, 0);
			draw_set_pipeline(pipes_copy);
			draw_image(l0->texpaint_pack, 0, 0);
			draw_set_pipeline(NULL);
			draw_end();

			if (l1->paint_occ || l1->paint_rough || l1->paint_met || l1->paint_height) {
				if (l1->paint_occ && l1->paint_rough && l1->paint_met && l1->paint_height) {
					layers_commands_merge_pack(pipes_merge, l0->texpaint_pack, weight, l1->texpaint_pack, 1.0, empty, l1->paint_height_blend ? 107 : 101);
				}
				else {
					if (l1->paint_occ) {
						layers_commands_merge_pack(pipes_get_merge_r(), l0->texpaint_pack, weight, l1->texpaint_pack, 1.0, empty, 101);
					}
					if (l1->paint_rough) {
						layers_commands_merge_pack(pipes_get_merge_g(), l0->texpaint_pack, weight, l1->texpaint_pack, 1.0, empty, 101);
					}
					if (l1->paint_met) {
						layers_commands_merge_pack(pipes_get_merge_b(), l0->texpaint_pack, weight, l1->texpaint_pack, 1.0, empty, 101);
					}
				}
			}
		}

		gpu_delete_texture(weight);
	}
}

void layers_draw_mesh_materials() {
	bool any = false;
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		if (tab_meshes_get_override(g_project->_->paint_objects->buffer[i]) >= 0) {
			any = true;
			break;
		}
	}
	if (!any) {
		return;
	}

	string_array_t *additional = any_array_create_from_raw(
	    (void *[]){
	        "expb",
	        "expc",
	        "texpaint_blend0",
	    },
	    3);
	render_path_set_target("expa", additional, NULL, GPU_CLEAR_NONE, 0, 0.0);
	render_path_bind_target("main", "gbufferD");
	render_path_bind_target("texpaint_blend1", "paintmask");
	render_path_base_draw_overrides(NULL);
}

static gpu_texture_t *_layers_udim_target(gpu_texture_t **t, gpu_texture_t *like) {
	if (*t != NULL && ((*t)->width != like->width || (*t)->height != like->height || (*t)->format != like->format)) {
		gpu_delete_texture(*t);
		*t = NULL;
	}
	if (*t == NULL) {
		*t = gpu_create_render_target(like->width, like->height, like->format);
	}
	return *t;
}

// Copies the udim tiles found in both layouts from src (old_tiles, old_stride) into their slots of dst (project layout).
// A tile keeps its pixels 1:1, scaled only when the per tile resolution differs (atlas size clamp).
static void _layers_udim_copy_tiles(gpu_texture_t *dst, gpu_texture_t *src, gpu_pipeline_t *pipe, i32_array_t *old_tiles, i32 old_stride) {
	i32 stride = util_udim_stride();
	f32 sw     = src->width / (f32)old_stride;
	f32 sh     = src->height / (f32)old_stride;
	f32 dw     = dst->width / (f32)stride;
	f32 dh     = dst->height / (f32)stride;
	draw_begin(dst, false, 0);
	draw_set_pipeline(pipe);
	for (i32 i = 0; i < old_tiles->length; ++i) {
		i32 slot = util_udim_slot(old_tiles->buffer[i]);
		if (slot < 0) {
			continue;
		}
		draw_scaled_sub_image(src, (i % old_stride) * sw, (i / old_stride) * sh, sw, sh, (slot % stride) * dw, (slot / stride) * dh, dw, dh);
	}
	draw_set_pipeline(NULL);
	draw_end();
}

// New atlas sized texture for the project layout, cleared to color, with the tiles of the old layout copied in
static gpu_texture_t *_layers_udim_relayout_texture(gpu_texture_t *src, u32 color, gpu_pipeline_t *pipe, i32_array_t *old_tiles, i32 old_stride) {
	gpu_texture_t *dst = gpu_create_render_target(util_udim_res_x(), util_udim_res_y(), src->format);
	_gpu_begin(dst, NULL, NULL, GPU_CLEAR_COLOR, color, 0.0);
	gpu_end();
	_layers_udim_copy_tiles(dst, src, pipe, old_tiles, old_stride);
	gpu_delete_texture(src);
	return dst;
}

static void _layers_udim_relayout_layer(slot_layer_t *l, i32_array_t *old_tiles, i32 old_stride) {
	any_map_t *rts = render_path_render_targets;
	if (slot_layer_is_layer(l) && l->texpaint != NULL) {
		gpu_texture_format_t format = l->texpaint->format;
		gpu_pipeline_t      *pipe   = format == GPU_TEXTURE_FORMAT_RGBA32 ? pipes_copy : format == GPU_TEXTURE_FORMAT_RGBA64 ? pipes_copy64 : pipes_copy128;
		// New tiles are cleared like a new layer
		l->texpaint         = _layers_udim_relayout_texture(l->texpaint, 0x00000000, pipe, old_tiles, old_stride);
		render_target_t *rt = any_map_get(rts, string("texpaint%s", l->ext));
		rt->_image          = l->texpaint;
		if (l->texpaint_nor != NULL) {
			l->texpaint_nor         = _layers_udim_relayout_texture(l->texpaint_nor, color_from_floats(0.5, 0.5, 1.0, 0.0), pipe, old_tiles, old_stride);
			render_target_t *rt_nor = any_map_get(rts, string("texpaint_nor%s", l->ext));
			rt_nor->_image          = l->texpaint_nor;
		}
		if (l->texpaint_pack != NULL) {
			l->texpaint_pack =
			    _layers_udim_relayout_texture(l->texpaint_pack, color_from_floats(1.0, layers_default_rough, 0.0, 0.0), pipe, old_tiles, old_stride);
			render_target_t *rt_pack = any_map_get(rts, string("texpaint_pack%s", l->ext));
			rt_pack->_image          = l->texpaint_pack;
		}
	}
	else if (slot_layer_is_mask(l) && l->texpaint != NULL) {
		// New tiles are white, the mask hides nothing there
		l->texpaint         = _layers_udim_relayout_texture(l->texpaint, 0xffffffff, pipes_copy8, old_tiles, old_stride);
		render_target_t *rt = any_map_get(rts, string("texpaint%s", l->ext));
		rt->_image          = l->texpaint;
	}
}

// The project tile list changed from old_tiles (old_stride): every layer texture is moved to the new atlas layout,
// tiles in both lists keep their pixels, new tiles are cleared. Undo layers and paint targets are only resized, history is reset by the caller.
void layers_udim_relayout(i32_array_t *old_tiles, i32 old_stride) {
	layers_resize_undo_steps();
	for (i32 i = 0; i < g_project->_->layers->length; ++i) {
		_layers_udim_relayout_layer(g_project->_->layers->buffer[i], old_tiles, old_stride);
	}
	layers_resize_targets();

	render_path_raytrace_ready     = false; // Rebuild baketex
	g_context->layer_preview_dirty = true;
	g_context->ddirty              = 2;
}

static gpu_texture_t *layers_move_a    = NULL;
static gpu_texture_t *layers_move_b    = NULL;
static gpu_texture_t *layers_move_c    = NULL;
static gpu_texture_t *layers_move_mask = NULL;

static gpu_texture_t *_layers_move_copy(gpu_texture_t **target, gpu_texture_t *src, gpu_pipeline_t *pipe, i32 dx, i32 dy) {
	gpu_texture_t *dst = _layers_udim_target(target, src);
	draw_begin(dst, true, 0x00000000);
	draw_set_pipeline(pipe);
	draw_image(src, dx, dy);
	draw_set_pipeline(NULL);
	draw_end();
	return dst;
}

// Layer with the pending 2D view move applied, the layer itself when it is not being moved
static slot_layer_t *layers_move_preview(slot_layer_t *l) {
	i32 dx, dy;
	if (ui_view2d_move_layer(&dx, &dy) != l) {
		return l;
	}
	static slot_layer_t moved;
	moved = *l;
	if (slot_layer_is_mask(l)) {
		moved.texpaint = _layers_move_copy(&layers_move_mask, l->texpaint, pipes_copy8, dx, dy);
	}
	else {
		gpu_pipeline_t *pipe = base_bits == TEXTURE_BITS_BITS8 ? pipes_copy : base_bits == TEXTURE_BITS_BITS16 ? pipes_copy64 : pipes_copy128;
		moved.texpaint       = _layers_move_copy(&layers_move_a, l->texpaint, pipe, dx, dy);
		moved.texpaint_nor   = _layers_move_copy(&layers_move_b, l->texpaint_nor, pipe, dx, dy);
		moved.texpaint_pack  = _layers_move_copy(&layers_move_c, l->texpaint_pack, pipe, dx, dy);
	}
	return &moved;
}

slot_layer_t *layers_flatten(bool height_to_normal, slot_layer_t_array_t *layers) {
	if (layers == NULL) {
		layers = g_project->_->layers;
	}
	layers_make_temp_img();
	layers_make_export_img();
	render_target_t *empty_rt = any_map_get(render_path_render_targets, "empty_white");
	gpu_texture_t   *empty    = empty_rt->_image;

	// Clear export layer
	_gpu_begin(layers_expa, NULL, NULL, GPU_CLEAR_COLOR, color_from_floats(0.0, 0.0, 0.0, 0.0), 0.0);
	gpu_end();
	_gpu_begin(layers_expb, NULL, NULL, GPU_CLEAR_COLOR, color_from_floats(0.5, 0.5, 1.0, 0.0), 0.0);
	gpu_end();
	_gpu_begin(layers_expc, NULL, NULL, GPU_CLEAR_COLOR, color_from_floats(1.0, 0.0, 0.0, 0.0), 0.0);
	gpu_end();

	// Flatten layers
	for (i32 i = 0; i < layers->length; ++i) {
		slot_layer_t *l1 = layers->buffer[i];
		if (!slot_layer_is_visible(l1)) {
			continue;
		}
		if (!slot_layer_is_layer(l1)) {
			continue;
		}
		if (slot_layer_has_visible_filter(l1)) {
			continue;
		}

		gpu_texture_t        *mask    = empty;
		slot_layer_t_array_t *l1masks = slot_layer_get_masks(l1, true);
		if (l1masks != NULL) {
			layers_make_temp_mask_img();
			draw_begin(pipes_temp_mask_image, GPU_CLEAR_COLOR, 0xffffffff);
			draw_end();
			slot_layer_t *l1 = ALLOC_INIT(slot_layer_t, {.texpaint = pipes_temp_mask_image});
			for (i32 i = 0; i < l1masks->length; ++i) {
				layers_merge_layer(l1, layers_move_preview(l1masks->buffer[i]), false);
			}
			mask = pipes_temp_mask_image;
		}

		l1 = layers_move_preview(l1);

		// Opacity still counts without the base color channel, the path tracer and export read the alpha as opacity
		if (l1->paint_base || l1->paint_opac) {
			draw_begin(layers_temp_image, false, 0); // Copy to temp
			draw_set_pipeline(pipes_copy);
			draw_image(layers_expa, 0, 0);
			draw_set_pipeline(NULL);
			draw_end();

			// if (g_context->tool == TOOL_TYPE_CURSOR) {
			// 	// Do not multiply basecol by alpha
			// 	draw_begin(layers_expa, false, 0); // Copy to temp
			// 	draw_set_pipeline(pipes_copy);
			// 	draw_image(l1->texpaint, 0, 0);
			// 	draw_set_pipeline(NULL);
			// 	draw_end();
			// }
			// else {
			_gpu_begin(layers_expa, NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
			gpu_set_pipeline(l1->paint_base ? pipes_merge : pipes_get_merge_a());
			gpu_set_texture(pipes_tex0, l1->texpaint);
			gpu_set_texture(pipes_tex1, empty);
			gpu_set_texture(pipes_texmask, mask);
			gpu_set_texture(pipes_texa, layers_temp_image);
			gpu_set_float(pipes_opac, slot_layer_get_opacity(l1));
			gpu_set_float(pipes_tex1w, empty->width);
			gpu_set_int(pipes_blending, layers->length > 1 ? slot_layer_get_blending(l1) : 0);
			gpu_set_vertex_buffer(const_data_screen_aligned_vb);
			gpu_set_index_buffer(const_data_screen_aligned_ib);
			gpu_draw();
			gpu_end();
			// }
		}

		if (l1->paint_nor) {
			draw_begin(layers_temp_image, false, 0);
			draw_set_pipeline(pipes_copy);
			draw_image(layers_expb, 0, 0);
			draw_set_pipeline(NULL);
			draw_end();

			_gpu_begin(layers_expb, NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
			gpu_set_pipeline(pipes_merge);
			gpu_set_texture(pipes_tex0, l1->texpaint);
			gpu_set_texture(pipes_tex1, l1->texpaint_nor);
			gpu_set_texture(pipes_texmask, mask);
			gpu_set_texture(pipes_texa, layers_temp_image);
			gpu_set_float(pipes_opac, slot_layer_get_opacity(l1));
			gpu_set_float(pipes_tex1w, l1->texpaint_nor->width);
			gpu_set_int(pipes_blending, l1->paint_nor_blend ? 102 : 101);
			gpu_set_vertex_buffer(const_data_screen_aligned_vb);
			gpu_set_index_buffer(const_data_screen_aligned_ib);
			gpu_draw();
			gpu_end();
		}

		if (l1->paint_occ || l1->paint_rough || l1->paint_met || l1->paint_height) {
			draw_begin(layers_temp_image, false, 0);
			draw_set_pipeline(pipes_copy);
			draw_image(layers_expc, 0, 0);
			draw_set_pipeline(NULL);
			draw_end();

			if (l1->paint_occ && l1->paint_rough && l1->paint_met && l1->paint_height) {
				layers_commands_merge_pack(pipes_merge, layers_expc, l1->texpaint, l1->texpaint_pack, slot_layer_get_opacity(l1), mask,
				                           l1->paint_height_blend ? 103 : 101);
			}
			else {
				if (l1->paint_occ) {
					layers_commands_merge_pack(pipes_get_merge_r(), layers_expc, l1->texpaint, l1->texpaint_pack, slot_layer_get_opacity(l1), mask, 101);
				}
				if (l1->paint_rough) {
					layers_commands_merge_pack(pipes_get_merge_g(), layers_expc, l1->texpaint, l1->texpaint_pack, slot_layer_get_opacity(l1), mask, 101);
				}
				if (l1->paint_met) {
					layers_commands_merge_pack(pipes_get_merge_b(), layers_expc, l1->texpaint, l1->texpaint_pack, slot_layer_get_opacity(l1), mask, 101);
				}
			}
		}
	}

	slot_layer_t *l0 = ALLOC_INIT(slot_layer_t, {.texpaint = layers_expa, .texpaint_nor = layers_expb, .texpaint_pack = layers_expc});

	// Merge height map into normal map
	if (height_to_normal && make_material_height_used) {
		draw_begin(layers_temp_image, false, 0);
		draw_set_pipeline(pipes_copy);
		draw_image(l0->texpaint_nor, 0, 0);
		draw_set_pipeline(NULL);
		draw_end();

		_gpu_begin(l0->texpaint_nor, NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
		gpu_set_pipeline(pipes_merge);
		gpu_set_texture(pipes_tex0, layers_temp_image);
		gpu_set_texture(pipes_tex1, l0->texpaint_pack);
		gpu_set_texture(pipes_texmask, empty);
		gpu_set_texture(pipes_texa, empty);
		gpu_set_float(pipes_opac, 1.0);
		gpu_set_float(pipes_tex1w, l0->texpaint_pack->width);
		gpu_set_int(pipes_blending, 104);
		gpu_set_vertex_buffer(const_data_screen_aligned_vb);
		gpu_set_index_buffer(const_data_screen_aligned_ib);
		gpu_draw();
		gpu_end();
	}

	return l0;
}

void layers_on_resized_on_next_frame(void *_) {
	layers_resize();
	slot_layer_t    *_layer    = g_context->layer;
	slot_material_t *_material = g_context->material;
	for (i32 i = 0; i < g_project->_->layers->length; ++i) {
		slot_layer_t *l = g_project->_->layers->buffer[i];
		if (l->fill_material != NULL) {
			g_context->layer    = l;
			g_context->material = l->fill_material;
			layers_update_paint_set();
			layers_update_fill_layer(true);
		}
	}
	g_context->layer    = _layer;
	g_context->material = _material;
	layers_update_paint_set();
	make_material_parse_paint_material(true);
}

void layers_on_resized() {
	sys_notify_on_next_frame(&layers_on_resized_on_next_frame, NULL);
	util_uv_uvmap              = NULL;
	util_uv_uvmap_cached       = false;
	util_uv_trianglemap        = NULL;
	util_uv_trianglemap_cached = false;
	util_uv_dilatemap_cached   = false;
	render_path_raytrace_ready = false;
}

void tab_layers_remap_layer_pointers(ui_node_t_array_t *nodes, i32_imap_t *pointer_map) {
	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *n = nodes->buffer[i];
		if (string_equals(n->type, "LAYER") || string_equals(n->type, "LAYER_MASK") || string_equals(n->type, "MESH_DISPLACE")) {
			i32 i = n->buttons->buffer[0]->default_value->buffer[0];
			if (i32_imap_get(pointer_map, i) != -1) {
				n->buttons->buffer[0]->default_value->buffer[0] = i32_imap_get(pointer_map, i);
			}
		}
	}
}

// Material and mesh nodes, after layers were added, removed or moved
void tab_layers_remap_all_layer_pointers(slot_layer_t_array_t *old_layers) {
	for (i32 i = 0; i < g_project->_->materials->length; ++i) {
		slot_material_t *m = g_project->_->materials->buffer[i];
		tab_layers_remap_layer_pointers(m->canvas->nodes, tab_layers_fill_layer_map(old_layers));
	}
	ui_node_canvas_t_array_t *mesh_canvases = nodes_mesh_canvases();
	for (i32 i = 0; i < mesh_canvases->length; ++i) {
		tab_layers_remap_layer_pointers(mesh_canvases->buffer[i]->nodes, tab_layers_fill_layer_map(old_layers));
	}
}

slot_layer_t_array_t *tab_layers_init_layer_map() {
	return array_slice(g_project->_->layers, 0, g_project->_->layers->length);
}

i32_imap_t *tab_layers_fill_layer_map(slot_layer_t_array_t *old_layers) {
	// Old index -> new index, 9999 for removed layers
	i32_imap_t *res = any_imap_create();
	for (i32 i = 0; i < old_layers->length; ++i) {
		i32 new_index = array_index_of(g_project->_->layers, old_layers->buffer[i]);
		i32_imap_set(res, i, new_index > -1 ? new_index : 9999);
	}
	return res;
}

void tab_layers_make_mask_preview_rgba32_on_next_frame(void *_) {
	slot_layer_t *l = tab_layers_l;
	draw_begin(g_context->mask_preview_rgba32, true, 0xff000000);
	draw_set_pipeline(ui_view2d_pipe);
	gpu_set_int(ui_view2d_channel_loc, 1);
	draw_image(l->texpaint_preview, 0, 0);
	draw_end();
	draw_set_pipeline(NULL);
}

void tab_layers_make_mask_preview_rgba32(slot_layer_t *l) {
	if (g_context->mask_preview_rgba32 == NULL) {
		g_context->mask_preview_rgba32 = gpu_create_render_target(util_render_layer_preview_size, util_render_layer_preview_size, GPU_TEXTURE_FORMAT_RGBA32);
	}
	// Convert from R8 to RGBA32 for tooltip display
	if (g_context->mask_preview_last != l) {
		g_context->mask_preview_last = l;
		tab_layers_l                 = l;
		sys_notify_on_next_frame(&tab_layers_make_mask_preview_rgba32_on_next_frame, NULL);
	}
}
