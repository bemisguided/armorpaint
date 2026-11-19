
#include "../global.h"

// "Details" dialog of the assets of a category (meta_category_t), shared by the Textures, Materials, Brushes, Fonts and Sounds trays.
// Name (Materials and Brushes only), Tags, and for Textures the Bake Texture section (bake type and encoding, see util_meta.c).
// Changes apply at once, the name commits on Enter or when the field loses focus (a click anywhere else, Tab). Closing the dialog needs no commit of its own:
// the click on OK or outside ends the edit first, and the field commits in the last frame the dialog draws.
// Opened on an item that is part of the selection of its tray (util_asset_selection.c), the dialog edits all selected items at once:
// the tags of every item, and fields that differ across the items show as Mixed and are written only when changed. The name is hidden then.
// Every edit is one undo step for all items (history_group_begin / history_group_end).

#define UI_ASSET_DETAILS_MIXED ((i32)0x80000000) // Combo value of a field that differs across the items

static i32          ui_asset_details_category = 0;
static any_array_t *ui_asset_details_items    = NULL; // Edited items (asset_t, slot_material_t...), the item alone or the selection of its tray
static char        *ui_asset_details_input    = "";   // New tag
static char        *ui_asset_details_name     = "";   // Name field text, committed to the asset on Enter or focus loss
static i32          ui_asset_details_ids[2];
static bool         ui_asset_details_had_mixed[2];        // The field started mixed when the dialog opened: its Mixed entry stays in the combo list
static i32          ui_asset_details_last_btp = BTP_NONE; // Last bake type chosen in this session, BTP_NONE = none yet
// Metadata of each item from before the first change made through the open combo, see ui_asset_details_combo_commit
static any_array_t *ui_asset_details_combo_prev = NULL;

// Position of each edited item in the live list of its category the last time it was looked up, a hint that makes the lookup O(1)
static i32_array_t *ui_asset_details_hints = NULL;

// Tags of all edited items with the number of items that have each, rebuilt by ui_asset_details_tags_update when they change
static string_array_t *ui_asset_details_tag_list  = NULL; // Owned copies, in order of first appearance, compared case-insensitively
static i32_array_t    *ui_asset_details_tag_have  = NULL;
static u64             ui_asset_details_tag_hash  = 0;
static i32             ui_asset_details_tag_frame = -1;
static bool            ui_asset_details_tag_valid = false;

static i32 ui_asset_details_count() {
	return ui_asset_details_items != NULL ? ui_asset_details_items->length : 0;
}

static bool ui_asset_details_multi() {
	return ui_asset_details_count() > 1;
}

// Index of the i-th edited item in the live list of its category, -1 when it is gone. A selection of hundreds of items is looked up many
// times a frame, so the position found last time is tried first and the list is only scanned when the item moved.
static i32 ui_asset_details_index(i32 i) {
	void *item = ui_asset_details_items->buffer[i];
	i32   hint = ui_asset_details_hints->buffer[i];
	if (hint >= 0 && hint < meta_count(ui_asset_details_category) && meta_item(ui_asset_details_category, hint) == item) {
		return hint;
	}
	hint                              = meta_item_index(ui_asset_details_category, item);
	ui_asset_details_hints->buffer[i] = hint;
	return hint;
}

static meta_t *ui_asset_details_peek(i32 i) {
	return meta_peek(ui_asset_details_category, ui_asset_details_index(i));
}

static meta_t *ui_asset_details_meta(i32 i) {
	return meta_get(ui_asset_details_category, ui_asset_details_index(i));
}

// Drops the items that are gone (deleted, undo of an add) together with their combo snapshot
static void ui_asset_details_refresh() {
	for (i32 i = ui_asset_details_count() - 1; i >= 0; --i) {
		if (ui_asset_details_index(i) == -1) {
			array_splice(ui_asset_details_items, i, 1);
			i32_array_splice(ui_asset_details_hints, i, 1);
			if (ui_asset_details_combo_prev != NULL) {
				meta_free(ui_asset_details_combo_prev->buffer[i]);
				array_splice(ui_asset_details_combo_prev, i, 1);
			}
		}
	}
}

// The value of an i32 key shared by all items, UI_ASSET_DETAILS_MIXED when they differ
static i32 ui_asset_details_shared(i32 key, i32 def) {
	i32 v = meta_get_i32(ui_asset_details_peek(0), key, def);
	for (i32 i = 1; i < ui_asset_details_count(); ++i) {
		if (meta_get_i32(ui_asset_details_peek(i), key, def) != v) {
			return UI_ASSET_DETAILS_MIXED;
		}
	}
	return v;
}

// Combo over ids, an id that is not in the list is appended as `Unknown (n)` so it is shown and kept, UI_ASSET_DETAILS_MIXED comes first as `Mixed`.
// A field that was mixed when the dialog opened keeps its Mixed entry after the first pick, so the list indices of an open combo do not shift
// under its arrow keys. Returns true and sets value when changed.
static bool ui_asset_details_combo(i32 *value, string_array_t *ar, i32_array_t *ids, char *label, char *(*label_of)(i32), i32 id_slot) {
	i32 pos = i32_array_index_of(ids, *value);
	if (*value == UI_ASSET_DETAILS_MIXED) {
		ui_asset_details_had_mixed[id_slot] = true;
	}
	if (pos == -1 && *value != UI_ASSET_DETAILS_MIXED) {
		any_array_push(ar, label_of(*value));
		i32_array_push(ids, *value);
		pos = ids->length - 1;
	}
	if (ui_asset_details_had_mixed[id_slot]) {
		array_insert((any_array_t *)ar, 0, tr("Mixed"));
		i32_array_push(ids, 0);
		for (i32 i = ids->length - 1; i > 0; --i) {
			ids->buffer[i] = ids->buffer[i - 1];
		}
		ids->buffer[0] = UI_ASSET_DETAILS_MIXED;
		pos            = *value == UI_ASSET_DETAILS_MIXED ? 0 : pos + 1;
	}
	ui_set_next_id((ui_id_t)&ui_asset_details_ids[id_slot]);
	ui_combo(&pos, ar, label, true, UI_ALIGN_LEFT, true);
	bool changed = ui_item_changed() && ids->buffer[pos] != *value &&
	               ids->buffer[pos] != UI_ASSET_DETAILS_MIXED; // Escape hands back the old value, Mixed again changes nothing
	if (changed) {
		*value = ids->buffer[pos];
	}
	array_free(ar);
	free(ar);
	array_free(ids);
	free(ids);
	return changed;
}

// Copies of the metadata of the items, taken before an edit and handed to ui_asset_details_push_edit once the edit is applied
static any_array_t *ui_asset_details_snapshot() {
	any_array_t *prev = any_array_create(0);
	for (i32 i = 0; i < ui_asset_details_count(); ++i) {
		any_array_push(prev, meta_copy(ui_asset_details_peek(i)));
	}
	return prev;
}

// Same tags and bake keys, the fields the dialog edits
static bool ui_asset_details_same(meta_t *a, meta_t *b) {
	i32 na = a != NULL && a->tags != NULL ? a->tags->length : 0;
	i32 nb = b != NULL && b->tags != NULL ? b->tags->length : 0;
	if (na != nb) {
		return false;
	}
	for (i32 i = 0; i < na; ++i) {
		if (!string_equals(a->tags->buffer[i], b->tags->buffer[i])) {
			return false;
		}
	}
	i32 keys[] = {META_BAKE_TYPE, META_ENCODING};
	for (i32 i = 0; i < 2; ++i) {
		if (meta_get_i32(a, keys[i], -1) != meta_get_i32(b, keys[i], -1)) {
			return false;
		}
	}
	return true;
}

// One undo step for a tag or bake edit of all items, prev holds the snapshots from before it. Items the edit left unchanged get no step.
static void ui_asset_details_push_edit(any_array_t *prev) {
	history_group_begin();
	for (i32 i = 0; i < prev->length; ++i) {
		if (ui_asset_details_same(prev->buffer[i], ui_asset_details_peek(i))) {
			meta_free(prev->buffer[i]);
		}
		else {
			history_edit_meta(ui_asset_details_category, ui_asset_details_index(i), prev->buffer[i]);
		}
	}
	history_group_end();
	array_delete(prev);
}

// A combo with a search bar hands over a value for each typed letter or arrow key while it is open, and the old value again on Escape.
// The values apply at once so the dialog shows them, the undo step waits until the combo closes (ui_asset_details_combo_commit).
static void ui_asset_details_combo_edit() {
	if (ui_asset_details_combo_prev == NULL) {
		ui_asset_details_combo_prev = ui_asset_details_snapshot();
	}
}

// Once no combo is open: one undo step when the bake keys differ from those before the combo edit, none when they came back to them
static void ui_asset_details_combo_commit() {
	if (ui_asset_details_combo_prev == NULL || g_ui->combo_selected_id != 0) {
		return;
	}
	any_array_t *prev           = ui_asset_details_combo_prev;
	ui_asset_details_combo_prev = NULL;
	for (i32 i = 0; i < prev->length; ++i) {
		i32 btp = meta_get_i32(ui_asset_details_peek(i), META_BAKE_TYPE, BTP_NONE);
		if (btp != meta_get_i32(prev->buffer[i], META_BAKE_TYPE, BTP_NONE)) {
			ui_asset_details_last_btp = btp;
		}
	}
	ui_asset_details_push_edit(prev);
}

static bool ui_asset_details_has_name() {
	return ui_asset_details_category == MC_MATERIAL || ui_asset_details_category == MC_BRUSH;
}

static char *ui_asset_details_current_name() {
	if (ui_asset_details_category == MC_MATERIAL) {
		return g_project->_->materials->buffer[ui_asset_details_index(0)]->canvas->name;
	}
	return g_project->_->brushes->buffer[ui_asset_details_index(0)]->canvas->name;
}

// Commits the name field to the asset (one undo step, none when unchanged) and reloads the field from the asset
static void ui_asset_details_commit_name() {
	if (!ui_asset_details_has_name()) {
		return;
	}
	if (asset_rename(ui_asset_details_category, ui_asset_details_index(0), ui_asset_details_name)) {
		base_redraw_ui();
		ui_nodes_hwnd->redraws = 2;
	}
	ui_asset_details_name = string_copy(ui_asset_details_current_name());
}

static void ui_asset_details_draw_name() {
	ui_id_t name_id = ui_widget_id(&ui_asset_details_name, UI_ID_TEXT);
	if (g_ui->text_selected_id != name_id) {
		ui_asset_details_name = ui_asset_details_current_name(); // Follows undo and redo
	}
	ui_set_next_id((ui_id_t)&ui_asset_details_name);
	ui_text_input(&ui_asset_details_name, tr("Name"), UI_ALIGN_LEFT, true, false);
	if (ui_item_changed()) { // The edit ended (Enter, Tab or a click elsewhere): the field hands over its text only then, Escape hands back the old one
		ui_asset_details_commit_name();
	}
}

// Rebuilds the tag list of the edited items when their metadata changed (checked once per frame, no string is read for it)
static void ui_asset_details_tags_update() {
	if (ui_asset_details_tag_valid && ui_asset_details_tag_frame == g_context->frame) {
		return;
	}
	u64 hash = 0xcbf29ce484222325ull ^ (u64)ui_asset_details_count();
	for (i32 i = 0; i < ui_asset_details_count(); ++i) {
		hash = ui_asset_tags_hash(ui_asset_details_peek(i), hash);
	}
	ui_asset_details_tag_frame = g_context->frame;
	if (ui_asset_details_tag_valid && ui_asset_details_tag_hash == hash) {
		return;
	}
	for (i32 i = 0; ui_asset_details_tag_list != NULL && i < ui_asset_details_tag_list->length; ++i) {
		free(ui_asset_details_tag_list->buffer[i]);
	}
	array_delete(ui_asset_details_tag_list);
	array_delete(ui_asset_details_tag_have);
	ui_asset_details_tag_list = string_array_create(0);
	ui_asset_details_tag_have = i32_array_create(0);
	string_array_t *lower     = string_array_create(0);
	i32_array_t    *last_item = i32_array_create(0); // Last item counted for the tag, a tag twice on one item counts once
	for (i32 i = 0; i < ui_asset_details_count(); ++i) {
		meta_t *m = ui_asset_details_peek(i);
		for (i32 j = 0; m != NULL && m->tags != NULL && j < m->tags->length; ++j) {
			char *t = m->tags->buffer[j];
			char *l = string_to_lower_tmp(t);
			i32   k = string_array_index_of(lower, l);
			if (k < 0) {
				char *copy = malloc(strlen(t) + 1);
				strcpy(copy, t);
				string_array_push(ui_asset_details_tag_list, copy);
				string_array_push(lower, l);
				i32_array_push(ui_asset_details_tag_have, 0);
				i32_array_push(last_item, -1);
				k = lower->length - 1;
			}
			if (last_item->buffer[k] != i) {
				last_item->buffer[k] = i;
				ui_asset_details_tag_have->buffer[k]++;
			}
		}
	}
	array_delete(lower);
	array_delete(last_item);
	ui_asset_details_tag_hash  = hash;
	ui_asset_details_tag_valid = true;
}

static i32 ui_asset_details_tag_count() {
	ui_asset_details_tags_update();
	return ui_asset_details_tag_list->length;
}

// Adds (or removes) the tag on every item, one undo step
static void ui_asset_details_set_tag(char *tag, bool add) {
	any_array_t *prev = ui_asset_details_snapshot();
	for (i32 i = 0; i < ui_asset_details_count(); ++i) {
		if (add) {
			meta_add_tag(ui_asset_details_meta(i), tag);
		}
		else {
			meta_remove_tag(ui_asset_details_peek(i), tag);
		}
	}
	ui_asset_details_push_edit(prev);
	ui_asset_details_tag_valid = false;
	base_redraw_ui();
}

static void ui_asset_details_draw_tags() {
	ui_text(tr("Tags"), UI_ALIGN_LEFT, 0x00000000);

	// A tag on all items: name and X. A tag on some items only: dimmed name with the share, + adds it to the others, X removes it from all.
	ui_asset_details_tags_update();
	string_array_t *tags = ui_asset_details_tag_list;
	i32             n    = ui_asset_details_count();
	char           *edit = NULL;
	bool            add  = false;
	for (i32 i = 0; i < tags->length; ++i) {
		char *t    = tags->buffer[i];
		i32   have = ui_asset_details_tag_have->buffer[i];
		if (have == n) {
			f32_array_t *row = f32_array_create_from_raw_tmp(
			    (f32[]){
			        7 / 8.0,
			        1 / 8.0,
			    },
			    2);
			ui_row(row);
			ui_text(t, UI_ALIGN_LEFT, 0x00000000);
		}
		else {
			f32_array_t *row = f32_array_create_from_raw_tmp(
			    (f32[]){
			        6 / 8.0,
			        1 / 8.0,
			        1 / 8.0,
			    },
			    3);
			ui_row(row);
			i32 text_col      = g_theme->TEXT_COL;
			g_theme->TEXT_COL = g_theme->LABEL_COL;
			ui_text(string_tmp("%s (%d/%d)", t, have, n), UI_ALIGN_LEFT, 0x00000000);
			g_theme->TEXT_COL = text_col;
			if (ui_button(tr("+"), UI_ALIGN_CENTER, "")) {
				edit = t;
				add  = true;
			}
			if (g_ui->is_hovered) {
				ui_tooltip(tr("Add to all"));
			}
		}
		if (ui_button(tr("X"), UI_ALIGN_CENTER, "")) {
			edit = t;
			add  = false;
		}
	}
	if (edit != NULL) {
		ui_asset_details_set_tag(string_copy(edit), add); // Copied, the tag list is rebuilt
	}

	f32_array_t *row = f32_array_create_from_raw_tmp(
	    (f32[]){
	        2 / 3.0,
	        1 / 3.0,
	    },
	    2);
	ui_row(row);
	ui_text_input(&ui_asset_details_input, tr("Tag"), UI_ALIGN_LEFT, true, false);
	if (ui_icon_button(tr("Add"), ICON_PLUS, UI_ALIGN_CENTER) || g_ui->is_return_down) {
		if (!string_equals(ui_asset_details_input, "")) {
			ui_asset_details_set_tag(ui_asset_details_input, true); // No step for an empty tag or one every item has
			ui_asset_details_input = "";
		}
	}
}

static void ui_asset_details_draw_bake() {
	i32 n       = ui_asset_details_count();
	i32 baked_n = 0;
	for (i32 i = 0; i < n; ++i) {
		baked_n += meta_get_i32(ui_asset_details_peek(i), META_BAKE_TYPE, BTP_NONE) != BTP_NONE ? 1 : 0;
	}
	bool mixed = baked_n > 0 && baked_n < n;
	bool baked = baked_n == n;
	bool prev  = baked;
	if (mixed) {
		f32_array_t *row = f32_array_create_from_raw_tmp(
		    (f32[]){
		        2 / 3.0,
		        1 / 3.0,
		    },
		    2);
		ui_row(row);
	}
	ui_check(&baked, tr("Bake Texture"), "");
	if (mixed) {
		i32 text_col      = g_theme->TEXT_COL;
		g_theme->TEXT_COL = g_theme->LABEL_COL;
		ui_text(tr("Mixed"), UI_ALIGN_RIGHT, 0x00000000);
		g_theme->TEXT_COL = text_col;
	}
	if (baked != prev) {
		// On: the unbaked items get the last used type, items already baked keep their keys. Off: every item is cleared.
		any_array_t *before = ui_asset_details_snapshot();
		i32          t      = ui_asset_details_last_btp != BTP_NONE ? ui_asset_details_last_btp : BTP_AO;
		for (i32 i = 0; i < n; ++i) {
			if (!baked) {
				meta_clear_bake(ui_asset_details_peek(i));
			}
			else if (meta_get_i32(ui_asset_details_peek(i), META_BAKE_TYPE, BTP_NONE) == BTP_NONE) {
				meta_set_bake(ui_asset_details_meta(i), t, asset_bake_default_encoding(t));
			}
		}
		ui_asset_details_push_edit(before);
		return;
	}
	if (!baked) {
		return;
	}
	i32 btp = ui_asset_details_shared(META_BAKE_TYPE, BTP_NONE);
	i32 enc = ui_asset_details_shared(META_ENCODING, AE_NONE);

	// Type
	string_array_t *ar  = any_array_create(0);
	i32_array_t    *ids = i32_array_create(0);
	for (i32 i = 0; i < asset_bake_type_count(); ++i) {
		any_array_push(ar, asset_bake_type_label(asset_bake_type_at(i)));
		i32_array_push(ids, asset_bake_type_at(i));
	}
	i32 new_btp = btp;
	if (ui_asset_details_combo(&new_btp, ar, ids, tr("Type"), &asset_bake_type_label, 0)) {
		// A type change refills the encoding too
		ui_asset_details_combo_edit();
		for (i32 i = 0; i < n; ++i) {
			meta_set_bake(ui_asset_details_meta(i), new_btp, asset_bake_default_encoding(new_btp));
		}
		return;
	}

	// Encoding
	ar  = any_array_create(0);
	ids = i32_array_create(0);
	for (i32 e = AE_COLOR; e <= AE_NORMAL; ++e) {
		any_array_push(ar, asset_encoding_label(e));
		i32_array_push(ids, e);
	}
	i32 new_enc = enc;
	if (ui_asset_details_combo(&new_enc, ar, ids, tr("Encoding"), &asset_encoding_label, 1)) {
		// Only the encoding is written, each item keeps its type
		ui_asset_details_combo_edit();
		for (i32 i = 0; i < n; ++i) {
			meta_set_i32(ui_asset_details_meta(i), META_ENCODING, new_enc);
		}
	}
}

// Box height in unscaled units: the fixed height of the category grows by one row per tag beyond two, up to the window height.
// Past that the window scrolls, so every tag stays reachable.
static i32 ui_asset_details_height() {
	i32 mh = 320;
	if (ui_asset_details_category == MC_TEXTURE) {
		mh = 400;
	}
	else if (ui_asset_details_has_name()) {
		mh = 360;
	}
	i32 tags = ui_asset_details_tag_count();
	f32 row  = (UI_ELEMENT_H() + UI_ELEMENT_OFFSET()) / (f32)UI_SCALE();
	if (tags > 2) {
		mh += math_ceil((tags - 2) * row);
	}
	if (ui_asset_details_multi() && !ui_asset_details_has_name()) { // The item count line, it takes the place of the name field
		mh += math_ceil(row);
	}
	i32 max = math_floor(iron_window_height() / (f32)UI_SCALE()) - 20;
	return mh < max ? mh : max;
}

static void ui_asset_details_draw_count() {
	any_map_t *vars = any_map_create();
	any_map_set(vars, "count", string_tmp("%d", ui_asset_details_count()));
	ui_text(vtr("{count} items", vars), UI_ALIGN_LEFT, 0x00000000);
	map_free(vars);
}

static void ui_asset_details_box_draw() {
	ui_asset_details_refresh();
	if (ui_asset_details_count() == 0) {
		ui_asset_details_combo_prev = NULL;
		ui_box_hide();
		return;
	}
	ui_box_modalh = ui_asset_details_height();
	if (ui_asset_details_multi()) {
		ui_asset_details_draw_count();
	}
	else if (ui_asset_details_has_name()) {
		ui_asset_details_draw_name();
	}
	ui_asset_details_draw_tags();
	if (ui_asset_details_category == MC_TEXTURE) {
		ui_asset_details_draw_bake();
		ui_asset_details_combo_commit();
	}
	ui_end_element();
	if (ui_icon_button(tr("OK"), ICON_CHECK, UI_ALIGN_CENTER)) {
		ui_box_hide();
	}
}

// A combo closed with Enter is committed in the next draw, unless the dialog closes first (Escape or a click outside)
static void ui_asset_details_on_hide() {
	ui_asset_details_refresh();
	ui_asset_details_combo_commit();
	// A tag field left in edit mode (OK right after Add) would keep is_typing set and block the undo and redo shortcuts until Escape
	if (g_ui->text_selected_id != 0) {
		ui_deselect_text(g_ui);
		g_ui->submit_text_id    = 0; // Drop the pending text, the dialog is gone
		g_ui->text_to_submit[0] = '\0';
	}
}

static asset_tray_t ui_asset_details_tray(i32 category) {
	switch (category) {
	case MC_TEXTURE:
		return ASSET_TRAY_TEXTURES;
	case MC_MATERIAL:
		return ASSET_TRAY_MATERIALS;
	case MC_BRUSH:
		return ASSET_TRAY_BRUSHES;
	case MC_FONT:
		return ASSET_TRAY_FONTS;
	case MC_SOUND:
		return ASSET_TRAY_SOUNDS;
	}
	return ASSET_TRAY_NONE;
}

// Opens the details dialog of an item of a category (meta_category_t), for the whole selection of its tray when the item is part of it
void ui_asset_details_show(i32 category, i32 index) {
	void *item = meta_item(category, index);
	if (item == NULL) {
		return;
	}
	asset_tray_t tray     = ui_asset_details_tray(category);
	any_array_t *selected = tray != ASSET_TRAY_NONE && asset_selection_contains(tray, item) ? asset_selection_get_tray_order(tray) : NULL;
	any_array_t *items    = any_array_create(0);
	for (i32 i = 0; selected != NULL && i < selected->length; ++i) {
		if (category != MC_FONT || meta_item_index(category, selected->buffer[i]) > 0) { // Not the built-in default font, it takes no metadata
			any_array_push(items, selected->buffer[i]);
		}
	}
	array_delete(selected);
	if (items->length == 0) {
		any_array_push(items, item);
	}
	array_delete(ui_asset_details_items);
	ui_asset_details_items = items;
	array_delete(ui_asset_details_hints);
	ui_asset_details_hints = i32_array_create(0);
	for (i32 i = 0; i < items->length; ++i) {
		i32_array_push(ui_asset_details_hints, -1);
	}
	ui_asset_details_tag_valid  = false;
	ui_asset_details_category   = category;
	ui_asset_details_combo_prev = NULL;
	ui_asset_details_input      = "";
	ui_asset_details_name       = "";
	for (i32 i = 0; i < 2; ++i) {
		ui_asset_details_had_mixed[i] = false;
	}
	ui_box_show_custom(&ui_asset_details_box_draw, 400, ui_asset_details_height(), &ui_asset_details_on_hide, true, tr("Details"));
}
