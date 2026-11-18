
#include "../global.h"

// Multi-selection of the items of one asset tray (textures, materials, brushes, fonts or sounds).
// The active item of the tray is the primary item and always part of the selection.
// With no extra items selected the selection is the active item alone.

static asset_tray_t asset_selection_tray        = ASSET_TRAY_NONE;
static any_array_t *asset_selection_items       = NULL;  // Selection order, holds the primary
static void        *asset_selection_anchor      = NULL;  // Start of shift ranges
static void        *asset_selection_primary     = NULL;  // Item the tray was asked to make active
static bool         asset_selection_primary_set = false; // The tray made the primary active
static void        *asset_selection_collapse_to = NULL;  // Plain press on a selected item, collapse unless dragged
static char        *asset_selection_filters[ASSET_TRAY_COUNT];

any_array_t *asset_selection_tray_items(asset_tray_t tray) {
	switch (tray) {
	case ASSET_TRAY_TEXTURES:
		return g_project->_->assets;
	case ASSET_TRAY_MATERIALS:
		return g_project->_->materials;
	case ASSET_TRAY_BRUSHES:
		return g_project->_->brushes;
	case ASSET_TRAY_FONTS:
		return g_project->_->fonts;
	case ASSET_TRAY_SOUNDS:
		return g_project->_->sounds;
	default:
		return NULL;
	}
}

void *asset_selection_active(asset_tray_t tray) {
	switch (tray) {
	case ASSET_TRAY_TEXTURES:
		return g_context->texture;
	case ASSET_TRAY_MATERIALS:
		return g_context->material;
	case ASSET_TRAY_BRUSHES:
		return g_context->brush;
	case ASSET_TRAY_FONTS:
		return g_context->font;
	case ASSET_TRAY_SOUNDS:
		return g_context->sound;
	default:
		return NULL;
	}
}

void asset_selection_clear() {
	asset_selection_tray        = ASSET_TRAY_NONE;
	asset_selection_items       = any_array_create_from_raw((void *[]){}, 0);
	asset_selection_anchor      = NULL;
	asset_selection_primary     = NULL;
	asset_selection_primary_set = false;
	asset_selection_collapse_to = NULL;
}

static void asset_selection_set_primary(void *item) {
	asset_selection_primary     = item;
	asset_selection_primary_set = false;
}

static void asset_selection_select_only(asset_tray_t tray, void *item) {
	asset_selection_clear();
	asset_selection_tray = tray;
	any_array_push(asset_selection_items, item);
	asset_selection_anchor = item;
	asset_selection_set_primary(item);
}

// Start a selection in this tray, seeded with the active item
static void asset_selection_begin(asset_tray_t tray) {
	if (asset_selection_tray == tray && asset_selection_items->length > 0) {
		return;
	}
	void *active = asset_selection_active(tray);
	asset_selection_clear();
	asset_selection_tray = tray;
	if (active != NULL && array_index_of(asset_selection_tray_items(tray), active) >= 0) {
		any_array_push(asset_selection_items, active);
		asset_selection_anchor = active;
		asset_selection_set_primary(active);
	}
}

static bool asset_selection_has(void *item) {
	return asset_selection_items != NULL && array_index_of(asset_selection_items, item) >= 0;
}

bool asset_selection_contains(asset_tray_t tray, void *item) {
	return asset_selection_tray == tray && asset_selection_has(item);
}

i32 asset_selection_count(asset_tray_t tray) {
	if (asset_selection_tray != tray || asset_selection_items->length == 0) {
		return asset_selection_active(tray) != NULL ? 1 : 0;
	}
	return asset_selection_items->length;
}

// Item the tray should make active, NULL when the pressed item stays as it is
void *asset_selection_primary_item() {
	return asset_selection_primary;
}

// Items in selection order, the active item alone when nothing else is selected
any_array_t *asset_selection_get(asset_tray_t tray) {
	any_array_t *result = any_array_create_from_raw((void *[]){}, 0);
	any_array_t *items  = asset_selection_tray_items(tray);
	if (asset_selection_tray == tray) {
		for (i32 i = 0; i < asset_selection_items->length; ++i) {
			if (array_index_of(items, asset_selection_items->buffer[i]) >= 0) {
				any_array_push(result, asset_selection_items->buffer[i]);
			}
		}
	}
	if (result->length == 0) {
		void *active = asset_selection_active(tray);
		if (active != NULL && array_index_of(items, active) >= 0) {
			any_array_push(result, active);
		}
	}
	return result;
}

// Items in their current tray order
any_array_t *asset_selection_get_tray_order(asset_tray_t tray) {
	any_array_t *selected = asset_selection_get(tray);
	any_array_t *items    = asset_selection_tray_items(tray);
	any_array_t *result   = any_array_create_from_raw((void *[]){}, 0);
	for (i32 i = 0; i < items->length; ++i) {
		if (array_index_of(selected, items->buffer[i]) >= 0) {
			any_array_push(result, items->buffer[i]);
		}
	}
	return result;
}

// Items moved by a drag of this item: the selection when the item is part of it, else the item alone
any_array_t *asset_selection_drag_items(asset_tray_t tray, void *item, bool tray_order) {
	if (asset_selection_contains(tray, item)) {
		return tray_order ? asset_selection_get_tray_order(tray) : asset_selection_get(tray);
	}
	any_array_t *result = any_array_create_from_raw((void *[]){}, 0);
	if (array_index_of(asset_selection_tray_items(tray), item) >= 0) {
		any_array_push(result, item);
	}
	return result;
}

// Tray indices of the items moved by a drag of this item, in selection order
i32_array_t *asset_selection_drag_indices(asset_tray_t tray, void *item) {
	any_array_t *moving  = asset_selection_drag_items(tray, item, false);
	i32_array_t *indices = i32_array_create_from_raw((i32[]){}, 0);
	for (i32 i = 0; i < moving->length; ++i) {
		i32_array_push(indices, array_index_of(asset_selection_tray_items(tray), moving->buffer[i]));
	}
	return indices;
}

static void asset_selection_add_range(asset_tray_t tray, void *from, void *to, i32_array_t *visible) {
	any_array_t *items = asset_selection_tray_items(tray);
	i32          count = visible != NULL ? visible->length : items->length;
	i32          a     = -1;
	i32          b     = -1;
	for (i32 i = 0; i < count; ++i) {
		void *item = items->buffer[visible != NULL ? visible->buffer[i] : i];
		if (item == from) {
			a = i;
		}
		if (item == to) {
			b = i;
		}
	}
	if (b == -1) {
		return;
	}
	if (a == -1) {
		a = b;
	}
	i32 step = b >= a ? 1 : -1;
	for (i32 i = a;; i += step) {
		void *item = items->buffer[visible != NULL ? visible->buffer[i] : i];
		if (!asset_selection_has(item)) {
			any_array_push(asset_selection_items, item);
		}
		if (i == b) {
			break;
		}
	}
}

// Left press on an item. visible: tray indices of the shown items in display order, NULL when all are shown.
// Returns true when the press may start a drag. The tray then makes asset_selection_primary_item() active.
bool asset_selection_press(asset_tray_t tray, void *item, i32_array_t *visible) {
	asset_selection_collapse_to = NULL;
	bool ctrl                   = g_ui->is_ctrl_down;
	bool shift                  = g_ui->is_shift_down;

	if (shift) {
		asset_selection_begin(tray);
		void *anchor = asset_selection_anchor != NULL ? asset_selection_anchor : item;
		if (!ctrl) {
			asset_selection_items = any_array_create_from_raw((void *[]){}, 0);
		}
		asset_selection_add_range(tray, anchor, item, visible);
		asset_selection_anchor = anchor;
		asset_selection_set_primary(item);
		return false;
	}

	if (ctrl) {
		asset_selection_begin(tray);
		if (asset_selection_has(item)) {
			if (asset_selection_items->length > 1) {
				array_remove(asset_selection_items, item);
				if (asset_selection_primary == item) {
					asset_selection_set_primary(asset_selection_items->buffer[asset_selection_items->length - 1]);
				}
			}
		}
		else {
			any_array_push(asset_selection_items, item);
			asset_selection_set_primary(item);
		}
		asset_selection_anchor = item;
		return false;
	}

	if (asset_selection_contains(tray, item) && asset_selection_items->length > 1) {
		asset_selection_collapse_to = item;
		asset_selection_set_primary(item);
		return true;
	}

	asset_selection_select_only(tray, item);
	return true;
}

// Right press on an item, before its menu opens
void asset_selection_context(asset_tray_t tray, void *item) {
	asset_selection_collapse_to = NULL;
	if (asset_selection_contains(tray, item)) {
		asset_selection_set_primary(item);
	}
	else {
		asset_selection_select_only(tray, item);
	}
}

// Replace the selection, for example with the copies made by Duplicate
void asset_selection_set(asset_tray_t tray, any_array_t *items, void *primary) {
	asset_selection_clear();
	asset_selection_tray = tray;
	for (i32 i = 0; i < items->length; ++i) {
		any_array_push(asset_selection_items, items->buffer[i]);
	}
	asset_selection_anchor = primary;
	asset_selection_set_primary(primary);
}

// Ctrl+A over a tray window selects every shown item. visible: tray indices of the shown items, NULL when all are shown.
// Called every frame by the trays while their window is drawn. The active item stays the primary one.
void asset_selection_select_all(asset_tray_t tray, i32_array_t *visible) {
	if (!g_ui->is_ctrl_down || !g_ui->is_key_pressed || g_ui->key_code != KEY_CODE_A || g_ui->is_typing) {
		return;
	}
	if (!ui_input_in_rect(g_ui->_window_x, g_ui->_window_y, g_ui->_window_w, g_ui->_window_h)) {
		return;
	}
	any_array_t *items = asset_selection_tray_items(tray);
	i32          count = visible != NULL ? visible->length : items->length;
	if (count == 0) {
		return;
	}
	g_ui->is_key_pressed = false;
	void *active         = asset_selection_active(tray);
	asset_selection_clear();
	asset_selection_tray = tray;
	for (i32 i = 0; i < count; ++i) {
		any_array_push(asset_selection_items, items->buffer[visible != NULL ? visible->buffer[i] : i]);
	}
	void *primary = active != NULL && asset_selection_has(active) ? active : asset_selection_items->buffer[0];
	asset_selection_anchor = primary;
	asset_selection_set_primary(primary);
	ui_dock_redraw(TAB_AREA_SIDEBAR1);
}

// Marquee: dragging from empty tray space draws a rectangle and selects the items it touches. The trays register the cell of every
// shown item while they draw (asset_selection_marquee_begin, _cell, then _end after the loop), so filtered items are never reached.
// Replaces the selection (shift: adds, ctrl: toggles) and never changes the active item, which stays part of the selection.
static any_array_t *asset_marquee_items  = NULL; // Items drawn this frame, in display order
static f32_array_t *asset_marquee_cells  = NULL; // x, y, w, h of each in screen pixels
static any_array_t *asset_marquee_base   = NULL; // Selection when the marquee started
static asset_tray_t asset_marquee_tray   = ASSET_TRAY_NONE;
static void        *asset_marquee_window = NULL; // Window the marquee started in (a tray can be shown in two windows)
static bool         asset_marquee_active = false;
static bool         asset_marquee_moved  = false;
static f32          asset_marquee_x      = 0.0;
static f32          asset_marquee_y      = 0.0;

void asset_selection_marquee_begin(asset_tray_t tray) {
	if (asset_marquee_items != NULL) {
		array_delete(asset_marquee_items);
		array_delete(asset_marquee_cells);
	}
	asset_marquee_items = any_array_create_from_raw((void *[]){}, 0);
	asset_marquee_cells = f32_array_create_from_raw((f32[]){}, 0);
}

// Cell of an item, x and y relative to the tray window like the layout coordinates
void asset_selection_marquee_cell(void *item, f32 x, f32 y, f32 w, f32 h) {
	any_array_push(asset_marquee_items, item);
	f32_array_push(asset_marquee_cells, g_ui->_window_x + x);
	f32_array_push(asset_marquee_cells, g_ui->_window_y + y);
	f32_array_push(asset_marquee_cells, w);
	f32_array_push(asset_marquee_cells, h);
}

static bool asset_marquee_press_on_item(f32 px, f32 py) {
	for (i32 i = 0; i < asset_marquee_items->length; ++i) {
		f32 *c = &asset_marquee_cells->buffer[i * 4];
		if (px >= c[0] && px < c[0] + c[2] && py >= c[1] && py < c[1] + c[3]) {
			return true;
		}
	}
	return false;
}

void asset_selection_marquee_end(asset_tray_t tray) {
	if (asset_marquee_items == NULL) {
		return;
	}
	if (asset_marquee_active && (asset_marquee_tray != tray || asset_marquee_window != g_ui->current_window)) {
		return; // Another tray or window is being drawn
	}

	if (!asset_marquee_active) {
		f32  px     = g_ui->input_started_x;
		f32  py     = g_ui->input_started_y;
		bool inside = px >= g_ui->_window_x && px < g_ui->_window_x + g_ui->_window_w - UI_SCROLL_W() && py >= g_ui->_window_y + g_ui->window_header_h &&
		              py < g_ui->_window_y + g_ui->_window_h;
		if (g_ui->input_started && g_ui->enabled && g_ui->input_enabled && !g_ui->is_typing && !base_is_dragging && inside &&
		    !asset_marquee_press_on_item(px, py)) {
			asset_marquee_active = true;
			asset_marquee_moved  = false;
			asset_marquee_tray   = tray;
			asset_marquee_window = g_ui->current_window;
			asset_marquee_x      = px;
			asset_marquee_y      = py;
			asset_marquee_base   = asset_selection_get(tray);
		}
		return;
	}

	if (!mouse_down("left")) {
		asset_marquee_active = false;
		return;
	}

	f32 mx = g_ui->input_x;
	f32 my = g_ui->input_y;
	if (!asset_marquee_moved && (math_abs(mx - asset_marquee_x) > 4 * UI_SCALE() || math_abs(my - asset_marquee_y) > 4 * UI_SCALE())) {
		asset_marquee_moved = true;
	}
	if (!asset_marquee_moved) {
		return;
	}

	f32 x0 = math_max(math_min(asset_marquee_x, mx), g_ui->_window_x);
	f32 y0 = math_max(math_min(asset_marquee_y, my), g_ui->_window_y + g_ui->window_header_h);
	f32 x1 = math_min(math_max(asset_marquee_x, mx), g_ui->_window_x + g_ui->_window_w);
	f32 y1 = math_min(math_max(asset_marquee_y, my), g_ui->_window_y + g_ui->_window_h);

	// Items touched by the rectangle
	any_array_t *hit = any_array_create_from_raw((void *[]){}, 0);
	for (i32 i = 0; i < asset_marquee_items->length; ++i) {
		f32 *c = &asset_marquee_cells->buffer[i * 4];
		if (c[0] < x1 && c[0] + c[2] > x0 && c[1] < y1 && c[1] + c[3] > y0) {
			any_array_push(hit, asset_marquee_items->buffer[i]);
		}
	}

	void        *active = asset_selection_active(tray);
	any_array_t *items  = any_array_create_from_raw((void *[]){}, 0);
	if (g_ui->is_shift_down) {
		for (i32 i = 0; i < asset_marquee_base->length; ++i) {
			any_array_push(items, asset_marquee_base->buffer[i]);
		}
		for (i32 i = 0; i < hit->length; ++i) {
			if (array_index_of(items, hit->buffer[i]) == -1) {
				any_array_push(items, hit->buffer[i]);
			}
		}
	}
	else if (g_ui->is_ctrl_down) {
		for (i32 i = 0; i < asset_marquee_base->length; ++i) {
			if (array_index_of(hit, asset_marquee_base->buffer[i]) == -1) {
				any_array_push(items, asset_marquee_base->buffer[i]);
			}
		}
		for (i32 i = 0; i < hit->length; ++i) {
			if (array_index_of(asset_marquee_base, hit->buffer[i]) == -1) {
				any_array_push(items, hit->buffer[i]);
			}
		}
	}
	else {
		for (i32 i = 0; i < hit->length; ++i) {
			any_array_push(items, hit->buffer[i]);
		}
	}
	if (active != NULL && array_index_of(items, active) == -1) {
		any_array_push(items, active);
	}
	array_delete(hit);

	asset_selection_clear();
	asset_selection_tray = tray;
	for (i32 i = 0; i < items->length; ++i) {
		any_array_push(asset_selection_items, items->buffer[i]);
	}
	array_delete(items);
	asset_selection_anchor = active;
	asset_selection_set_primary(active);

	// The rectangle, in window coordinates
	u32 c = g_theme->HIGHLIGHT_COL;
	draw_set_color((c & 0x00ffffff) | 0x30000000);
	draw_filled_rect(x0 - g_ui->_window_x, y0 - g_ui->_window_y, x1 - x0, y1 - y0);
	draw_set_color((c & 0x00ffffff) | 0xb0000000);
	draw_rect(x0 - g_ui->_window_x, y0 - g_ui->_window_y, x1 - x0, y1 - y0, 1.0);
	draw_set_color(0xffffffff);
	if (g_ui->current_window != NULL) {
		g_ui->current_window->redraws = 2; // Follow the mouse
	}
}

// Called every frame by the trays with a search field, a new search text collapses the selection
void asset_selection_sync_filter(asset_tray_t tray, char *search) {
	char *last = asset_selection_filters[tray];
	if (last != NULL && string_equals(last, search)) {
		return;
	}
	asset_selection_filters[tray] = string_copy(search);
	if (last != NULL && asset_selection_tray == tray) {
		asset_selection_clear();
	}
}

static void *asset_selection_dragged_item(asset_tray_t *tray) {
	if (base_drag_asset != NULL) {
		*tray = ASSET_TRAY_TEXTURES;
		return base_drag_asset;
	}
	if (base_drag_material != NULL) {
		*tray = ASSET_TRAY_MATERIALS;
		return base_drag_material;
	}
	if (base_drag_brush != NULL) {
		*tray = ASSET_TRAY_BRUSHES;
		return base_drag_brush;
	}
	if (base_drag_font != NULL) {
		*tray = ASSET_TRAY_FONTS;
		return base_drag_font;
	}
	if (base_drag_sound != NULL) {
		*tray = ASSET_TRAY_SOUNDS;
		return base_drag_sound;
	}
	*tray = ASSET_TRAY_NONE;
	return NULL;
}

// Number of items moved by the current drag
i32 asset_selection_drag_count() {
	asset_tray_t tray;
	void        *item = asset_selection_dragged_item(&tray);
	if (item == NULL || !asset_selection_contains(tray, item)) {
		return item != NULL ? 1 : 0;
	}
	return asset_selection_count(tray);
}

static void asset_selection_cancel_drag() {
	base_drag_asset    = NULL;
	base_drag_material = NULL;
	base_drag_brush    = NULL;
	base_drag_font     = NULL;
	base_drag_sound    = NULL;
	base_is_dragging   = false;
}

// Stops the drag when the dragged item was deleted (delete key, undo). Called before the drop handling and again before the
// drag image is drawn: a delete or an undo in the same frame (shortcut) frees the item's image after the first call
void asset_selection_validate_drag() {
	asset_tray_t drag_tray;
	void        *dragged = asset_selection_dragged_item(&drag_tray);
	if (dragged != NULL && array_index_of(asset_selection_tray_items(drag_tray), dragged) == -1) {
		asset_selection_cancel_drag();
	}
}

// Called every frame before the drop handling of base_update
void asset_selection_update() {
	if (asset_selection_items == NULL) {
		asset_selection_clear();
	}

	asset_selection_validate_drag();

	// Plain click on a selected item without dragging
	if (asset_selection_collapse_to != NULL && (mouse_released("left") || !mouse_down("left"))) {
		if (!base_is_dragging && asset_selection_has(asset_selection_collapse_to)) {
			asset_selection_select_only(asset_selection_tray, asset_selection_collapse_to);
			asset_selection_primary_set = true;
		}
		asset_selection_collapse_to = NULL;
	}

	if (asset_selection_tray == ASSET_TRAY_NONE) {
		return;
	}

	// Drop items that are gone
	any_array_t *items = asset_selection_tray_items(asset_selection_tray);
	for (i32 i = asset_selection_items->length - 1; i >= 0; --i) {
		if (array_index_of(items, asset_selection_items->buffer[i]) == -1) {
			array_splice(asset_selection_items, i, 1);
		}
	}

	// The active item was changed by something else (arrow keys, new item, undo, scripts)
	void *active = asset_selection_active(asset_selection_tray);
	if (active == asset_selection_primary) {
		asset_selection_primary_set = true;
	}
	else if (asset_selection_primary_set || !asset_selection_has(asset_selection_primary)) {
		asset_selection_clear();
	}
}

// Moves the items as one block to the drop position (0 to length), keeping their tray order.
// on_move is called with the old and new index of every single move made.
void asset_selection_reorder(any_array_t *items, any_array_t *moving, i32 drop_pos, void (*on_move)(i32, i32)) {
	any_array_t *order = any_array_create_from_raw((void *[]){}, 0);
	for (i32 i = 0; i <= items->length; ++i) {
		if (i == drop_pos) {
			for (i32 j = 0; j < items->length; ++j) {
				if (array_index_of(moving, items->buffer[j]) >= 0) {
					any_array_push(order, items->buffer[j]);
				}
			}
		}
		if (i < items->length && array_index_of(moving, items->buffer[i]) == -1) {
			any_array_push(order, items->buffer[i]);
		}
	}

	// Single moves from later positions keep the existing index remapping of each tray valid
	for (i32 to = 0; to < order->length; ++to) {
		i32 from = array_index_of(items, order->buffer[to]);
		if (from != to) {
			void *item = items->buffer[from];
			array_splice(items, from, 1);
			array_insert(items, to, item);
			if (on_move != NULL) {
				on_move(from, to);
			}
		}
	}
	array_delete(order);
}

// Frame colour of selected items that are not the active item
u32 asset_selection_highlight_color() {
	return (g_theme->HIGHLIGHT_COL & 0x00ffffff) | 0x80000000;
}

// dir/name.ext, or dir/name.001.ext and up when the file exists or is in taken; the result is added to taken
char *asset_selection_unique_path(char *dir, char *name, char *ext, string_array_t *taken) {
	char *path = string("%s%s%s.%s", dir, PATH_SEP, name, ext);
	for (i32 i = 1; iron_file_exists(path) || (taken != NULL && string_array_index_of(taken, path) >= 0); ++i) {
		path = string("%s%s%s.%03d.%s", dir, PATH_SEP, name, i, ext);
	}
	if (taken != NULL) {
		string_array_push(taken, path);
	}
	return path;
}
