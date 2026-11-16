#include "../global.h"

// Bake properties of a texture (bake type, encoding, object, tile), see util_meta.c. Changes apply to the meta at once.

static i32 ui_asset_props_index = -1;
static i32 ui_asset_props_ids[4];

// Combo over ids, an id that is not in the list is appended as `Unknown (n)` so it is shown and kept. Returns true and sets value when changed.
static bool ui_asset_props_combo(i32 *value, string_array_t *ar, i32_array_t *ids, char *label, char *(*label_of)(i32), i32 id_slot) {
	i32 pos = i32_array_index_of(ids, *value);
	if (pos == -1) {
		any_array_push(ar, label_of(*value));
		i32_array_push(ids, *value);
		pos = ids->length - 1;
	}
	ui_set_next_id((ui_id_t)&ui_asset_props_ids[id_slot]);
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

static void ui_asset_props_draw() {
	if (ui_asset_props_index < 0 || ui_asset_props_index >= g_project->_->assets->length) {
		ui_box_hide();
		return;
	}
	meta_t *m    = meta_peek(MC_TEXTURE, ui_asset_props_index);
	i32     btp  = meta_get_i32(m, META_BAKE_TYPE, BTP_NONE);
	i32     enc  = meta_get_i32(m, META_ENCODING, AE_NONE);
	i32     obj  = meta_get_i32(m, META_OBJECT, -1);
	i32     tile = meta_get_i32(m, META_TILE, -1);

	// Bake type, None first
	string_array_t *ar  = any_array_create(0);
	i32_array_t    *ids = i32_array_create(0);
	any_array_push(ar, asset_bake_type_label(BTP_NONE));
	i32_array_push(ids, BTP_NONE);
	for (i32 i = 0; i < asset_bake_type_count(); ++i) {
		any_array_push(ar, asset_bake_type_label(asset_bake_type_at(i)));
		i32_array_push(ids, asset_bake_type_at(i));
	}
	i32 new_btp = btp;
	if (ui_asset_props_combo(&new_btp, ar, ids, tr("Bake Type"), &asset_bake_type_label, 0)) {
		m = meta_get(MC_TEXTURE, ui_asset_props_index);
		if (new_btp == BTP_NONE) {
			meta_clear_bake(m);
		}
		else {
			// A type change refills the encoding only, Object and Tile are left as they are for every type
			meta_set_i32(m, META_BAKE_TYPE, new_btp);
			meta_set_i32(m, META_ENCODING, asset_bake_default_encoding(new_btp));
		}
		return;
	}
	if (btp == BTP_NONE) {
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
	bool changed = ui_asset_props_combo(&new_enc, ar, ids, tr("Encoding"), &asset_encoding_label, 1);

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
	i32 new_obj = obj;
	changed     = ui_asset_props_combo(&new_obj, ar, ids, tr("Object"), &asset_unknown_label, 2) || changed;

	// Tile, Any first, then the project tiles (1001 without a tiles list)
	ar  = any_array_create(0);
	ids = i32_array_create(0);
	any_array_push(ar, tr("Any"));
	i32_array_push(ids, -1);
	for (i32 i = 0; i < util_udim_count(); ++i) {
		any_array_push(ar, string_tmp("%d", util_udim_tile(i)));
		i32_array_push(ids, util_udim_tile(i));
	}
	i32 new_tile = tile;
	changed      = ui_asset_props_combo(&new_tile, ar, ids, tr("Tile"), &asset_unknown_label, 3) || changed;

	if (changed) {
		meta_set_bake(meta_get(MC_TEXTURE, ui_asset_props_index), btp, new_enc, new_obj, new_tile);
	}
}

static void ui_asset_props_box_draw() {
	ui_asset_props_draw();
	ui_end_element();
	if (ui_icon_button(tr("OK"), ICON_CHECK, UI_ALIGN_CENTER) || g_ui->is_return_down) {
		ui_box_hide();
	}
}

void ui_asset_props_show(i32 texture_index) {
	ui_asset_props_index = texture_index;
	ui_box_show_custom(&ui_asset_props_box_draw, 400, 280, NULL, true, tr("Bake Properties"));
}
