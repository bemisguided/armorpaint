
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
	bool changed = ui_item_changed();
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
		ui_asset_details_last_btp = new_btp;
		meta_t *prev              = ui_asset_details_snapshot();
		meta_set_bake(meta_get(MC_TEXTURE, ui_asset_details_index), new_btp, asset_bake_default_encoding(new_btp), ui_asset_details_obj, ui_asset_details_tile);
		ui_asset_details_push_edit(prev);
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
		meta_t *prev = ui_asset_details_snapshot();
		meta_set_bake(meta_get(MC_TEXTURE, ui_asset_details_index), btp, new_enc, new_obj, new_tile);
		ui_asset_details_push_edit(prev);
	}
}

static void ui_asset_details_box_draw() {
	i32 count = meta_count(ui_asset_details_category);
	if (ui_asset_details_index < 0 || ui_asset_details_index >= count) {
		ui_box_hide();
		return;
	}
	if (ui_asset_details_has_name()) {
		ui_asset_details_draw_name();
	}
	ui_asset_details_draw_tags();
	if (ui_asset_details_category == MC_TEXTURE) {
		ui_asset_details_draw_bake();
	}
	ui_end_element();
	if (ui_icon_button(tr("OK"), ICON_CHECK, UI_ALIGN_CENTER)) {
		ui_box_hide();
	}
}

// Opens the details dialog of an item of a category (meta_category_t)
void ui_asset_details_show(i32 category, i32 index) {
	if (index < 0 || index >= meta_count(category)) {
		return;
	}
	ui_asset_details_category = category;
	ui_asset_details_index    = index;
	ui_asset_details_input    = "";
	ui_asset_details_name     = "";
	ui_asset_details_obj      = -1;
	ui_asset_details_tile     = -1;
	i32 mh                    = 320;
	if (category == MC_TEXTURE) {
		meta_t *m             = meta_peek(MC_TEXTURE, index);
		ui_asset_details_obj  = meta_get_i32(m, META_OBJECT, -1);
		ui_asset_details_tile = meta_get_i32(m, META_TILE, -1);
		mh                    = 460;
	}
	else if (category == MC_MATERIAL || category == MC_BRUSH) {
		mh = 360;
	}
	ui_box_show_custom(&ui_asset_details_box_draw, 400, mh, NULL, true, tr("Details"));
}
