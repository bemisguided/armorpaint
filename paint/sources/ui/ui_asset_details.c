
#include "../global.h"

// "Details" dialog of an asset of a category (meta_category_t), shared by the Textures, Materials, Brushes, Fonts and Sounds trays.
// Name (Materials and Brushes only), Tags, and for Textures the Bake Texture section (bake type, object, encoding, tile, see util_meta.c).
// Changes apply at once, the name commits on Enter or when the field loses focus (a click anywhere else, Tab). Closing the dialog needs no commit of its own:
// the click on OK or outside ends the edit first, and the field commits in the last frame the dialog draws.

static i32     ui_asset_details_category = 0;
static i32     ui_asset_details_index    = 0;
static char   *ui_asset_details_input    = ""; // New tag
static char   *ui_asset_details_name     = ""; // Name field text, committed to the asset on Enter or focus loss
static i32     ui_asset_details_ids[4];
static i32     ui_asset_details_last_btp = BTP_NONE; // Last bake type chosen in this session, BTP_NONE = none yet
static i32     ui_asset_details_obj      = -1;       // Object and Tile are kept while the dialog is open so a type change never clears them
static i32     ui_asset_details_tile     = -1;
static meta_t *ui_asset_details_combo_prev = NULL; // Metadata from before the first change made through the open combo, see ui_asset_details_combo_commit

// Combo over ids, an id that is not in the list is appended as `Unknown (n)` so it is shown and kept. Returns true and sets value when changed.
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

// Copy of the metadata of the item, taken before an edit and handed to ui_asset_details_push_edit once the edit is applied
static meta_t *ui_asset_details_snapshot() {
	return meta_copy(meta_peek(ui_asset_details_category, ui_asset_details_index));
}

// One undo step for a tag or bake edit, prev is the snapshot from before it
static void ui_asset_details_push_edit(meta_t *prev) {
	history_edit_meta(ui_asset_details_category, ui_asset_details_index, prev);
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
	meta_t *prev                = ui_asset_details_combo_prev;
	meta_t *m                   = meta_peek(MC_TEXTURE, ui_asset_details_index);
	ui_asset_details_combo_prev = NULL;
	i32  keys[]                 = {META_BAKE_TYPE, META_ENCODING, META_OBJECT, META_TILE};
	bool same                   = true;
	for (i32 i = 0; i < 4; ++i) {
		same = same && meta_get_i32(prev, keys[i], -1) == meta_get_i32(m, keys[i], -1);
	}
	if (same) {
		return;
	}
	i32 btp = meta_get_i32(m, META_BAKE_TYPE, BTP_NONE);
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
		return g_project->_->materials->buffer[ui_asset_details_index]->canvas->name;
	}
	return g_project->_->brushes->buffer[ui_asset_details_index]->canvas->name;
}

// Commits the name field to the asset (one undo step, none when unchanged) and reloads the field from the asset
static void ui_asset_details_commit_name() {
	if (!ui_asset_details_has_name()) {
		return;
	}
	if (asset_rename(ui_asset_details_category, ui_asset_details_index, ui_asset_details_name)) {
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

static void ui_asset_details_draw_tags() {
	meta_t *m = meta_peek(ui_asset_details_category, ui_asset_details_index);
	ui_text(tr("Tags"), UI_ALIGN_LEFT, 0x00000000);

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
		meta_t *prev = ui_asset_details_snapshot();
		meta_remove_tag(m, string_copy(m->tags->buffer[remove]));
		ui_asset_details_push_edit(prev);
		base_redraw_ui();
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
			meta_t       *prev = ui_asset_details_snapshot();
			i32           n    = m != NULL && m->tags != NULL ? m->tags->length : 0;
			meta_add_tag(meta_get(ui_asset_details_category, ui_asset_details_index), ui_asset_details_input);
			m = meta_peek(ui_asset_details_category, ui_asset_details_index);
			if ((m != NULL && m->tags != NULL ? m->tags->length : 0) != n) { // Not for an empty or duplicate tag
				ui_asset_details_push_edit(prev);
			}
			ui_asset_details_input = "";
			base_redraw_ui();
		}
	}
}

static void ui_asset_details_draw_bake() {
	meta_t *m     = meta_peek(MC_TEXTURE, ui_asset_details_index);
	i32     btp   = meta_get_i32(m, META_BAKE_TYPE, BTP_NONE);
	bool baked = btp != BTP_NONE;
	bool prev  = baked;
	ui_check(&baked, tr("Bake Texture"), "");
	if (baked != prev) {
		meta_t *before = ui_asset_details_snapshot();
		m              = meta_get(MC_TEXTURE, ui_asset_details_index);
		if (baked) {
			i32 t = ui_asset_details_last_btp != BTP_NONE ? ui_asset_details_last_btp : BTP_AO;
			ui_asset_details_obj  = -1;
			ui_asset_details_tile = -1;
			meta_set_bake(m, t, asset_bake_default_encoding(t), -1, -1);
		}
		else {
			meta_clear_bake(m);
			ui_asset_details_obj  = -1;
			ui_asset_details_tile = -1;
		}
		ui_asset_details_push_edit(before);
		return;
	}
	if (!baked) {
		return;
	}
	i32 enc               = meta_get_i32(m, META_ENCODING, AE_NONE);
	i32 obj               = meta_get_i32(m, META_OBJECT, -1);
	i32 tile              = meta_get_i32(m, META_TILE, -1);
	ui_asset_details_obj  = obj; // Follows undo and redo
	ui_asset_details_tile = tile;

	// Type
	string_array_t *ar  = any_array_create(0);
	i32_array_t    *ids = i32_array_create(0);
	for (i32 i = 0; i < asset_bake_type_count(); ++i) {
		any_array_push(ar, asset_bake_type_label(asset_bake_type_at(i)));
		i32_array_push(ids, asset_bake_type_at(i));
	}
	i32 new_btp = btp;
	if (ui_asset_details_combo(&new_btp, ar, ids, tr("Type"), &asset_bake_type_label, 0)) {
		// A type change refills the encoding only, Object and Tile come from the dialog so a changing type (typed search) never loses them
		ui_asset_details_combo_edit();
		meta_set_bake(meta_get(MC_TEXTURE, ui_asset_details_index), new_btp, asset_bake_default_encoding(new_btp), ui_asset_details_obj, ui_asset_details_tile);
		return;
	}

	// Object, Any first, then the paint objects by paint_id
	ar  = any_array_create(0);
	ids = i32_array_create(0);
	any_array_push(ar, tr("Any"));
	i32_array_push(ids, -1);
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		mesh_object_t *o = g_project->_->paint_objects->buffer[i];
		if (o->paint_id > 0 && o->data->index_array->length > 0) { // Like the layer object combo, no empty group objects (Root)
			any_array_push(ar, o->base->name);
			i32_array_push(ids, o->paint_id);
		}
	}
	i32  new_obj = obj;
	bool changed = ui_asset_details_combo(&new_obj, ar, ids, tr("Object"), &asset_unknown_label, 1);
	if (changed) {
		ui_asset_details_obj = new_obj;
	}

	// Encoding
	ar  = any_array_create(0);
	ids = i32_array_create(0);
	for (i32 e = AE_COLOR; e <= AE_NORMAL; ++e) {
		any_array_push(ar, asset_encoding_label(e));
		i32_array_push(ids, e);
	}
	i32 new_enc = enc;
	changed     = ui_asset_details_combo(&new_enc, ar, ids, tr("Encoding"), &asset_encoding_label, 2) || changed;

	// Tile, only in UDIM projects, Any first, then the project tiles
	i32 new_tile = tile;
	if (util_udim_active()) {
		ar  = any_array_create(0);
		ids = i32_array_create(0);
		any_array_push(ar, tr("Any"));
		i32_array_push(ids, -1);
		for (i32 i = 0; i < util_udim_count(); ++i) {
			any_array_push(ar, string_tmp("%d", util_udim_tile(i)));
			i32_array_push(ids, util_udim_tile(i));
		}
		if (ui_asset_details_combo(&new_tile, ar, ids, tr("Tile"), &asset_unknown_label, 3)) {
			ui_asset_details_tile = new_tile;
			changed               = true;
		}
	}

	if (changed) {
		ui_asset_details_combo_edit();
		meta_set_bake(meta_get(MC_TEXTURE, ui_asset_details_index), btp, new_enc, new_obj, new_tile);
	}
}

// Box height in unscaled units: the fixed height of the category grows by one row per tag beyond two, up to the window height.
// Past that the window scrolls, so every tag stays reachable.
static i32 ui_asset_details_height() {
	i32 mh = 320;
	if (ui_asset_details_category == MC_TEXTURE) {
		mh = 460;
	}
	else if (ui_asset_details_category == MC_MATERIAL || ui_asset_details_category == MC_BRUSH) {
		mh = 360;
	}
	meta_t       *m    = meta_peek(ui_asset_details_category, ui_asset_details_index);
	i32           tags = m != NULL && m->tags != NULL ? m->tags->length : 0;
	f32           row  = (UI_ELEMENT_H() + UI_ELEMENT_OFFSET()) / (f32)UI_SCALE();
	if (tags > 2) {
		mh += math_ceil((tags - 2) * row);
	}
	i32 max = math_floor(iron_window_height() / (f32)UI_SCALE()) - 20;
	return mh < max ? mh : max;
}

static void ui_asset_details_box_draw() {
	i32 count = meta_count(ui_asset_details_category);
	if (ui_asset_details_index < 0 || ui_asset_details_index >= count) {
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
	if (index < 0 || index >= meta_count(category)) {
		return;
	}
	ui_asset_details_category   = category;
	ui_asset_details_index      = index;
	ui_asset_details_combo_prev = NULL;
	ui_asset_details_input      = "";
	ui_asset_details_name       = "";
	ui_asset_details_obj        = -1;
	ui_asset_details_tile       = -1;
	if (category == MC_TEXTURE) {
		meta_t *m             = meta_peek(MC_TEXTURE, index);
		ui_asset_details_obj  = meta_get_i32(m, META_OBJECT, -1);
		ui_asset_details_tile = meta_get_i32(m, META_TILE, -1);
	}
	ui_box_show_custom(&ui_asset_details_box_draw, 400, ui_asset_details_height(), &ui_asset_details_on_hide, true, tr("Details"));
}
