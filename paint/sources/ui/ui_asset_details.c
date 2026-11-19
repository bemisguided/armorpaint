#include "../global.h"

// "Details" dialog of an asset of a category (meta_category_t), shared by the Textures, Materials, Brushes, Fonts and Sounds trays.
// Name (Materials and Brushes only), Tags, and for Textures the Bake Texture section (bake type and encoding, see util_meta.c).
// Changes apply at once, the name commits on Enter or when the field loses focus (a click anywhere else, Tab). Closing the dialog needs no commit of its own:
// the click on OK or outside ends the edit first, and the field commits in the last frame the dialog draws.
// Every edit of the tags or bake keys is one undo step.

static i32     ui_asset_details_category = 0;
static void   *ui_asset_details_item     = NULL; // Edited item (asset_t, slot_material_t...)
static char   *ui_asset_details_input    = "";   // New tag
static char   *ui_asset_details_name     = "";   // Name field text, committed to the asset on Enter or focus loss
static i32     ui_asset_details_ids[2];
static i32     ui_asset_details_last_btp = BTP_NONE; // Last bake type chosen in this session, BTP_NONE = none yet
// Metadata of the item from before the first change made through the open combo, see ui_asset_details_combo_commit
static meta_t *ui_asset_details_combo_prev = NULL;

// Index of the edited item in the live list of its category, -1 when it is gone (deleted, undo of an add)
static i32 ui_asset_details_index() {
	return meta_item_index(ui_asset_details_category, ui_asset_details_item);
}

static meta_t *ui_asset_details_peek() {
	return meta_peek(ui_asset_details_category, ui_asset_details_index());
}

static meta_t *ui_asset_details_meta() {
	return meta_get(ui_asset_details_category, ui_asset_details_index());
}

// Combo over ids, an id that is not in the list is appended as `Unknown (n)` so it is shown and kept.
// Returns true and sets value when changed.
static bool ui_asset_details_combo(i32 *value, string_array_t *ar, i32_array_t *ids, char *label, char *(*label_of)(i32), i32 id_slot) {
	i32 pos = i32_array_index_of(ids, *value);
	if (pos == -1) {
		any_array_push(ar, label_of(*value));
		i32_array_push(ids, *value);
		pos = ids->length - 1;
	}
	ui_set_next_id((ui_id_t)&ui_asset_details_ids[id_slot]);
	ui_combo(&pos, ar, label, true, UI_ALIGN_LEFT, true);
	bool changed = ui_item_changed() && ids->buffer[pos] != *value; // Escape hands back the old value
	if (changed) {
		*value = ids->buffer[pos];
	}
	array_free(ar);
	free(ar);
	array_free(ids);
	free(ids);
	return changed;
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

// One undo step for a tag or bake edit, prev is a copy of the metadata from before it. No step when the edit left it unchanged.
static void ui_asset_details_push_edit(meta_t *prev) {
	if (ui_asset_details_same(prev, ui_asset_details_peek())) {
		meta_free(prev);
	}
	else {
		history_edit_meta(ui_asset_details_category, ui_asset_details_index(), prev);
	}
}

// A combo with a search bar hands over a value for each typed letter or arrow key while it is open, and the old value again on Escape.
// The values apply at once so the dialog shows them, the undo step waits until the combo closes (ui_asset_details_combo_commit).
static void ui_asset_details_combo_edit() {
	if (ui_asset_details_combo_prev == NULL) {
		ui_asset_details_combo_prev = meta_copy(ui_asset_details_peek());
	}
}

// Once no combo is open: one undo step when the bake keys differ from those before the combo edit, none when they came back to them
static void ui_asset_details_combo_commit() {
	if (ui_asset_details_combo_prev == NULL || g_ui->combo_selected_id != 0) {
		return;
	}
	meta_t *prev                = ui_asset_details_combo_prev;
	ui_asset_details_combo_prev = NULL;
	i32 btp                     = meta_get_i32(ui_asset_details_peek(), META_BAKE_TYPE, BTP_NONE);
	if (btp != meta_get_i32(prev, META_BAKE_TYPE, BTP_NONE)) {
		ui_asset_details_last_btp = btp;
	}
	ui_asset_details_push_edit(prev);
}

static bool ui_asset_details_has_name() {
	return ui_asset_details_category == MC_MATERIAL || ui_asset_details_category == MC_BRUSH;
}

static char *ui_asset_details_current_name() {
	if (ui_asset_details_category == MC_MATERIAL) {
		return g_project->_->materials->buffer[ui_asset_details_index()]->canvas->name;
	}
	return g_project->_->brushes->buffer[ui_asset_details_index()]->canvas->name;
}

// Commits the name field to the asset (one undo step, none when unchanged) and reloads the field from the asset
static void ui_asset_details_commit_name() {
	if (!ui_asset_details_has_name()) {
		return;
	}
	if (asset_rename(ui_asset_details_category, ui_asset_details_index(), ui_asset_details_name)) {
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

static i32 ui_asset_details_tag_count() {
	meta_t *m = ui_asset_details_peek();
	return m != NULL && m->tags != NULL ? m->tags->length : 0;
}

// Adds (or removes) the tag, one undo step
static void ui_asset_details_set_tag(char *tag, bool add) {
	meta_t *prev = meta_copy(ui_asset_details_peek());
	if (add) {
		meta_add_tag(ui_asset_details_meta(), tag);
	}
	else {
		meta_remove_tag(ui_asset_details_peek(), tag);
	}
	ui_asset_details_push_edit(prev);
	base_redraw_ui();
}

static void ui_asset_details_draw_tags() {
	ui_text(tr("Tags"), UI_ALIGN_LEFT, 0x00000000);

	meta_t *m      = ui_asset_details_peek();
	char   *remove = NULL;
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
			remove = m->tags->buffer[i];
		}
	}
	if (remove != NULL) {
		ui_asset_details_set_tag(remove, false);
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
			ui_asset_details_set_tag(ui_asset_details_input, true); // No step for an empty tag or one the item has
			ui_asset_details_input = "";
		}
	}
}

static void ui_asset_details_draw_bake() {
	bool baked = meta_get_i32(ui_asset_details_peek(), META_BAKE_TYPE, BTP_NONE) != BTP_NONE;
	bool prev  = baked;
	ui_check(&baked, tr("Bake Texture"), "");
	if (baked != prev) {
		// On: the item gets the last used type. Off: its bake keys are cleared.
		meta_t *before = meta_copy(ui_asset_details_peek());
		i32     t      = ui_asset_details_last_btp != BTP_NONE ? ui_asset_details_last_btp : BTP_AO;
		if (baked) {
			meta_set_bake(ui_asset_details_meta(), t, asset_bake_default_encoding(t));
		}
		else {
			meta_clear_bake(ui_asset_details_peek());
		}
		ui_asset_details_push_edit(before);
		return;
	}
	if (!baked) {
		return;
	}
	i32 btp = meta_get_i32(ui_asset_details_peek(), META_BAKE_TYPE, BTP_NONE);
	i32 enc = meta_get_i32(ui_asset_details_peek(), META_ENCODING, AE_NONE);

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
		meta_set_bake(ui_asset_details_meta(), new_btp, asset_bake_default_encoding(new_btp));
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
		ui_asset_details_combo_edit();
		meta_set_i32(ui_asset_details_meta(), META_ENCODING, new_enc);
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
	i32 max = math_floor(iron_window_height() / (f32)UI_SCALE()) - 20;
	return mh < max ? mh : max;
}

static void ui_asset_details_box_draw() {
	if (ui_asset_details_index() == -1) { // The item is gone
		ui_asset_details_combo_prev = NULL;
		ui_box_hide();
		return;
	}
	ui_box_modalh = ui_asset_details_height();
	if (ui_asset_details_has_name()) {
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
	if (ui_asset_details_index() == -1) {
		ui_asset_details_combo_prev = NULL;
	}
	ui_asset_details_combo_commit();
	// A tag field left in edit mode (OK right after Add) would keep is_typing set and block the undo and redo shortcuts until Escape
	if (g_ui->text_selected_id != 0) {
		ui_deselect_text(g_ui);
		g_ui->submit_text_id    = 0; // Drop the pending text, the dialog is gone
		g_ui->text_to_submit[0] = '\0';
	}
}

// Opens the details dialog of an item of a category (meta_category_t)
void ui_asset_details_show(i32 category, i32 index) {
	void *item = meta_item(category, index);
	if (item == NULL || (category == MC_FONT && index == 0)) { // The built-in default font takes no metadata
		return;
	}
	ui_asset_details_item       = item;
	ui_asset_details_category   = category;
	ui_asset_details_combo_prev = NULL;
	ui_asset_details_input      = "";
	ui_asset_details_name       = "";
	ui_box_show_custom(&ui_asset_details_box_draw, 400, ui_asset_details_height(), &ui_asset_details_on_hide, true, tr("Details"));
}
