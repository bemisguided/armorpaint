
#include "../global.h"

i32 _tab_materials_draw_slots;
i32 tab_materials_drag_pos = -1;

void tab_materials_button_nodes() {
	if (ui_button(tr("Nodes"), UI_ALIGN_CENTER, "")) {
		ui_base_show_material_nodes();
	}
	else if (g_ui->is_hovered) {
		ui_tooltip(string_tmp("%s (%s)", tr("Show Node Editor"), (char *)any_map_get(g_keymap, "toggle_node_editor")));
	}
}

void tab_materials_draw_slots_update_fill_layers(void *_) {
	layers_update_fill_layers();
}

void tab_materials_update_material() {
	ui_header_handle->redraws = 2;
	ui_nodes_hwnd->redraws    = 2;
	ui_nodes_group_stack      = any_array_create_from_raw((void *[]){}, 0);
	make_material_parse_paint_material(true);
	util_render_make_material_preview();
	bool decal = context_is_decal();
	if (decal) {
		util_render_make_decal_preview();
	}
	base_update_tools_nodes();
}

static void *tab_materials_duplicate(void *source) {
	g_context->material = slot_material_create(g_project->_->materials->buffer[0]->data, NULL);
	any_array_push(g_project->_->materials, g_context->material);
	ui_node_canvas_t *cloned    = util_clone_canvas(((slot_material_t *)source)->canvas);
	cloned->name                = string_copy(slot_material_unique_name(cloned, cloned->name));
	g_context->material->canvas = cloned;
	tab_materials_update_material();
	history_duplicate_material();
	return g_context->material;
}

static void tab_materials_set_active(void *material) {
	context_set_material(material);
}

void tab_materials_duplicate_selected(void *_) {
	asset_tray_duplicate_selected(ASSET_TRAY_MATERIALS, &tab_materials_duplicate, &tab_materials_set_active);
}

void tab_materials_update_material_pointers(ui_node_t_array_t *nodes, i32 deleted) {
	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *n = nodes->buffer[i];
		if (string_equals(n->type, "MATERIAL")) {
			if (n->buttons->buffer[0]->default_value->buffer[0] == deleted) {
				n->buttons->buffer[0]->default_value->buffer[0] = 9999; // Material deleted
			}
			else if (n->buttons->buffer[0]->default_value->buffer[0] > deleted) {
				n->buttons->buffer[0]->default_value->buffer[0]--; // Offset by deleted material
			}
		}
	}
}

static void tab_materials_on_material_removed(slot_material_t *m, i32 deleted) {
	tab_meshes_on_material_deleted(deleted);
	for (i32 i = 0; i < g_project->_->materials->length; ++i) {
		tab_materials_update_material_pointers(g_project->_->materials->buffer[i]->canvas->nodes, deleted);
	}
	for (i32 i = 0; i < g_project->_->material_groups->length; ++i) {
		tab_materials_update_material_pointers(g_project->_->material_groups->buffer[i]->canvas->nodes, deleted);
	}
	for (i32 i = 0; i < m->canvas->nodes->length; ++i) {
		ui_viewnodes_on_node_remove(m->canvas->nodes->buffer[i]);
	}
}

void tab_materials_delete_material(slot_material_t *m) {
	i32 i = array_index_of(g_project->_->materials, m);
	for (i32 j = 0; j < g_project->_->layers->length; ++j) {
		slot_layer_t *l = g_project->_->layers->buffer[j];
		if (l->fill_material == m) {
			l->fill_material = NULL;
		}
	}
	history_delete_material();
	context_select_material(i == g_project->_->materials->length - 1 ? i - 1 : i + 1);
	array_splice(g_project->_->materials, i, 1);
	ui_dock_redraw(1);
	tab_materials_on_material_removed(m, i);
}

static bool tab_materials_nodes_link_material(ui_node_t_array_t *nodes, i32 index) {
	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *n = nodes->buffer[i];
		if (string_equals(n->type, "MATERIAL") && n->buttons->buffer[0]->default_value->buffer[0] == index) {
			return true;
		}
	}
	return false;
}

static bool tab_materials_is_used(i32 index) {
	slot_material_t *m = g_project->_->materials->buffer[index];
	// Fill or path layer
	for (i32 i = 0; i < g_project->_->layers->length; ++i) {
		slot_layer_t *l = g_project->_->layers->buffer[i];
		if (l->fill_material == m || l->path_material == m) {
			return true;
		}
	}
	// Linked in another material via material node
	for (i32 i = 0; i < g_project->_->materials->length; ++i) {
		if (i != index && tab_materials_nodes_link_material(g_project->_->materials->buffer[i]->canvas->nodes, index)) {
			return true;
		}
	}
	for (i32 i = 0; i < g_project->_->material_groups->length; ++i) {
		if (tab_materials_nodes_link_material(g_project->_->material_groups->buffer[i]->canvas->nodes, index)) {
			return true;
		}
	}
	// Override material in meshes tab
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		if (tab_meshes_get_override(g_project->_->paint_objects->buffer[i]) == index) {
			return true;
		}
	}
	return false;
}

static void tab_materials_delete_one(void *material) {
	g_context->material = material; // History stores the selected material
	tab_materials_delete_material(material);
}

// At least one material always stays, one undo step for all
void tab_materials_delete_selected() {
	asset_tray_delete_selected(ASSET_TRAY_MATERIALS, &tab_materials_delete_one, NULL, 1, true);
}

void tab_materials_delete_unused() {
	i32   count = g_project->_->materials->length;
	bool *used  = calloc(count, sizeof(bool));
	for (i32 i = 0; i < count; ++i) {
		used[i] = tab_materials_is_used(i);
	}

	slot_material_t *selected = g_context->material;
	bool             deleted  = false;
	// Go backwards so the indices of the remaining materials stay valid
	for (i32 i = count - 1; i >= 0 && g_project->_->materials->length > 1; --i) {
		if (used[i]) {
			continue;
		}
		slot_material_t *m  = g_project->_->materials->buffer[i];
		g_context->material = m; // History stores the selected material
		history_delete_material();
		array_splice(g_project->_->materials, i, 1);
		tab_materials_on_material_removed(m, i);
		if (m == selected) {
			selected = NULL;
		}
		deleted = true;
	}
	free(used);

	if (!deleted) {
		return;
	}
	context_set_material(selected != NULL ? selected : g_project->_->materials->buffer[0]);
	ui_dock_redraw(1);
}

void tab_materials_draw_slots_menu() {
	i32              i = _tab_materials_draw_slots;
	slot_material_t *m = g_project->_->materials->buffer[i];

	if (ui_menu_button(tr("To Fill Layer"), "", ICON_SPHERE)) {
		context_select_material(i);
		layers_create_fill_layers(asset_selection_get(ASSET_TRAY_MATERIALS), UV_TYPE_UVMAP, mat4_nan(), -1);
	}

	if (ui_menu_button(tr("Export"), "", ICON_EXPORT)) {
		context_select_material(i);
		box_export_show_material(asset_selection_get(ASSET_TRAY_MATERIALS));
	}

	if (ui_menu_button(tr("Bake"), "", ICON_BAKE)) {
		context_select_material(i);
		box_export_show_bake_material();
	}

	if (ui_menu_button(tr("Duplicate"), "ctrl+d", ICON_DUPLICATE)) {
		sys_notify_on_next_frame(&tab_materials_duplicate_selected, NULL);
	}

	if (g_project->_->materials->length > 1 && ui_menu_button(tr("Delete"), "delete", ICON_DELETE)) {
		tab_materials_delete_selected();
	}

	if (g_config->experimental && ui_menu_button(tr("Inspect as Script"), "", ICON_SEARCH)) {
		node_shader_dump_to_script = true;
		context_select_material(i);
	}

	ui_menu_separator();
	ui_menu_align();
	ui_menu_label(tr("Channels"), NULL);
	ui_menu_align();
	ui_row2();
	bool channels_changed = false;
	ui_check(&m->paint_base, tr("Base Color"), "");
	channels_changed |= ui_item_changed();
	ui_check(&m->paint_opac, tr("Opacity"), "");
	channels_changed |= ui_item_changed();

	if (g_config->tools != TOOLS_SCULPT) {
		ui_row2();
		ui_check(&m->paint_nor, tr("Normal"), "");
		channels_changed |= ui_item_changed();
		ui_check(&m->paint_height, tr("Height"), "");
		channels_changed |= ui_item_changed();
		ui_row2();
		ui_check(&m->paint_rough, tr("Roughness"), "");
		channels_changed |= ui_item_changed();
		ui_check(&m->paint_met, tr("Metallic"), "");
		channels_changed |= ui_item_changed();
		ui_row2();
		ui_check(&m->paint_emis, tr("Emission"), "");
		channels_changed |= ui_item_changed();
		ui_check(&m->paint_subs, tr("Subsurface"), "");
		channels_changed |= ui_item_changed();
		ui_check(&m->paint_occ, tr("Occlusion"), "");
		channels_changed |= ui_item_changed();
	}

	if (channels_changed) {
		make_material_parse_paint_material(true);
		ui_menu_keep_open = true;
	}
}

void tab_materials_draw_slots(bool mini) {
	f32 a     = mini ? 1.0 : ui_base_asset_scale();
	i32 slotw = math_floor((50 * a + 1) * UI_SCALE() + g_config->window_scale * 2);
	i32 num   = math_floor(g_ui->_window_w / (float)slotw);
	if (num == 0) {
		return;
	}

	bool drag_pos_set = false;
	f32  uix          = 0.0;
	f32  uiy          = 0.0;
	i32  imgw_val     = math_floor(50 * a * UI_SCALE());
	asset_tray_begin(ASSET_TRAY_MATERIALS, NULL, NULL);

	for (i32 row = 0; row < math_floor(math_ceil(g_project->_->materials->length / (float)num)); ++row) {
		i32          mult = g_config->show_asset_names ? 2 : 1;
		f32_array_t *ar   = f32_array_create_from_raw_tmp(NULL, num * mult);
		for (i32 i = 0; i < num * mult; ++i) {
			ar->buffer[i] = 1 / (float)num;
		}
		ui_row(ar);

		g_ui->_x += mini ? 2 : 2 * UI_SCALE();
		f32 off = g_config->show_asset_names ? UI_ELEMENT_OFFSET() * 10.0 + (a - 1) * 50 * UI_SCALE() : 6;
		if (row > 0) {
			g_ui->_y += off;
		}

		for (i32 j = 0; j < num; ++j) {
			i32 imgw = math_floor(50 * a * UI_SCALE());
			i32 i    = j + row * num;
			if (i >= g_project->_->materials->length) {
				ui_end_element_of_size(imgw);
				if (g_config->show_asset_names) {
					ui_end_element_of_size(0);
				}
				continue;
			}
			gpu_texture_t *img      = UI_SCALE() > 1 ? g_project->_->materials->buffer[i]->image : g_project->_->materials->buffer[i]->image_icon;
			gpu_texture_t *img_full = g_project->_->materials->buffer[i]->image;

			// Selection plate behind the icon, the ring follows after it
			slot_material_t *material = g_project->_->materials->buffer[i];
			uix                       = g_ui->_x;
			uiy                       = g_ui->_y;
			f32 frame_x               = mini ? 0 : -1;
			f32 frame_y               = mini ? -2 : -1;
			i32 cell_w                = 50 * a;
			f32 frame_w               = mini ? g_ui->_w / (float)UI_SCALE() - 2 : cell_w + 2;
			f32 frame_h               = mini ? g_ui->_w / (float)UI_SCALE() - 4 : cell_w + 2;
			asset_tray_plate_draw(ASSET_TRAY_MATERIALS, material, uix, uiy, frame_x, frame_y, frame_w, frame_h);

			// Draw material icon
			i32 tile = UI_SCALE() > 1 ? 100 : 50;
			f32 imgh = mini ? ui_sidebar_w_mini * 0.85 : 50 * a * UI_SCALE();
			asset_selection_marquee_cell(material, uix, uiy, mini ? g_ui->_w : imgw_val, imgh);

			if (base_drag_material != NULL && tab_materials_drag_pos == i) {
				ui_fill(-1, -2, 2, imgw_val + 4, g_theme->HIGHLIGHT_COL);
			}

			g_ui->image_scroll_align = mini;
			ui_state_t state         = g_project->_->materials->buffer[i]->preview_ready
			                               ? ui_image(img, 0xffffffff, imgh)
			                               : ui_sub_image(resource_get("icons.k"), 0xffffffff, imgh, tile, tile, tile, tile);
			g_ui->image_scroll_align = true;
			asset_tray_ring_draw(ASSET_TRAY_MATERIALS, material, uix, uiy, frame_x, frame_y, frame_w, frame_h);

			if (state == UI_STATE_HOVERED && base_drag_material != NULL) {
				tab_materials_drag_pos = (mouse_x > uix + g_ui->_window_x + imgw_val / 2.0) ? i + 1 : i;
				drag_pos_set           = true;
			}

			// Draw material numbers when selecting a material via keyboard shortcut
			bool is_typing = g_ui->is_typing;
			if (!is_typing) {
				if (i < 9 && keymap_shortcut(any_map_get(g_keymap, "select_material"), SHORTCUT_TYPE_DOWN)) {
					char *number = i32_to_string(i + 1);
					i32   width  = draw_string_width(g_font, g_ui->font_size, number) + 10;
					i32   height = draw_font_height(g_font, g_ui->font_size);
					draw_set_color(g_theme->TEXT_COL);
					draw_filled_rect(uix, uiy, width, height);
					draw_set_color(g_theme->BUTTON_COL);
					draw_string(number, uix + 5, uiy);
				}
			}

			// Select material
			if (state == UI_STATE_STARTED && g_ui->input_y > g_ui->_window_y) {
				bool             can_drag;
				slot_material_t *primary = asset_tray_press(ASSET_TRAY_MATERIALS, material, NULL, &can_drag);
				if (g_context->material != primary) {
					context_set_material(primary);
					if (g_context->tool == TOOL_TYPE_MATERIAL) {
						sys_notify_on_next_frame(&tab_materials_draw_slots_update_fill_layers, NULL);
					}
				}
				if (can_drag) {
					bool double_click  = asset_tray_drag_begin(uix, uiy);
					base_drag_material = g_context->material;
					// Double click to show nodes
					if (double_click) {
						ui_base_show_material_nodes();
						base_drag_material = NULL;
						base_is_dragging   = false;
					}
				}
			}

			// Context menu
			if (g_ui->is_hovered && g_ui->input_released_r) {
				asset_selection_context(ASSET_TRAY_MATERIALS, material);
				context_select_material(i);
				_tab_materials_draw_slots = i;
				ui_menu_draw(&tab_materials_draw_slots_menu, -1, -1);
			}
			if (g_ui->is_hovered) {
				ui_tooltip_image(img_full, 0);
				if (i < 9) {
					i32 i1 = i + 1;
					ui_tooltip(
					    string_tmp("%s - (%s %d)", g_project->_->materials->buffer[i]->canvas->name, (char *)any_map_get(g_keymap, "select_material"), i1));
				}
				else {
					ui_tooltip(g_project->_->materials->buffer[i]->canvas->name);
				}
			}
			if (g_config->show_asset_names) {
				g_ui->_y += slotw * 0.9 + (a - 1) * 5 * UI_SCALE();
				ui_base_asset_name(g_project->_->materials->buffer[i]->canvas->name, uix, imgh);
				if (g_ui->is_hovered) {
					if (i < 9) {
						i32 i1 = i + 1;
						ui_tooltip(
						    string_tmp("%s - (%s %d)", g_project->_->materials->buffer[i]->canvas->name, (char *)any_map_get(g_keymap, "select_material"), i1));
					}
					else {
						ui_tooltip(g_project->_->materials->buffer[i]->canvas->name);
					}
				}
				g_ui->_y -= slotw * 0.9 + (a - 1) * 5 * UI_SCALE();
				if (i == g_project->_->materials->length - 1) {
					g_ui->_y += j == num - 1 ? imgw : imgw + UI_ELEMENT_H() + UI_ELEMENT_OFFSET();
				}
			}
		}

		g_ui->_y += mini ? 0 : 6;
	}

	if (base_drag_material != NULL && tab_materials_drag_pos == g_project->_->materials->length) {
		g_ui->_x = uix;
		g_ui->_y = uiy;
		ui_fill(imgw_val + 1, -2, 2, imgw_val + 4, g_theme->HIGHLIGHT_COL);
	}

	asset_tray_end(ASSET_TRAY_MATERIALS);

	if (!drag_pos_set) {
		tab_materials_drag_pos = -1;
	}

	bool in_focus = g_ui->input_x > g_ui->_window_x && g_ui->input_x < g_ui->_window_x + g_ui->_window_w && g_ui->input_y > g_ui->_window_y &&
	                g_ui->input_y < g_ui->_window_y + g_ui->_window_h;
	if (in_focus && g_ui->is_delete_down && g_project->_->materials->length > 1) {
		g_ui->is_delete_down = false;
		tab_materials_delete_selected();
	}
	if (in_focus && g_ui->is_ctrl_down && g_ui->is_key_pressed && g_ui->key_code == KEY_CODE_D) {
		sys_notify_on_next_frame(&tab_materials_duplicate_selected, NULL);
	}
	if (in_focus) {
		i32 i = array_index_of(g_project->_->materials, g_context->material);
		if (g_ui->is_key_pressed && g_ui->key_code == KEY_CODE_UP) {
			if (i > 0) {
				context_select_material(i - 1);
			}
		}
		if (g_ui->is_key_pressed && g_ui->key_code == KEY_CODE_DOWN) {
			if (i < g_project->_->materials->length - 1) {
				context_select_material(i + 1);
			}
		}
	}
}

void tab_materials_button_new_on_next_frame(void *_) {
	g_context->material = slot_material_create(g_project->_->materials->buffer[0]->data, NULL);
	any_array_push(g_project->_->materials, g_context->material);
	tab_materials_update_material();
	history_new_material();
}

void tab_materials_button_new(char *text) {
	if (ui_icon_button(text, ICON_PLUS, UI_ALIGN_CENTER)) {
		sys_notify_on_next_frame(&tab_materials_button_new_on_next_frame, NULL);
	}
}

void tab_materials_draw_mini(i32 *htab) {
	ui_set_hovered_tab_name(tr("Materials"));

	ui_begin_sticky();
	ui_separator(5, true);

	tab_materials_button_nodes();
	tab_materials_button_new("");

	ui_end_sticky();
	ui_separator(3, false);
	tab_materials_draw_slots(true);
}

void tab_materials_draw_edit() {
	if (ui_menu_button(tr("Import"), "", ICON_IMPORT)) {
		project_import_material();
	}
}

void tab_materials_draw_full(i32 *htab) {
	if (ui_tab(htab, tr("Materials"), false, -1, false)) {
		ui_begin_sticky();
		f32_array_t *row = f32_array_create_from_raw_tmp(
		    (f32[]){
		        -64,
		        -64,
		        -64,
		    },
		    3);
		ui_row(row);

		tab_materials_button_new(tr("New"));
		tab_materials_button_nodes();
		if (ui_icon_button(tr("Edit"), ICON_EDIT, UI_ALIGN_CENTER)) {
			ui_menu_draw(&tab_materials_draw_edit, -1, -1);
		}

		ui_end_sticky();
		ui_separator(3, false);
		tab_materials_draw_slots(false);
		ui_node_params_draw(CANVAS_TYPE_MATERIAL, &g_config->material_params);
	}
}

void tab_materials_draw(i32 *htab) {
	bool mini = g_ui->_window_w <= ui_sidebar_w_mini;
	mini ? tab_materials_draw_mini(htab) : tab_materials_draw_full(htab);
}

void tab_materials_accept_material_drop(slot_material_t *material) {
	if (tab_materials_drag_pos == -1) {
		return;
	}

	any_array_t *moving = asset_selection_drag_items(ASSET_TRAY_MATERIALS, material, true);
	asset_selection_reorder(g_project->_->materials, moving, tab_materials_drag_pos, &tab_meshes_on_material_reordered);
}

// Copy picked values into the Material Output node when it is the only node in the selected material
void tab_materials_apply_picked_color(swatch_color_t *swatch) {
	ui_node_canvas_t *canvas = g_context->material->canvas;
	if (canvas->nodes->length != 1 || !string_equals(canvas->nodes->buffer[0]->type, "OUTPUT_MATERIAL_PBR")) {
		return;
	}
	ui_node_t *node = canvas->nodes->buffer[0];
	node->inputs->buffer[0]->default_value =
	    f32_array_create_xyzw(color_get_rb(swatch->base) / 255.0, color_get_gb(swatch->base) / 255.0, color_get_bb(swatch->base) / 255.0, 1.0);
	node->inputs->buffer[1]->default_value->buffer[0] = swatch->opacity;
	node->inputs->buffer[2]->default_value->buffer[0] = swatch->occlusion;
	node->inputs->buffer[3]->default_value->buffer[0] = swatch->roughness;
	node->inputs->buffer[4]->default_value->buffer[0] = swatch->metallic;
	node->inputs->buffer[5]->default_value =
	    f32_array_create_xyz(color_get_rb(swatch->normal) / 255.0, color_get_gb(swatch->normal) / 255.0, color_get_bb(swatch->normal) / 255.0);
	node->inputs->buffer[7]->default_value->buffer[0] = swatch->height;
	tab_materials_update_material();
	if (layers_is_fill_material()) {
		layers_update_fill_layers();
	}
	if (layers_is_path_material()) {
		layers_update_path_layers();
	}
	tab_meshes_refresh_overrides(g_context->material);
	ui_dock_redraw(TAB_AREA_SIDEBAR1);
}

void tab_materials_accept_swatch_drop(swatch_color_t *swatch) {
	g_context->material = slot_material_create(g_project->_->materials->buffer[0]->data, NULL);
	for (i32 i = 0; i < g_context->material->canvas->nodes->length; ++i) {
		ui_node_t *node = g_context->material->canvas->nodes->buffer[i];
		if (string_equals(node->type, "OUTPUT_MATERIAL_PBR")) {
			node->inputs->buffer[0]->default_value            = f32_array_create_xyzw(color_get_rb(swatch->base) / 255.0, color_get_gb(swatch->base) / 255.0,
			                                                                          color_get_bb(swatch->base) / 255.0, color_get_ab(swatch->base) / 255.0);
			node->inputs->buffer[1]->default_value->buffer[0] = swatch->opacity;
			node->inputs->buffer[2]->default_value->buffer[0] = swatch->occlusion;
			node->inputs->buffer[3]->default_value->buffer[0] = swatch->roughness;
			node->inputs->buffer[4]->default_value->buffer[0] = swatch->metallic;
			node->inputs->buffer[7]->default_value->buffer[0] = swatch->height;
		}
	}
	any_array_push(g_project->_->materials, g_context->material);
	tab_materials_update_material();
	history_new_material();
}
