
#include "../global.h"

i32 _tab_brushes_draw_i;
i32 tab_brushes_drag_pos = -1;

void tab_brushes_draw_make_brush_preview(void *_) {
	i32           i      = _tab_brushes_draw_i;
	slot_brush_t *_brush = g_context->brush;
	g_context->brush     = g_project->_->brushes->buffer[i];
	make_material_parse_brush();
	brush_output_node_parse_inputs();
	util_render_make_brush_preview();
	g_context->brush = _brush;
}

static bool tab_brushes_preview_pending = false;

void tab_brushes_draw_make_selected_preview(void *_) {
	// Wait for the startup init on frame 2
	if (g_context->frame < 3) {
		sys_notify_on_next_frame(&tab_brushes_draw_make_selected_preview, NULL);
		return;
	}
	tab_brushes_preview_pending = false;
	if (g_context->brush->preview_ready) {
		return;
	}
	make_material_parse_brush();
	brush_output_node_parse_inputs();
	util_render_make_brush_preview();
	// Skipped while sculpting, redrawing would request it again
	if (g_context->brush->preview_ready) {
		ui_dock_redraw(-1);
	}
}

// Copies of the selected brushes become the selection, the copy of the active brush stays active
void tab_brushes_duplicate_selected(void *_) {
	any_array_t  *sources = asset_selection_get(ASSET_TRAY_BRUSHES);
	any_array_t  *copies  = any_array_create_from_raw((void *[]){}, 0);
	slot_brush_t *active  = g_context->brush;
	slot_brush_t *primary = NULL;
	history_group_begin();
	for (i32 i = 0; i < sources->length; ++i) {
		slot_brush_t *source = sources->buffer[i];
		g_context->brush     = slot_brush_create(NULL);
		any_array_push(g_project->_->brushes, g_context->brush);
		ui_node_canvas_t *cloned = util_clone_canvas(source->canvas);
		cloned->name             = string_copy(slot_brush_unique_name(cloned, cloned->name));
		g_context->brush->canvas = cloned;
		g_context->brush->meta   = source->meta != NULL ? meta_copy(source->meta) : NULL;
		context_set_brush(g_context->brush);
		util_render_make_brush_preview();
		history_duplicate_brush();
		any_array_push(copies, g_context->brush);
		if (source == active) {
			primary = g_context->brush;
		}
	}
	history_group_end();
	if (primary != NULL && primary != g_context->brush) {
		context_set_brush(primary);
	}
	if (copies->length > 1) {
		asset_selection_set(ASSET_TRAY_BRUSHES, copies, g_context->brush);
	}
}

void tab_brushes_delete_brush(slot_brush_t *b) {
	i32 i            = array_index_of(g_project->_->brushes, b);
	g_context->brush = b;
	history_delete_brush();
	context_select_brush(i == g_project->_->brushes->length - 1 ? i - 1 : i + 1);
	array_splice(g_project->_->brushes, i, 1);
	ui_dock_redraw(1);
}

// At least one brush always stays, one undo step for all
void tab_brushes_delete_selected() {
	any_array_t *brushes = asset_selection_get(ASSET_TRAY_BRUSHES);
	history_group_begin();
	for (i32 i = 0; i < brushes->length && g_project->_->brushes->length > 1; ++i) {
		tab_brushes_delete_brush(brushes->buffer[i]);
	}
	history_group_end();
}

void tab_brushes_draw_context_menu() {
	i32 i = _tab_brushes_draw_i;
	// let b: slot_brush_t = brushes[i];

	if (ui_menu_button(tr("Export"), "", ICON_EXPORT)) {
		context_select_brush(i);
		box_export_show_brush(asset_selection_get(ASSET_TRAY_BRUSHES));
	}

	if (ui_menu_button(tr("Duplicate"), "ctrl+d", ICON_DUPLICATE)) {
		sys_notify_on_next_frame(&tab_brushes_duplicate_selected, NULL);
	}

	if (ui_menu_button(tr("Details..."), "", ICON_EDIT)) {
		ui_asset_details_show(MC_BRUSH, i);
	}

	if (g_project->_->brushes->length > 1 && ui_menu_button(tr("Delete"), "delete", ICON_DELETE)) {
		tab_brushes_delete_selected();
	}
}

void tab_brushes_draw_edit() {
	if (ui_menu_button(tr("Import"), "", ICON_IMPORT)) {
		project_import_brush();
	}
}

void tab_brushes_draw(i32 *htab) {
	if (ui_tab(htab, tr("Brushes"), false, -1, false)) {
		// The parameters panel shows the stroke preview of the selected brush
		if (!g_context->brush->preview_ready && !tab_brushes_preview_pending) {
			tab_brushes_preview_pending = true;
			sys_notify_on_next_frame(&tab_brushes_draw_make_selected_preview, NULL);
		}

		ui_begin_sticky();
		f32_array_t *row = f32_array_create(0);
		for (i32 i = 0; i < 3; ++i) {
			f32_array_push(row, -64);
		}
		if (ui_asset_tags_any(MC_BRUSH)) {
			f32_array_push(row, -150); // Tags filter
		}
		ui_row(row);
		if (ui_icon_button(tr("New"), ICON_PLUS, UI_ALIGN_CENTER)) {
			g_context->brush = slot_brush_create(NULL);
			any_array_push(g_project->_->brushes, g_context->brush);
			make_material_parse_brush();
			history_new_brush();
			ui_nodes_hwnd->redraws = 2;
		}
		if (ui_button(tr("Nodes"), UI_ALIGN_CENTER, "")) {
			ui_base_show_brush_nodes();
		}
		if (ui_icon_button(tr("Edit"), ICON_EDIT, UI_ALIGN_CENTER)) {
			ui_menu_draw(&tab_brushes_draw_edit, -1, -1);
		}
		ui_asset_tags_filter(MC_BRUSH);
		ui_end_sticky();
		ui_separator(3, false);

		f32 a     = ui_base_asset_scale();
		i32 slotw = math_floor((50 * a + 1) * UI_SCALE());
		i32 num   = math_floor(g_ui->_window_w / (float)slotw);
		if (num == 0) {
			return;
		}

		// Indices of the brushes passing the tag filter
		i32_array_t *filtered = i32_array_create_from_raw((i32[]){}, 0);
		for (i32 i = 0; i < g_project->_->brushes->length; ++i) {
			if (ui_asset_tags_visible(MC_BRUSH, i)) {
				i32_array_push(filtered, i);
			}
		}

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

			g_ui->_x += 2 * UI_SCALE();
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
				gpu_texture_t *img      = UI_SCALE() > 1 ? g_project->_->brushes->buffer[i]->image : g_project->_->brushes->buffer[i]->image_icon;
				gpu_texture_t *img_full = g_project->_->brushes->buffer[i]->image;

				slot_brush_t *brush     = g_project->_->brushes->buffer[i];
				bool          is_active = g_context->brush == brush;
				if (is_active || asset_selection_contains(ASSET_TRAY_BRUSHES, brush)) {
					// ui_fill(1, -2, img.width + 3, img.height + 3, ui.ops.theme.HIGHLIGHT_COL); // TODO
					i32 w = 50 * a;
					ui_rect_round(-1, -1, w + 2, w + 2, is_active ? g_theme->HIGHLIGHT_COL : asset_selection_highlight_color(), 2 * UI_SCALE());
				}

				uix      = g_ui->_x;
				uiy      = g_ui->_y;
				i32 tile = UI_SCALE() > 1 ? 100 : 50;

				if (base_drag_brush != NULL && tab_brushes_drag_pos == i) {
					ui_fill(-1, -2, 2, imgw_val + 4, g_theme->HIGHLIGHT_COL);
				}

				g_ui->image_scroll_align = false;
				ui_state_t state         = g_project->_->brushes->buffer[i]->preview_ready
				                               ? ui_image(img, 0xffffffff, imgw)
				                               : ui_sub_image(resource_get("icons.k"), -1, imgw, tile * 5, tile, tile, tile);
				g_ui->image_scroll_align = true;

				if (state == UI_STATE_HOVERED && base_drag_brush != NULL && !ui_asset_tags_active(MC_BRUSH)) {
					tab_brushes_drag_pos = (mouse_x > uix + g_ui->_window_x + imgw_val / 2.0) ? i + 1 : i;
					drag_pos_set         = true;
				}

				if (state == UI_STATE_STARTED) {
					bool          can_drag = asset_selection_press(ASSET_TRAY_BRUSHES, brush, NULL);
					slot_brush_t *primary  = asset_selection_primary_item();
					if (g_context->brush != primary) {
						context_set_brush(primary);
					}
					if (can_drag) {
						base_drag_off_x = -(mouse_x - uix - g_ui->_window_x - 3);
						base_drag_off_y = -(mouse_y - uiy - g_ui->_window_y + 1);
						base_drag_brush = g_context->brush;
						if (sys_time() - g_context->select_time < 0.2) {
							ui_base_show_brush_nodes();
							base_drag_brush  = NULL;
							base_is_dragging = false;
						}
						g_context->select_time = sys_time();
					}
				}
				if (g_ui->is_hovered && g_ui->input_released_r) {
					asset_selection_context(ASSET_TRAY_BRUSHES, brush);
					context_select_brush(i);

					_tab_brushes_draw_i = i;

					ui_menu_draw(&tab_brushes_draw_context_menu, -1, -1);
				}

				if (g_ui->is_hovered) {
					if (img_full == NULL) {
						_tab_brushes_draw_i = i;
						sys_notify_on_next_frame(&tab_brushes_draw_make_brush_preview, NULL);
					}
					else {
						ui_tooltip_image(img_full, 0);
						ui_tooltip(g_project->_->brushes->buffer[i]->canvas->name);
					}
				}

				if (g_config->show_asset_names) {
					g_ui->_y += slotw * 0.9 + (a - 1) * 5 * UI_SCALE();
					ui_base_asset_name(g_project->_->brushes->buffer[i]->canvas->name, uix, imgw);
					if (g_ui->is_hovered) {
						ui_tooltip(g_project->_->brushes->buffer[i]->canvas->name);
					}
					g_ui->_y -= slotw * 0.9 + (a - 1) * 5 * UI_SCALE();
					if (fi == filtered->length - 1) {
						g_ui->_y += j == num - 1 ? imgw : imgw + UI_ELEMENT_H() + UI_ELEMENT_OFFSET();
					}
				}
			}

			g_ui->_y += 6;
		}

		if (base_drag_brush != NULL && tab_brushes_drag_pos == g_project->_->brushes->length) {
			g_ui->_x = uix;
			g_ui->_y = uiy;
			ui_fill(imgw_val + 1, -2, 2, imgw_val + 4, g_theme->HIGHLIGHT_COL);
		}

		if (!drag_pos_set) {
			tab_brushes_drag_pos = -1;
		}

		bool in_focus = g_ui->input_x > g_ui->_window_x && g_ui->input_x < g_ui->_window_x + g_ui->_window_w && g_ui->input_y > g_ui->_window_y &&
		                g_ui->input_y < g_ui->_window_y + g_ui->_window_h;
		if (in_focus && g_ui->is_delete_down && g_project->_->brushes->length > 1) {
			g_ui->is_delete_down = false;
			tab_brushes_delete_selected();
		}
		if (in_focus && g_ui->is_ctrl_down && g_ui->is_key_pressed && g_ui->key_code == KEY_CODE_D) {
			sys_notify_on_next_frame(&tab_brushes_duplicate_selected, NULL);
		}
		if (in_focus) {
			i32 i = array_index_of(g_project->_->brushes, g_context->brush);
			if (g_ui->is_key_pressed && g_ui->key_code == KEY_CODE_UP) {
				if (i > 0) {
					context_select_brush(i - 1);
				}
			}
			if (g_ui->is_key_pressed && g_ui->key_code == KEY_CODE_DOWN) {
				if (i < g_project->_->brushes->length - 1) {
					context_select_brush(i + 1);
				}
			}
		}

		ui_node_params_draw(CANVAS_TYPE_BRUSH, &g_config->brush_params);
	}
}

void tab_brushes_accept_brush_drop(slot_brush_t *brush) {
	if (tab_brushes_drag_pos == -1) {
		return;
	}

	any_array_t *moving = asset_selection_drag_items(ASSET_TRAY_BRUSHES, brush, true);
	asset_selection_reorder(g_project->_->brushes, moving, tab_brushes_drag_pos, NULL);
}
