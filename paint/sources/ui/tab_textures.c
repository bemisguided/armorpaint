
#include "../global.h"

gpu_texture_t *_tab_textures_draw_img;
asset_t       *_tab_textures_draw_asset;
i32            _tab_textures_draw_i;
bool           _tab_textures_draw_is_packed;
i32            tab_textures_drag_pos = -1;

void tab_textures_draw_set_as_envmap(void *_) {
	import_envmap_run(_tab_textures_draw_asset->file, _tab_textures_draw_img);
}

void tab_textures_draw_to_layer(void *_) {
	layers_create_image_layers(asset_selection_get(ASSET_TRAY_TEXTURES));
}

void tab_textures_draw_to_mask(void *_) {
	layers_create_image_mask(_tab_textures_draw_asset);
}

static any_array_t    *_tab_textures_export_assets  = NULL; // Selected when Export was picked
static any_array_t    *_tab_textures_export_targets = NULL;
static string_array_t *_tab_textures_export_files   = NULL;

static bool tab_textures_is_hdr(gpu_texture_t *img) {
	return img->format == GPU_TEXTURE_FORMAT_RGBA64 || img->format == GPU_TEXTURE_FORMAT_RGBA128;
}

void tab_textures_draw_export_on_next_frame2(void *_) {
	for (i32 i = 0; i < _tab_textures_export_targets->length; ++i) {
		gpu_texture_t *target = _tab_textures_export_targets->buffer[i];
		char          *file   = _tab_textures_export_files->buffer[i];
		buffer_t      *pixels = gpu_get_texture_pixels(target);
		if (target->format == GPU_TEXTURE_FORMAT_RGBA64) {
			buffer_t *b = export_exr_run(target->width, target->height, pixels, 16, 1, 0);
			iron_file_save_bytes(file, b, b->length);
			array_free(b);
			free(b);
		}
		else if (ends_with(file, ".jpg")) {
			iron_write_jpg(file, pixels, target->width, target->height, 0, 80);
		}
		else {
			iron_write_png(file, pixels, target->width, target->height, 0);
		}
		gpu_delete_texture(target);
	}
}

void tab_textures_draw_export_on_next_frame(void *_) {
	_tab_textures_export_targets = any_array_create_from_raw((void *[]){}, 0);
	for (i32 i = 0; i < _tab_textures_export_assets->length; ++i) {
		gpu_texture_t *img    = project_get_image(_tab_textures_export_assets->buffer[i]);
		bool           hdr    = tab_textures_is_hdr(img);
		gpu_texture_t *target = gpu_create_render_target(img->width, img->height, hdr ? GPU_TEXTURE_FORMAT_RGBA64 : GPU_TEXTURE_FORMAT_RGBA32);
		draw_begin(target, false, 0);
		draw_set_pipeline(hdr ? pipes_copy64 : pipes_copy);
		draw_scaled_image(img, 0, 0, target->width, target->height);
		draw_set_pipeline(NULL);
		draw_end();
		any_array_push(_tab_textures_export_targets, target);
	}
	sys_notify_on_next_frame(&tab_textures_draw_export_on_next_frame2, NULL);
}

// One file for a single texture, named from the file browser; each texture under its own name for several
void tab_textures_draw_export(char *path) {
	any_array_t *assets        = any_array_create_from_raw((void *[]){}, 0);
	_tab_textures_export_files = string_array_create(0);
	for (i32 i = 0; i < _tab_textures_export_assets->length; ++i) {
		asset_t       *asset = _tab_textures_export_assets->buffer[i];
		gpu_texture_t *img   = project_get_image(asset);
		if (img == NULL) {
			continue;
		}
		bool hdr = tab_textures_is_hdr(img);
		if (_tab_textures_export_assets->length == 1) {
			char *f = ui_files_filename;
			if (string_equals(f, "")) {
				f = string_copy(tr("untitled"));
			}
			if (hdr && !ends_with(f, ".exr")) {
				f = string("%s.exr", f);
			}
			else if (!hdr && !ends_with(f, ".jpg") && !ends_with(f, ".png")) {
				f = string("%s.png", f);
			}
			string_array_push(_tab_textures_export_files, string("%s%s%s", path, PATH_SEP, f));
		}
		else {
			char *name = asset->name;
			i32   dot  = string_last_index_of(name, ".");
			if (dot > 0) {
				name = substring(name, 0, dot);
			}
			asset_selection_unique_path(path, name, hdr ? "exr" : "png", _tab_textures_export_files);
		}
		any_array_push(assets, asset);
	}
	_tab_textures_export_assets = assets;
	sys_notify_on_next_frame(&tab_textures_draw_export_on_next_frame, NULL);
}

void tab_textures_update_texture_pointers(ui_node_t_array_t *nodes, i32 index) {
	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *n = nodes->buffer[i];
		if (string_equals(n->type, "TEX_IMAGE")) {
			if (n->buttons->buffer[0]->default_value->buffer[0] == index) {
				n->buttons->buffer[0]->default_value->buffer[0] = 9999; // Texture deleted, use pink now
			}
			else if (n->buttons->buffer[0]->default_value->buffer[0] > index) {
				n->buttons->buffer[0]->default_value->buffer[0]--; // Offset by deleted texture
			}
		}
	}
}

void tab_textures_delete_texture_on_next_frame(void *_) {
	make_material_parse_paint_material(true);
	util_render_make_material_preview();
	ui_dock_redraw(TAB_AREA_SIDEBAR1);
}

void tab_textures_delete_texture(asset_t *asset) {
	i32 index = array_index_of(g_project->_->assets, asset);
	if (g_project->_->assets->length > 1) {
		g_context->texture = g_project->_->assets->buffer[index == g_project->_->assets->length - 1 ? index - 1 : index + 1];
	}
	else if (g_context->texture == asset) {
		g_context->texture = NULL;
	}
	ui_dock_redraw(TAB_AREA_STATUS);

	if (g_context->tool == TOOL_TYPE_COLORID && index == g_context->colorid) {
		ui_header_handle->redraws  = 2;
		g_context->ddirty          = 2;
		g_context->colorid_picked  = false;
		ui_toolbar_handle->redraws = 1;
	}

	if (data_get_texture(asset->file) == scene_world->_->envmap) {
		project_set_default_envmap();
	}

	if (g_project->packed_assets != NULL) {
		for (i32 i = 0; i < g_project->packed_assets->length; ++i) {
			packed_asset_t *pa = g_project->packed_assets->buffer[i];
			if (string_equals(pa->name, asset->file)) {
				array_splice(g_project->packed_assets, i, 1);
				break;
			}
		}
	}

	data_delete_texture(asset->file);
	meta_free(asset->meta);
	asset->meta = NULL;
	array_splice(g_project->_->assets, index, 1);
	sys_notify_on_next_frame(&tab_textures_delete_texture_on_next_frame, NULL);

	for (i32 i = 0; i < g_project->_->materials->length; ++i) {
		slot_material_t *m = g_project->_->materials->buffer[i];
		tab_textures_update_texture_pointers(m->canvas->nodes, index);
	}

	for (i32 i = 0; i < g_project->_->material_groups->length; ++i) {
		node_group_t *g = g_project->_->material_groups->buffer[i];
		tab_textures_update_texture_pointers(g->canvas->nodes, index);
	}

	for (i32 i = 0; i < g_project->_->brushes->length; ++i) {
		slot_brush_t *b = g_project->_->brushes->buffer[i];
		tab_textures_update_texture_pointers(b->canvas->nodes, index);
	}

	ui_node_canvas_t_array_t *mesh_canvases = nodes_mesh_canvases();
	for (i32 i = 0; i < mesh_canvases->length; ++i) {
		tab_textures_update_texture_pointers(mesh_canvases->buffer[i]->nodes, index);
	}
}

static void tab_textures_delete_one(void *asset) {
	tab_textures_delete_texture(asset);
}

void tab_textures_delete_selected() {
	asset_tray_delete_selected(ASSET_TRAY_TEXTURES, &tab_textures_delete_one, NULL, 0, false);
}

void tab_textures_draw_context_menu() {
	if (ui_menu_button(tr("Export"), "", ICON_EXPORT)) {
		_tab_textures_export_assets = asset_selection_get(ASSET_TRAY_TEXTURES);
		bool hdr                    = tab_textures_is_hdr(_tab_textures_draw_img);
		ui_files_show(hdr ? "exr" : "png,jpg", true, false, &tab_textures_draw_export);
	}
	if (ui_menu_button(tr("Reimport"), "", ICON_SYNC)) {
		project_reimport_texture(_tab_textures_draw_asset);
	}
	if (ui_menu_button(tr("To Layer"), "", ICON_LAYER)) {
		sys_notify_on_next_frame(&tab_textures_draw_to_layer, NULL);
	}
	if (ui_menu_button(tr("To Mask"), "", ICON_MASK)) {
		sys_notify_on_next_frame(&tab_textures_draw_to_mask, NULL);
	}
	if (ui_menu_button(tr("Set as Envmap"), "", ICON_LANDSCAPE)) {
		sys_notify_on_next_frame(&tab_textures_draw_set_as_envmap, NULL);
	}
	if (ui_menu_button(tr("Set as Color ID Map"), "", ICON_COLOR_ID)) {
		g_context->colorid         = _tab_textures_draw_i;
		g_context->colorid_picked  = false;
		ui_toolbar_handle->redraws = 1;
		if (g_context->tool == TOOL_TYPE_COLORID) {
			ui_header_handle->redraws = 2;
			g_context->ddirty         = 2;
		}
	}
	if (ui_menu_button(tr("Details..."), "", ICON_EDIT)) {
		ui_asset_details_show(MC_TEXTURE, _tab_textures_draw_i);
	}
	if (ui_menu_button(tr("Delete"), "delete", ICON_DELETE)) {
		tab_textures_delete_selected();
	}
	if (!_tab_textures_draw_is_packed && ui_menu_button(tr("Open Containing Directory..."), "", ICON_FOLDER_OPEN)) {
		file_start(substring(_tab_textures_draw_asset->file, 0, string_last_index_of(_tab_textures_draw_asset->file, PATH_SEP)));
	}
	if (!_tab_textures_draw_is_packed && ui_menu_button(tr("Open in Browser"), "", ICON_NONE)) {
		tab_browser_show_directory(substring(_tab_textures_draw_asset->file, 0, string_last_index_of(_tab_textures_draw_asset->file, PATH_SEP)));
	}
	if (g_context->view_image == _tab_textures_draw_asset) {
		if (ui_menu_button(tr("Close in 3D View"), "", ICON_NONE)) {
			util_stencil_view_image_close();
		}
	}
	else if (ui_menu_button(tr("Open in 3D View"), "", ICON_NONE)) {
		util_stencil_view_image_open(_tab_textures_draw_asset, 0.5, 0.5);
	}
}

bool tab_textures_nodes_use_texture(ui_node_t_array_t *nodes, i32 index) {
	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *n = nodes->buffer[i];
		if (string_equals(n->type, "TEX_IMAGE") && n->buttons->buffer[0]->default_value->buffer[0] == index) {
			return true;
		}
	}
	return false;
}

bool tab_textures_is_texture_used(i32 index) {
	if (index == g_context->colorid) {
		return true;
	}

	asset_t *asset = g_project->_->assets->buffer[index];
	if (g_project->envmap != NULL && string_equals(g_project->envmap, asset->file)) {
		return true;
	}

	for (i32 i = 0; i < g_project->_->materials->length; ++i) {
		slot_material_t *m = g_project->_->materials->buffer[i];
		if (tab_textures_nodes_use_texture(m->canvas->nodes, index)) {
			return true;
		}
	}

	for (i32 i = 0; i < g_project->_->material_groups->length; ++i) {
		node_group_t *g = g_project->_->material_groups->buffer[i];
		if (tab_textures_nodes_use_texture(g->canvas->nodes, index)) {
			return true;
		}
	}

	for (i32 i = 0; i < g_project->_->brushes->length; ++i) {
		slot_brush_t *b = g_project->_->brushes->buffer[i];
		if (tab_textures_nodes_use_texture(b->canvas->nodes, index)) {
			return true;
		}
	}

	ui_node_canvas_t_array_t *mesh_canvases = nodes_mesh_canvases();
	for (i32 i = 0; i < mesh_canvases->length; ++i) {
		if (tab_textures_nodes_use_texture(mesh_canvases->buffer[i]->nodes, index)) {
			return true;
		}
	}

	return false;
}

void tab_textures_delete_unused() {
	asset_t_array_t *unused = any_array_create_from_raw((void *[]){}, 0);
	for (i32 i = 0; i < g_project->_->assets->length; ++i) {
		if (!tab_textures_is_texture_used(i)) {
			any_array_push(unused, g_project->_->assets->buffer[i]);
		}
	}

	for (i32 i = 0; i < unused->length; ++i) {
		tab_textures_delete_texture(unused->buffer[i]);
	}

	array_delete(unused);
}

static void tab_textures_apply_node_indices(ui_node_t_array_t *nodes, i32_array_t *new_indices) {
	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *n = nodes->buffer[i];
		if (string_equals(n->type, "TEX_IMAGE")) {
			i32 idx = n->buttons->buffer[0]->default_value->buffer[0];
			if (idx >= 0 && idx < new_indices->length) {
				n->buttons->buffer[0]->default_value->buffer[0] = new_indices->buffer[idx];
			}
		}
	}
}

static int tab_textures_sort_by_name_compare(const void *pa, const void *pb) {
	asset_t *a = *(asset_t **)pa;
	asset_t *b = *(asset_t **)pb;
	return strcmp(a->name, b->name);
}

static void tab_textures_sort_by_name() {
	asset_t_array_t *assets = g_project->_->assets;
	if (assets->length < 2) {
		return;
	}
	asset_t_array_t *old_order = (asset_t_array_t *)array_slice((any_array_t *)assets, 0, assets->length);
	array_sort((any_array_t *)assets, &tab_textures_sort_by_name_compare);

	// Old asset index -> new asset index
	i32_array_t *new_indices = i32_array_create(assets->length);
	for (i32 i = 0; i < old_order->length; ++i) {
		new_indices->buffer[i] = array_index_of(assets, old_order->buffer[i]);
	}

	for (i32 i = 0; i < g_project->_->materials->length; ++i) {
		tab_textures_apply_node_indices(g_project->_->materials->buffer[i]->canvas->nodes, new_indices);
	}
	for (i32 i = 0; i < g_project->_->material_groups->length; ++i) {
		tab_textures_apply_node_indices(g_project->_->material_groups->buffer[i]->canvas->nodes, new_indices);
	}
	for (i32 i = 0; i < g_project->_->brushes->length; ++i) {
		tab_textures_apply_node_indices(g_project->_->brushes->buffer[i]->canvas->nodes, new_indices);
	}
	ui_node_canvas_t_array_t *mesh_canvases = nodes_mesh_canvases();
	for (i32 i = 0; i < mesh_canvases->length; ++i) {
		tab_textures_apply_node_indices(mesh_canvases->buffer[i]->nodes, new_indices);
	}
	if (g_context->colorid >= 0 && g_context->colorid < new_indices->length) {
		g_context->colorid = new_indices->buffer[g_context->colorid];
	}

	array_delete(new_indices);
	array_delete(old_order);
	ui_dock_redraw(TAB_AREA_STATUS);
}

void tab_textures_draw_edit() {
	if (ui_menu_button(tr("Sort"), "", ICON_NONE)) {
		tab_textures_sort_by_name();
	}
}

void tab_textures_draw_import(char *path) {
	import_asset_run(path, -1.0, -1.0, true, false, NULL);
	ui_dock_redraw(TAB_AREA_STATUS);
}

char *tab_textures_search = "";

void tab_textures_draw(i32 *htab) {

	if (ui_tab(htab, tr("Textures"), false, -1, false) && g_ui->_window_h > ui_statusbar_default_h * UI_SCALE()) {

		ui_begin_sticky();

		f32_array_t *row = f32_array_create(0);
		f32_array_push(row, -100);
		f32_array_push(row, -100);
		f32_array_push(row, -100);
		f32_array_push(row, -200); // Search
		if (ui_asset_tags_any(MC_TEXTURE)) {
			f32_array_push(row, -150); // Tags filter
		}
		if (!string_equals(tab_textures_search, "")) {
			f32_array_push(row, -40); // Clear search
		}
		ui_row(row);

		if (ui_icon_button(tr("Import"), ICON_IMPORT, UI_ALIGN_CENTER)) {
			ui_files_show(string_array_join(path_texture_formats(), ","), false, true, &tab_textures_draw_import);
		}
		if (g_ui->is_hovered) {
			ui_tooltip(string_tmp("%s (%s)", tr("Import texture file"), (char *)any_map_get(g_keymap, "file_import_assets")));
		}
		if (ui_icon_button(tr("2D View"), ICON_WINDOW, UI_ALIGN_CENTER)) {
			ui_base_show_2d_view(VIEW_2D_TYPE_ASSET);
		}
		if (ui_icon_button(tr("Edit"), ICON_EDIT, UI_ALIGN_CENTER)) {
			ui_menu_draw(&tab_textures_draw_edit, -1, -1);
		}

		ui_text_input(&tab_textures_search, tr("Search"), UI_ALIGN_LEFT, true, true);
		if (g_ui->is_hovered) {
			ui_tooltip(string_tmp("%s\n%s", tr("ctrl+f to search"), tr("esc to cancel")));
		}
		if (g_ui->is_ctrl_down && g_ui->is_key_pressed && g_ui->key_code == KEY_CODE_F) {
			ui_start_text_edit(&tab_textures_search, UI_ALIGN_LEFT);
		}
		ui_asset_tags_filter(MC_TEXTURE);
		if (!string_equals(tab_textures_search, "") && (ui_button(tr("X"), UI_ALIGN_CENTER, "") || g_ui->is_escape_down)) {
			tab_textures_search = "";
		}

		ui_end_sticky();

		char *search = to_lower_case(tab_textures_search);

		if (g_project->_->assets->length > 0) {

			f32 a     = ui_base_asset_scale();
			i32 slotw = math_floor((50 * a + 2) * UI_SCALE());
			i32 num   = math_floor(g_ui->_window_w / (float)slotw);
			if (num == 0) {
				return;
			}

			// Indices of the assets passing the search filter
			i32_array_t *filtered = asset_tray_filter(ASSET_TRAY_TEXTURES, search, true);
			asset_tray_begin(ASSET_TRAY_TEXTURES, search, filtered);

			bool drag_pos_set = false;
			f32  uix          = 0.0;
			f32  uiy          = 0.0;
			i32  imgw_val     = math_floor(50 * a * UI_SCALE());

			for (i32 row = 0; row < math_floor(math_ceil(filtered->length / (float)num)); ++row) {
				i32          mult = g_config->show_asset_names ? 2 : 1;
				f32_array_t *ar   = f32_array_create_from_raw((f32[]){}, 0);
				for (i32 i = 0; i < num * mult; ++i) {
					f32_array_push(ar, 1 / (float)num);
				}
				ui_row(ar);

				g_ui->_x += 2;
				f32 off = g_config->show_asset_names ? UI_ELEMENT_OFFSET() * 10.0 + (a - 1) * 50 * UI_SCALE() : 6;
				if (row > 0) {
					g_ui->_y += off;
				}

				for (i32 j = 0; j < num; ++j) {
					i32 imgw = math_floor(50 * a * UI_SCALE());
					i32 fi   = j + row * num;
					if (fi >= filtered->length) {
						ui_end_element_of_size(imgw);
						if (g_config->show_asset_names) {
							ui_end_element_of_size(0);
						}
						continue;
					}
					i32 i = filtered->buffer[fi];

					asset_t       *asset = g_project->_->assets->buffer[i];
					gpu_texture_t *img   = project_get_image(asset);
					if (img == NULL) {
						render_target_t *empty_rt = any_map_get(render_path_render_targets, "empty_black");
						img                       = empty_rt->_image;
					}
					uix = g_ui->_x;
					uiy = g_ui->_y;
					asset_selection_marquee_cell(asset, uix, uiy, imgw_val, imgw_val);
					i32 sw = img->height < img->width ? img->height : 0;

					if (base_drag_asset != NULL && tab_textures_drag_pos == i) {
						ui_fill(-1, -2, 2, imgw_val + 4, g_theme->HIGHLIGHT_COL);
					}

					ui_state_t _state = ui_sub_image(img, 0xffffffff, slotw, 0, 0, sw, sw);

					// Reordering
					if (_state == UI_STATE_HOVERED && base_drag_asset != NULL && string_equals(search, "") && !ui_asset_tags_active(MC_TEXTURE)) {
						tab_textures_drag_pos = (mouse_x > uix + g_ui->_window_x + imgw_val / 2.0) ? i + 1 : i;
						drag_pos_set          = true;
					}

					if (_state == UI_STATE_STARTED && g_ui->input_y > g_ui->_window_y) {
						bool can_drag;
						g_context->texture = asset_tray_press(ASSET_TRAY_TEXTURES, asset, filtered, &can_drag);
						if (can_drag) {
							bool double_click = asset_tray_drag_begin(uix, uiy);
							base_drag_asset   = asset;
							if (double_click) {
								ui_base_show_2d_view(VIEW_2D_TYPE_ASSET);
							}
						}
						ui_view2d_hwnd->redraws = 2;
					}

					u32 color = asset_tray_frame_color(ASSET_TRAY_TEXTURES, asset);
					if (color != 0) {
						f32 _uix = g_ui->_x;
						f32 _uiy = g_ui->_y;
						g_ui->_x = uix;
						g_ui->_y = uiy;
						i32 w    = 50 * a;
						ui_rect_round(1, 1, w + 2, w + 2, color, 2 * UI_SCALE());
						g_ui->_x = _uix;
						g_ui->_y = _uiy;
					}

					bool is_packed = g_project->packed_assets != NULL && project_packed_asset_exists(g_project->packed_assets, asset->file);

					if (g_ui->is_hovered) {
						ui_tooltip_image(img, 256);
						char *tooltip = asset->name;
						if (is_packed) {
							tooltip = string_tmp("%s %s", tooltip, tr("(packed)"));
						}
#ifdef WITH_BC7
						if (img->format == GPU_TEXTURE_FORMAT_RGBA32_BC7) {
							tooltip = string_tmp("%s %s", tooltip, tr("(compressed)"));
						}
#endif
						ui_tooltip(tooltip);
					}

					if (g_ui->is_hovered && g_ui->input_released_r) {
						asset_selection_context(ASSET_TRAY_TEXTURES, asset);
						g_context->texture = asset;

						_tab_textures_draw_img       = img;
						_tab_textures_draw_asset     = asset;
						_tab_textures_draw_i         = i;
						_tab_textures_draw_is_packed = is_packed;
						ui_menu_draw(&tab_textures_draw_context_menu, -1, -1);
					}

					if (g_config->show_asset_names) {
						g_ui->_y += slotw * 0.9 + (a - 1) * 5 * UI_SCALE();
						ui_base_asset_name(g_project->_->assets->buffer[i]->name, uix, slotw);
						if (g_ui->is_hovered) {
							ui_tooltip(g_project->_->assets->buffer[i]->name);
						}
						g_ui->_y -= slotw * 0.9 + (a - 1) * 5 * UI_SCALE();
						if (fi == filtered->length - 1) {
							g_ui->_y += j == num - 1 ? imgw : imgw + UI_ELEMENT_H() + UI_ELEMENT_OFFSET();
						}
					}
				}
			}

			if (base_drag_asset != NULL && tab_textures_drag_pos == g_project->_->assets->length) {
				g_ui->_x = uix;
				g_ui->_y = uiy;
				ui_fill(imgw_val + 1, -2, 2, imgw_val + 4, g_theme->HIGHLIGHT_COL);
			}

			asset_tray_end(ASSET_TRAY_TEXTURES);

			if (!drag_pos_set) {
				tab_textures_drag_pos = -1;
			}
		}
		else {
			gpu_texture_t *img = resource_get("icons.k");
			rect_t        *r   = resource_tile50(img, ICON_DROP);
			ui_sub_image(img, g_theme->BUTTON_COL, r->h, r->x, r->y, r->w, r->h);
			if (g_ui->is_hovered) {
				ui_tooltip(tr("Drag and drop files here"));
			}
		}

		bool in_focus = g_ui->input_x > g_ui->_window_x && g_ui->input_x < g_ui->_window_x + g_ui->_window_w && g_ui->input_y > g_ui->_window_y &&
		                g_ui->input_y < g_ui->_window_y + g_ui->_window_h;
		if (in_focus && g_ui->is_delete_down && g_project->_->assets->length > 0 && array_index_of(g_project->_->assets, g_context->texture) >= 0) {
			g_ui->is_delete_down = false;
			tab_textures_delete_selected();
		}
		if (in_focus && g_project->_->assets->length > 0) {
			i32 i = array_index_of(g_project->_->assets, g_context->texture);
			if (g_ui->is_key_pressed && g_ui->key_code == KEY_CODE_UP) {
				if (i > 0) {
					g_context->texture      = g_project->_->assets->buffer[i - 1];
					ui_view2d_hwnd->redraws = 2;
				}
			}
			if (g_ui->is_key_pressed && g_ui->key_code == KEY_CODE_DOWN) {
				if (i < g_project->_->assets->length - 1) {
					g_context->texture      = g_project->_->assets->buffer[i + 1];
					ui_view2d_hwnd->redraws = 2;
				}
			}
		}
	}
}

void tab_textures_remap_node_indices(ui_node_t_array_t *nodes, i32 old_pos, i32 new_pos) {
	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *n = nodes->buffer[i];
		if (string_equals(n->type, "TEX_IMAGE")) {
			i32 idx = n->buttons->buffer[0]->default_value->buffer[0];
			if (idx == old_pos) {
				n->buttons->buffer[0]->default_value->buffer[0] = new_pos;
			}
			else if (new_pos > old_pos && idx > old_pos && idx <= new_pos) {
				n->buttons->buffer[0]->default_value->buffer[0] = idx - 1;
			}
			else if (new_pos < old_pos && idx >= new_pos && idx < old_pos) {
				n->buttons->buffer[0]->default_value->buffer[0] = idx + 1;
			}
		}
	}
}

void tab_textures_on_asset_moved(i32 asset_pos, i32 new_pos) {
	for (i32 i = 0; i < g_project->_->materials->length; ++i) {
		tab_textures_remap_node_indices(g_project->_->materials->buffer[i]->canvas->nodes, asset_pos, new_pos);
	}
	for (i32 i = 0; i < g_project->_->material_groups->length; ++i) {
		tab_textures_remap_node_indices(g_project->_->material_groups->buffer[i]->canvas->nodes, asset_pos, new_pos);
	}
	for (i32 i = 0; i < g_project->_->brushes->length; ++i) {
		tab_textures_remap_node_indices(g_project->_->brushes->buffer[i]->canvas->nodes, asset_pos, new_pos);
	}
	ui_node_canvas_t_array_t *mesh_canvases = nodes_mesh_canvases();
	for (i32 i = 0; i < mesh_canvases->length; ++i) {
		tab_textures_remap_node_indices(mesh_canvases->buffer[i]->nodes, asset_pos, new_pos);
	}
	if (g_context->colorid == asset_pos) {
		g_context->colorid = new_pos;
	}
	else if (new_pos > asset_pos && g_context->colorid > asset_pos && g_context->colorid <= new_pos) {
		g_context->colorid--;
	}
	else if (new_pos < asset_pos && g_context->colorid >= new_pos && g_context->colorid < asset_pos) {
		g_context->colorid++;
	}
}

// Moves the dragged texture, or the selection holding it, to the drop marker
void tab_textures_accept_asset_drop(asset_t *asset) {
	if (tab_textures_drag_pos == -1) {
		return;
	}
	any_array_t *moving = asset_selection_drag_items(ASSET_TRAY_TEXTURES, asset, true);
	asset_selection_reorder(g_project->_->assets, moving, tab_textures_drag_pos, &tab_textures_on_asset_moved);
}
