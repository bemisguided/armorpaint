#include "../global.h"

// Parts of the five asset trays (textures, materials, brushes, fonts, sounds) that are the same in every tab:
// the per-frame selection setup, the search filter, the highlight, the press, delete and duplicate.
// The tabs keep what differs between them (the layout, the activation of an item, what a delete or a copy does).

static char *asset_tray_item_name(asset_tray_t tray, void *item) {
	switch (tray) {
	case ASSET_TRAY_TEXTURES:
		return ((asset_t *)item)->name;
	case ASSET_TRAY_FONTS:
		return ((slot_font_t *)item)->name;
	case ASSET_TRAY_SOUNDS:
		return ((slot_sound_t *)item)->name;
	default:
		return "";
	}
}

static i32 asset_tray_meta_category(asset_tray_t tray) {
	switch (tray) {
	case ASSET_TRAY_TEXTURES:
		return MC_TEXTURE;
	case ASSET_TRAY_MATERIALS:
		return MC_MATERIAL;
	case ASSET_TRAY_BRUSHES:
		return MC_BRUSH;
	case ASSET_TRAY_FONTS:
		return MC_FONT;
	default:
		return MC_SOUND;
	}
}

// Tray indices of the items that pass the tag filter of the tray (tags: false shows all) and whose name contains the search text
// (lower case, NULL or empty for no search). Called every frame: the result is one array per tray, emptied and refilled by the next call
// (valid until then, the trays only use it while they draw), so a redraw allocates nothing
i32_array_t *asset_tray_filter(asset_tray_t tray, char *search, bool tags) {
	static i32_array_t *filtered_of_tray[ASSET_TRAY_COUNT];
	any_array_t        *items    = asset_selection_tray_items(tray);
	i32_array_t        *filtered = filtered_of_tray[tray];
	if (filtered == NULL) {
		filtered               = i32_array_create(0);
		filtered_of_tray[tray] = filtered;
	}
	filtered->length = 0;
	for (i32 i = 0; i < items->length; ++i) {
		if (tags && !ui_asset_tags_visible(asset_tray_meta_category(tray), i)) {
			continue;
		}
		if (search == NULL || string_equals(search, "") || string_index_of(to_lower_case(asset_tray_item_name(tray, items->buffer[i])), search) >= 0) {
			i32_array_push(filtered, i);
		}
	}
	return filtered;
}

// Call every frame before the items are drawn. search: the lower case search text, NULL for a tray without search field.
// visible: tray indices of the shown items, NULL when all are shown.
void asset_tray_begin(asset_tray_t tray, char *search, i32_array_t *visible) {
	if (search != NULL) {
		asset_selection_sync_filter(tray, search);
	}
	asset_selection_select_all(tray, visible);
	asset_selection_marquee_begin();
}

// Call every frame after the items are drawn
void asset_tray_end(asset_tray_t tray) {
	asset_selection_marquee_end(tray);
}

// Opaque per channel blend of two colours, t = 0 gives a, t = 1 gives b
static u32 asset_tray_mix(u32 a, u32 b, f32 t) {
	u32 result = 0xff000000;
	for (i32 shift = 0; shift <= 16; shift += 8) {
		f32 ca = (f32)((a >> shift) & 0xff);
		f32 cb = (f32)((b >> shift) & 0xff);
		result |= (u32)(ca + (cb - ca) * t + 0.5f) << shift;
	}
	return result;
}

// Colours of the selection mark, all derived from the theme and opaque: ui_rect_round blends a translucent colour wrongly in its
// corners. The ring is the highlight colour mixed towards the text colour until it reads on the window background (3:1 and more,
// the light theme is the limit), the active ring mixes further than the selected one. The plate is the window background with a
// few percent of the highlight colour, a bit more for the active item.
static void asset_tray_frame_colors(u32 bg_col, u32 text_col, u32 highlight_col, bool active, u32 *plate, u32 *ring) {
	*ring  = asset_tray_mix(highlight_col, text_col, active ? 0.5f : 0.34f);
	*plate = asset_tray_mix(bg_col, highlight_col, active ? 0.15f : 0.08f);
}

static f32 asset_tray_ring_thickness(bool active, f32 scale) {
	return (active ? 2.0f : 1.0f) * scale;
}

// Pixel box of the plate, inside the ring that ui_rect_round(x, y, w, h, color, thickness) draws (same rounding as there).
// ox, oy: pixel position of the ui origin, x .. h: ring box in ui units, thickness in pixels.
static void asset_tray_plate_rect(f32 ox, f32 oy, f32 x, f32 y, f32 w, f32 h, f32 scale, f32 thickness, i32 *px, i32 *py, i32 *pw, i32 *ph) {
	*px = (i32)(ox + x * scale - thickness / 2.0f) + 1;
	*py = (i32)(oy + y * scale - thickness / 2.0f) + 1;
	*pw = (i32)(w * scale + thickness) - 2;
	*ph = (i32)(h * scale + thickness) - 2;
}

// The selection mark of an item is drawn in two steps around its preview: the plate BEFORE the preview (it is opaque, so it must
// never be drawn after it: it would hide a square preview), the ring AFTER it (a preview that fills its cell would cover it).
// Nothing is drawn for an item that is not selected. x, y, w, h: the box of the ring in ui units relative to the item cell, the
// same arguments ui_rect_round takes. cell_x, cell_y: the ui position of the item cell (g_ui->_x, _y read before the preview).

// Step 1, before the preview: a plate tinted with the highlight colour (stronger for the active item).
void asset_tray_plate_draw(asset_tray_t tray, void *item, f32 cell_x, f32 cell_y, f32 x, f32 y, f32 w, f32 h) {
	bool active = asset_selection_active(tray) == item;
	if (!active && !asset_selection_contains(tray, item)) {
		return;
	}
	u32 plate_col;
	u32 ring_col;
	asset_tray_frame_colors(g_theme->WINDOW_BG_COL, g_theme->TEXT_COL, g_theme->HIGHLIGHT_COL, active, &plate_col, &ring_col);
	i32 px, py, pw, ph;
	asset_tray_plate_rect(cell_x, cell_y, x, y, w, h, UI_SCALE(), asset_tray_ring_thickness(active, UI_SCALE()), &px, &py, &pw, &ph);
	draw_set_color(plate_col);
	ui_draw_rect(true, false, px, py, pw, ph);
	draw_set_color(0xffffffff);
}

// Step 2, after the preview: a thin ring (selected) or a thicker, lighter ring (the active item).
void asset_tray_ring_draw(asset_tray_t tray, void *item, f32 cell_x, f32 cell_y, f32 x, f32 y, f32 w, f32 h) {
	bool active = asset_selection_active(tray) == item;
	if (!active && !asset_selection_contains(tray, item)) {
		return;
	}
	u32 plate_col;
	u32 ring_col;
	asset_tray_frame_colors(g_theme->WINDOW_BG_COL, g_theme->TEXT_COL, g_theme->HIGHLIGHT_COL, active, &plate_col, &ring_col);
	f32 _x   = g_ui->_x;
	f32 _y   = g_ui->_y;
	g_ui->_x = cell_x;
	g_ui->_y = cell_y;
	ui_rect_round(x, y, w, h, ring_col, asset_tray_ring_thickness(active, UI_SCALE()));
	g_ui->_x = _x;
	g_ui->_y = _y;
}

// Left press on an item, updates the selection. Returns the item the tray makes active (it can differ from the pressed one:
// ctrl click on a selected item) and tells in can_drag whether the press may start a drag.
void *asset_tray_press(asset_tray_t tray, void *item, i32_array_t *visible, bool *can_drag) {
	*can_drag = asset_selection_press(tray, item, visible);
	return asset_selection_primary_item();
}

// Start of a drag from the item cell at uix, uiy. Returns true for a double click (second press within 0.2 s).
bool asset_tray_drag_begin(f32 uix, f32 uiy) {
	base_drag_off_x        = -(mouse_x - uix - g_ui->_window_x - 3);
	base_drag_off_y        = -(mouse_y - uiy - g_ui->_window_y + 1);
	bool double_click      = sys_time() - g_context->select_time < 0.2;
	g_context->select_time = sys_time();
	return double_click;
}

// Deletes the selected items with delete_item, in selection order. keep: items that always stay in the tray (the loop stops at
// that many). can_delete: NULL or asked per item. grouped: all deletes make one undo step.
void asset_tray_delete_selected(asset_tray_t tray, void (*delete_item)(void *), bool (*can_delete)(void *), i32 keep, bool grouped) {
	any_array_t *selected = asset_selection_get(tray);
	any_array_t *items    = asset_selection_tray_items(tray);
	if (grouped) {
		history_group_begin();
	}
	for (i32 i = 0; i < selected->length && items->length > keep; ++i) {
		if (can_delete == NULL || can_delete(selected->buffer[i])) {
			delete_item(selected->buffer[i]);
		}
	}
	if (grouped) {
		history_group_end();
	}
}

// Copies the selected items with duplicate (it adds the copy to the tray, makes it active and pushes its history step, one undo
// step for all). The copies become the selection, the copy of the active item stays active, set_active makes an item active.
void asset_tray_duplicate_selected(asset_tray_t tray, void *(*duplicate)(void *), void (*set_active)(void *)) {
	any_array_t *sources = asset_selection_get(tray);
	any_array_t *copies  = any_array_create_from_raw((void *[]){}, 0);
	void        *active  = asset_selection_active(tray);
	void        *primary = NULL;
	void        *last    = NULL;
	history_group_begin();
	for (i32 i = 0; i < sources->length; ++i) {
		last = duplicate(sources->buffer[i]);
		any_array_push(copies, last);
		if (sources->buffer[i] == active) {
			primary = last;
		}
	}
	history_group_end();
	if (primary != NULL && primary != last) {
		set_active(primary);
	}
	if (copies->length > 1) {
		asset_selection_set(tray, copies, asset_selection_active(tray));
	}
}
