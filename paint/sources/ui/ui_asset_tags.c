
#include "../global.h"

// Tags of assets (meta_t tags), shared by the Textures, Materials, Brushes, Fonts, Sounds and Meshes trays:
// the "Tags..." panel of an item and the tag filter combo of a tray.

static i32   ui_asset_tags_box_category = 0;
static i32   ui_asset_tags_box_index    = 0;
static char *ui_asset_tags_input        = "";
static char *ui_asset_tags_selected[MC_COUNT]; // Selected filter tag per category, NULL = All

// Sorted unique (case-insensitive, first spelling) tags in use in a category
static string_array_t *ui_asset_tags_in_use(i32 category) {
	string_array_t *tags  = string_array_create(0);
	string_array_t *lower = string_array_create(0);
	i32             count = meta_count(category);
	for (i32 i = 0; i < count; ++i) {
		meta_t *m = meta_peek(category, i);
		for (i32 j = 0; m != NULL && m->tags != NULL && j < m->tags->length; ++j) {
			char *l = to_lower_case(m->tags->buffer[j]);
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

static void ui_asset_tags_box_draw() {
	meta_t *m = meta_peek(ui_asset_tags_box_category, ui_asset_tags_box_index);
	ui_text(meta_item_name(ui_asset_tags_box_category, ui_asset_tags_box_index), UI_ALIGN_LEFT, 0x00000000);

	i32 remove = -1;
	for (i32 i = 0; m != NULL && m->tags != NULL && i < m->tags->length; ++i) {
		f32_array_t *row = f32_array_create_from_raw_tmp(
		    (f32[]){
		        7 / 8.0,
		        1 / 8.0,
		    },
		    2);
		ui_row(row);
		ui_text(m->tags->buffer[i], UI_ALIGN_LEFT, 0x00000000);
		if (ui_button(tr("X"), UI_ALIGN_CENTER, "")) {
			remove = i;
		}
	}
	if (remove >= 0) {
		meta_remove_tag(m, string_copy(m->tags->buffer[remove]));
		base_redraw_ui();
	}

	f32_array_t *row = f32_array_create_from_raw_tmp(
	    (f32[]){
	        2 / 3.0,
	        1 / 3.0,
	    },
	    2);
	ui_row(row);
	ui_text_input(&ui_asset_tags_input, tr("Tag"), UI_ALIGN_LEFT, true, false);
	if (ui_icon_button(tr("Add"), ICON_PLUS, UI_ALIGN_CENTER) || g_ui->is_return_down) {
		if (!string_equals(ui_asset_tags_input, "")) {
			meta_add_tag(meta_get(ui_asset_tags_box_category, ui_asset_tags_box_index), ui_asset_tags_input);
			ui_asset_tags_input = "";
			base_redraw_ui();
		}
	}

	if (ui_icon_button(tr("OK"), ICON_CHECK, UI_ALIGN_CENTER)) {
		ui_box_hide();
	}
}

// Opens the tags panel of an item of a category (meta_category_t)
void ui_asset_tags_show(i32 category, i32 index) {
	if (index < 0 || index >= meta_count(category)) {
		return;
	}
	ui_asset_tags_box_category = category;
	ui_asset_tags_box_index    = index;
	ui_asset_tags_input        = "";
	ui_box_show_custom(&ui_asset_tags_box_draw, 400, 300, NULL, true, tr("Tags"));
}

// Draws the tag filter combo of a category in a row of its own, only when a tag is in use there (otherwise draws nothing).
// Call it every frame before ui_asset_tags_visible. Returns true when the combo was drawn
bool ui_asset_tags_filter(i32 category) {
	string_array_t *tags = ui_asset_tags_in_use(category);
	char          **sel  = &ui_asset_tags_selected[category];
	i32             idx  = 0;
	if (*sel != NULL) { // The selected tag may be gone, fall back to All
		for (i32 i = 0; i < tags->length; ++i) {
			if (string_equals(to_lower_case(tags->buffer[i]), to_lower_case(*sel))) {
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
	ui_row(f32_array_create_from_raw_tmp((f32[]){1.0}, 1));
	string_array_t *texts = string_array_create(0);
	string_array_push(texts, tr("All"));
	for (i32 i = 0; i < tags->length; ++i) {
		string_array_push(texts, tags->buffer[i]);
	}
	ui_combo(&idx, texts, tr("Tags"), false, UI_ALIGN_LEFT, true);
	if (ui_item_changed()) { // Copied only on change, the combo runs every frame
		*sel = idx > 0 ? string_copy(texts->buffer[idx]) : NULL;
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
