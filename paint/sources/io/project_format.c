
#include "../global.h"

// Save and load code of this fork's project format, kept in one place so it is easy to follow upstream format changes.
//
// Version: manifest_version_project is "99" (defined in globals.h, it has to exist before every unity included file).
// Upstream compares versions by exact string and has converters for 0..22 only, so "99" files cleanly fail to load there.
// Upstream "23" files load through import_arm_from_version_23 below, older ones chain into it from import_legacy.c.
// Projects decode by field position (armpack), so these types.h fields must stay in the order written here:
// - project_t: next_object_id, mesh_ids, udim_tiles, mesh_tiles, metas (after mesh_groups, before _)
// - meta_t:    category, index, bool_keys, bool_values, i32_keys, i32_values, f32_keys, f32_values, i32_array_keys,
//              i32_array_values (the entry maps of metas, every field written in this order)
// layer_data_t has the upstream layout, its object_mask is written as 0 (layer coverage is in metas).
// project_t atlas_objects / atlas_names of upstream "23" are skipped on load (Export > Atlases was removed).
// History steps are not saved.
// Metadata (meta_t, see util_meta.c): one list of sparse entries, one per item that has metadata. category says which list
// the item is in, index its position there when the project was saved: layers (MC_LAYER) by their position in layer_datas,
// groups, masks and filters counted. Save rebuilds the list from the live items, load copies each entry onto its item and
// ignores entries of unknown categories or out of range indices. Unknown key ids are kept as they are.
// An empty list is written as an empty array. Entry maps always list every meta_t field in order (armpack sizes an array of
// maps from its first element), NULL arrays encode as null and i32 array values as an array of arrays.

static i16_array_t     project_format_no_tiles    = {0};  // Mesh with no tile data, every vertex is in 1001
static i32_array_t     project_format_no_values   = {0};  // i32 array value of a key that has none
static meta_t_array_t *project_format_saved_metas = NULL; // List project_t.metas points to since the last save

// One metas entry, fields in declaration order
static void project_format_encode_meta(meta_t *m) {
	armpack_encode_map(10);
	armpack_encode_string("category");
	armpack_encode_i32(m->category);
	armpack_encode_string("index");
	armpack_encode_i32(m->index);
	armpack_encode_string("bool_keys");
	armpack_encode_array_i32(m->bool_keys);
	armpack_encode_string("bool_values");
	armpack_encode_array_i32(m->bool_values);
	armpack_encode_string("i32_keys");
	armpack_encode_array_i32(m->i32_keys);
	armpack_encode_string("i32_values");
	armpack_encode_array_i32(m->i32_values);
	armpack_encode_string("f32_keys");
	armpack_encode_array_i32(m->f32_keys);
	armpack_encode_string("f32_values");
	armpack_encode_array_f32(m->f32_values);
	armpack_encode_string("i32_array_keys");
	armpack_encode_array_i32(m->i32_array_keys);
	armpack_encode_string("i32_array_values");
	if (m->i32_array_values != NULL) {
		// Elements of an array of arrays can not be null
		armpack_encode_array(m->i32_array_values->length);
		for (i32 i = 0; i < m->i32_array_values->length; ++i) {
			i32_array_t *values = m->i32_array_values->buffer[i];
			armpack_encode_array_i32(values != NULL ? values : &project_format_no_values);
		}
	}
	else {
		armpack_encode_null();
	}
}

// util_encode_project, after mesh_groups
void project_format_encode_project(project_t *raw) {
	armpack_encode_string("next_object_id");
	armpack_encode_i32(raw->next_object_id);
	armpack_encode_string("mesh_ids");
	armpack_encode_array_i32(raw->mesh_ids);
	armpack_encode_string("udim_tiles");
	armpack_encode_array_i32(raw->udim_tiles);
	armpack_encode_string("mesh_tiles");
	if (raw->mesh_tiles != NULL) {
		// Elements of an array of arrays can not be null, an empty array means no tile data
		armpack_encode_array(raw->mesh_tiles->length);
		for (i32 i = 0; i < raw->mesh_tiles->length; ++i) {
			i16_array_t *tiles = raw->mesh_tiles->buffer[i];
			armpack_encode_array_i16(tiles != NULL ? tiles : &project_format_no_tiles);
		}
	}
	else {
		armpack_encode_null();
	}
	armpack_encode_string("metas");
	armpack_encode_array(raw->metas != NULL ? raw->metas->length : 0);
	for (i32 i = 0; raw->metas != NULL && i < raw->metas->length; ++i) {
		project_format_encode_meta(raw->metas->buffer[i]);
	}
}

i32 project_format_encode_size(project_t *raw) {
	i32 size = 16;
	for (i32 i = 0; raw->metas != NULL && i < raw->metas->length; ++i) {
		meta_t *m = raw->metas->buffer[i];
		size += 256; // Map, keys and array headers
		size += 8 * ((m->bool_keys != NULL ? m->bool_keys->length : 0) + (m->i32_keys != NULL ? m->i32_keys->length : 0) +
		             (m->f32_keys != NULL ? m->f32_keys->length : 0));
		for (i32 j = 0; m->i32_array_values != NULL && j < m->i32_array_values->length; ++j) {
			i32_array_t *values = m->i32_array_values->buffer[j];
			size += 16 + 4 * (values != NULL ? values->length : 0);
		}
	}
	for (i32 i = 0; raw->mesh_tiles != NULL && i < raw->mesh_tiles->length; ++i) {
		i16_array_t *tiles = raw->mesh_tiles->buffer[i];
		size += tiles != NULL ? tiles->length * 2 : 0;
	}
	return size;
}

static void project_format_reset_tiles(project_t *raw, i32 mesh_count) {
	if (raw->udim_tiles == NULL) {
		raw->udim_tiles = i32_array_create(0);
	}
	raw->udim_tiles->length = 0;
	for (i32 i = 0; i < util_udim_count(); ++i) {
		i32_array_push(raw->udim_tiles, util_udim_tile(i));
	}
	if (raw->mesh_tiles == NULL) {
		raw->mesh_tiles = any_array_create(0);
	}
	raw->mesh_tiles->length = 0;
	for (i32 i = 0; i < mesh_count; ++i) {
		any_array_push(raw->mesh_tiles, &project_format_no_tiles);
	}
}

// The metas list of the live items, the entries are the items' own metas with category and index set (the encoder only reads)
static void project_format_build_metas(project_t *raw) {
	array_delete(project_format_saved_metas);
	project_format_saved_metas = any_array_create(0);
	for (i32 i = 0; i < raw->_->layers->length; ++i) {
		meta_t *m = raw->_->layers->buffer[i]->meta;
		if (!meta_is_empty(m)) {
			m->category = MC_LAYER;
			m->index    = i;
			any_array_push(project_format_saved_metas, m);
		}
	}
	raw->metas = project_format_saved_metas;
}

// Before util_encode_project, mesh_datas are written one per paint object in the same order, layer_datas in layer order
void project_format_save_prep(project_t *raw) {
	project_format_build_metas(raw);
	mesh_object_t_array_t *objects = raw->_->paint_objects;
	if (raw->mesh_ids == NULL) {
		raw->mesh_ids = i32_array_create(0);
	}
	raw->mesh_ids->length = 0;
	for (i32 i = 0; i < objects->length; ++i) {
		i32_array_push(raw->mesh_ids, objects->buffer[i]->paint_id);
	}
	raw->next_object_id = raw->_->next_object_id;
	project_format_reset_tiles(raw, objects->length);
}

static bool project_format_ids_valid(i32_array_t *ids, i32 count) {
	if (ids == NULL || ids->length != count) {
		return false;
	}
	for (i32 i = 0; i < count; ++i) {
		if (ids->buffer[i] <= 0) {
			return false;
		}
		for (i32 j = 0; j < i; ++j) {
			if (ids->buffer[j] == ids->buffer[i]) {
				return false;
			}
		}
	}
	return true;
}

// After the paint objects of a loaded project exist, one per mesh_datas entry
void project_format_load_object_ids(project_t *raw) {
	mesh_object_t_array_t *objects = raw->_->paint_objects;
	raw->_->next_object_id         = 0;
	if (!project_format_ids_valid(raw->mesh_ids, objects->length)) {
		for (i32 i = 0; i < objects->length; ++i) {
			project_assign_object_id(objects->buffer[i]); // 1..N
		}
		return;
	}
	i32 max_id = raw->next_object_id;
	for (i32 i = 0; i < objects->length; ++i) {
		objects->buffer[i]->paint_id = raw->mesh_ids->buffer[i];
		if (raw->mesh_ids->buffer[i] > max_id) {
			max_id = raw->mesh_ids->buffer[i];
		}
	}
	raw->_->next_object_id = max_id;
}

// After the layers of a loaded project exist (one per layer_datas entry), each gets a copy of its saved metadata
void project_format_load_layer_metas(project_t *raw) {
	slot_layer_t_array_t *layers = raw->_->layers;
	for (i32 i = 0; raw->metas != NULL && i < raw->metas->length; ++i) {
		meta_t *m = raw->metas->buffer[i];
		if (m->category == MC_LAYER && m->index >= 0 && m->index < layers->length && !meta_is_empty(m)) {
			slot_layer_t *l = layers->buffer[m->index];
			meta_free(l->meta);
			l->meta = meta_copy(m);
		}
	}
}

// Fields the fork adds, for projects converted from an older format
void project_format_set_defaults(project_t *raw) {
	i32 mesh_count = raw->mesh_datas != NULL ? raw->mesh_datas->length : 0;
	raw->mesh_ids  = i32_array_create(0);
	for (i32 i = 0; i < mesh_count; ++i) {
		i32_array_push(raw->mesh_ids, i + 1);
	}
	raw->next_object_id = mesh_count;
	project_format_reset_tiles(raw, mesh_count);
	raw->metas = any_array_create(0); // Every layer covers all objects and tiles
}

// Upstream "23": no migration, every layer covers all objects and tiles.
// Object masks, atlas groups and name based udim tiles of the old file are dropped.
project_t *import_arm_from_version_23(any_map_t *old) {
	project_t *project = import_arm_from_map_to_arm(old);
	project_format_set_defaults(project);
	return project;
}
