
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

// Called every frame before the drop handling of base_update
void asset_selection_update() {
	if (asset_selection_items == NULL) {
		asset_selection_clear();
	}

	// The dragged item was deleted (delete key, undo)
	asset_tray_t drag_tray;
	void        *dragged = asset_selection_dragged_item(&drag_tray);
	if (dragged != NULL && array_index_of(asset_selection_tray_items(drag_tray), dragged) == -1) {
		asset_selection_cancel_drag();
	}

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

// dir/name.ext, or dir/name.001.ext and up when the file exists
char *asset_selection_unique_path(char *dir, char *name, char *ext) {
	char *path = string("%s%s%s.%s", dir, PATH_SEP, name, ext);
	for (i32 i = 1; iron_file_exists(path); ++i) {
		path = string("%s%s%s.%03d.%s", dir, PATH_SEP, name, i, ext);
	}
	return path;
}
