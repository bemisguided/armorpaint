
#include "../global.h"

// Save and load code of this fork's project format, kept in one place so it is easy to follow upstream format changes.
//
// Version: manifest_version_project is "99" (defined in globals.h, it has to exist before every unity included file).
// Upstream compares versions by exact string and has converters for 0..22 only, so "99" files cleanly fail to load there.
// Upstream "23" files load through import_arm_from_version_23 below, older ones chain into it from import_legacy.c.
// Projects decode by field position (armpack), so these types.h fields must stay in the order written here:
// - layer_data_t: all_objects, objects, all_tiles, tiles (after fill_material, before blending)
// - slot_layer_t: all_objects, objects, all_tiles, tiles (runtime copy of the above)
// - project_t:    next_object_id, mesh_ids, udim_tiles, mesh_tiles (after mesh_groups, before _)
// project_t atlas_objects / atlas_names of upstream "23" are skipped on load (Export > Atlases was removed).
// History steps are not saved.

static i16_array_t project_format_no_tiles = {0}; // Mesh with no tile data, every vertex is in 1001

// util_encode_project, layer_datas entry, after fill_material
void project_format_encode_layer(layer_data_t *ld) {
	armpack_encode_string("all_objects");
	armpack_encode_bool(ld->all_objects);
	armpack_encode_string("objects");
	armpack_encode_array_i32(ld->objects);
	armpack_encode_string("all_tiles");
	armpack_encode_bool(ld->all_tiles);
	armpack_encode_string("tiles");
	armpack_encode_array_i32(ld->tiles);
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
}

i32 project_format_encode_size(project_t *raw) {
	i32 size = 0;
	for (i32 i = 0; raw->mesh_tiles != NULL && i < raw->mesh_tiles->length; ++i) {
		i16_array_t *tiles = raw->mesh_tiles->buffer[i];
		size += tiles != NULL ? tiles->length * 2 : 0;
	}
	return size;
}

// Save, the arrays are only read by the encoder
void project_format_layer_to_data(layer_data_t *ld, slot_layer_t *l) {
	ld->all_objects = l->all_objects;
	ld->objects     = l->objects;
	ld->all_tiles   = l->all_tiles;
	ld->tiles       = l->tiles;
}

// Load, decoded arrays live in the file blob and are copied
void project_format_layer_from_data(slot_layer_t *l, layer_data_t *ld) {
	array_delete(l->objects);
	array_delete(l->tiles);
	l->all_objects = ld->all_objects;
	l->objects     = ld->objects != NULL ? i32_array_create_from_array(ld->objects) : i32_array_create(0);
	l->all_tiles   = ld->all_tiles;
	l->tiles       = ld->tiles != NULL ? i32_array_create_from_array(ld->tiles) : i32_array_create(0);
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

// Before util_encode_project, mesh_datas are written one per paint object in the same order
void project_format_save_prep(project_t *raw) {
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

// Fields the fork adds, for projects converted from an older format
void project_format_set_defaults(project_t *raw) {
	for (i32 i = 0; raw->layer_datas != NULL && i < raw->layer_datas->length; ++i) {
		layer_data_t *ld = raw->layer_datas->buffer[i];
		ld->all_objects  = true;
		ld->objects      = i32_array_create(0);
		ld->all_tiles    = true;
		ld->tiles        = i32_array_create(0);
	}
	i32 mesh_count = raw->mesh_datas != NULL ? raw->mesh_datas->length : 0;
	raw->mesh_ids  = i32_array_create(0);
	for (i32 i = 0; i < mesh_count; ++i) {
		i32_array_push(raw->mesh_ids, i + 1);
	}
	raw->next_object_id = mesh_count;
	project_format_reset_tiles(raw, mesh_count);
}

// Upstream "23": no migration, every layer covers all objects and tiles.
// Object masks, atlas groups and name based udim tiles of the old file are dropped.
project_t *import_arm_from_version_23(any_map_t *old) {
	project_t *project = import_arm_from_map_to_arm(old);
	project_format_set_defaults(project);
	return project;
}
