
#include "../global.h"

// Tags of assets (meta_t tags), shared by the Textures, Materials, Brushes, Fonts, Sounds and Meshes trays:
// the tag filter combo of a tray. The tags of an item are edited in the Details dialog (ui_asset_details.c).

static char *ui_asset_tags_selected[MC_COUNT]; // Selected filter tag per category, NULL = All

// Sorted unique (case-insensitive, first spelling) tags in use in a category
static string_array_t *ui_asset_tags_in_use(i32 category) {
	string_array_t *tags  = string_array_create(0);
	string_array_t *lower = string_array_create(0);
	i32             count = meta_count(category);
	for (i32 i = 0; i < count; ++i) {
		meta_t *m = meta_peek(category, i);
		for (i32 j = 0; m != NULL && m->tags != NULL && j < m->tags->length; ++j) {
			char *l = string_to_lower_tmp(m->tags->buffer[j]);
			if (string_array_index_of(lower, l) >= 0) {
				continue;
			}
			i32 pos = 0; // Insertion sort
			while (pos < lower->length && strcmp(lower->buffer[pos], l) < 0) {
				pos++;
			}
			array_insert((any_array_t *)lower, pos, l);
			array_insert((any_array_t *)tags, pos, m->tags->buffer[j]);
		}
	}
	array_delete(lower);
	return tags;
}

// True when any tag is in use in the category, so the tray gives the filter combo a cell in its row
bool ui_asset_tags_any(i32 category) {
	string_array_t *tags = ui_asset_tags_in_use(category);
	bool            any  = tags->length > 0;
	array_delete(tags);
	return any;
}

// Draws the tag filter combo of a category as one cell of the tray row, only when a tag is in use there (otherwise draws nothing).
// The row must have a cell for it when ui_asset_tags_any is true. Call it every frame before ui_asset_tags_visible. Returns true when the combo was drawn
bool ui_asset_tags_filter(i32 category) {
	string_array_t *tags = ui_asset_tags_in_use(category);
	char          **sel  = &ui_asset_tags_selected[category];
	i32             idx  = 0;
	if (*sel != NULL) { // The selected tag may be gone, fall back to All
		for (i32 i = 0; i < tags->length; ++i) {
			if (string_equals(string_to_lower_tmp(tags->buffer[i]), string_to_lower_tmp(*sel))) {
				idx = i + 1;
				break;
			}
		}
		if (idx == 0) {
			*sel = NULL;
		}
	}
	if (tags->length == 0) {
		array_delete(tags);
		return false;
	}
	string_array_t *texts = string_array_create(0);
	string_array_push(texts, tr("All"));
	for (i32 i = 0; i < tags->length; ++i) {
		string_array_push(texts, tags->buffer[i]);
	}
	ui_combo(&idx, texts, tr("Tags"), false, UI_ALIGN_LEFT, true);
	if (ui_item_changed()) { // Copied only on change, the combo runs every frame
		*sel = idx > 0 ? string_copy(texts->buffer[idx]) : NULL;
		asset_selection_clear(); // Items the filter hides must not stay selected
	}
	array_delete(texts);
	array_delete(tags);
	return true;
}

// True when a tag filter is selected in the category
bool ui_asset_tags_active(i32 category) {
	return ui_asset_tags_selected[category] != NULL;
}

// True when the filter of the category is All or the item has the selected tag
bool ui_asset_tags_visible(i32 category, i32 index) {
	char *sel = ui_asset_tags_selected[category];
	return sel == NULL || meta_has_tag(meta_peek(category, index), sel);
}
