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

// Tray indices of the items whose name contains the search text (lower case), all items for an empty text
i32_array_t *asset_tray_filter(asset_tray_t tray, char *search) {
	any_array_t *items    = asset_selection_tray_items(tray);
	i32_array_t *filtered = i32_array_create_from_raw((i32[]){}, 0);
	for (i32 i = 0; i < items->length; ++i) {
		if (string_equals(search, "") || string_index_of(to_lower_case(asset_tray_item_name(tray, items->buffer[i])), search) >= 0) {
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

// Frame colour of an item: the highlight colour for the active item, a fainter one for the other selected items, 0 for no frame
u32 asset_tray_frame_color(asset_tray_t tray, void *item) {
	if (asset_selection_active(tray) == item) {
		return g_theme->HIGHLIGHT_COL;
	}
	if (asset_selection_contains(tray, item)) {
		return (g_theme->HIGHLIGHT_COL & 0x00ffffff) | 0x80000000;
	}
	return 0;
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
