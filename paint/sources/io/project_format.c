
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
// project_t atlas_objects / atlas_names of upstream "23" are only read to convert atlas group object masks (Export > Atlases was removed).
// History steps are not saved.
// Udim: udim_tiles is the project tile list, mesh_tiles[i] the tile index per vertex of paint object i (empty = none, linked
// duplicates store none), and the "tex" vertex array of mesh_datas always holds tile-local uvs, never the runtime atlas uvs.

static i16_array_t  project_format_no_tiles    = {0};  // Mesh with no tile data, every vertex is in 1001
static any_array_t *project_format_saved_tiles = NULL; // Copies of the tile arrays project_t.mesh_tiles points to since the last save

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

	// Copies, so project_t never points into runtime arrays that are freed with their mesh
	for (i32 i = 0; project_format_saved_tiles != NULL && i < project_format_saved_tiles->length; ++i) {
		array_delete(project_format_saved_tiles->buffer[i]);
	}
	array_delete(project_format_saved_tiles);
	project_format_saved_tiles = any_array_create(0);
	for (i32 i = 0; i < objects->length; ++i) {
		mesh_data_t *md = objects->buffer[i]->data;
		if (util_mesh_data_owner(md) == i && util_udim_has_tiles(md)) { // Linked duplicates store no tiles
			i16_array_t *tiles = i16_array_create_from_array(md->_->udim_tile);
			any_array_push(project_format_saved_tiles, tiles);
			raw->mesh_tiles->buffer[i] = tiles;
		}
	}
}

// util_encode_mesh_datas: the values written for a vertex array, tile-local uvs for "tex"
i16_array_t *project_format_mesh_values(mesh_data_t *md, vertex_array_t *va) {
	if (string_equals(va->attrib, "tex") && util_udim_has_tiles(md)) {
		return md->_->udim_tex;
	}
	return va->values;
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

// After the paint objects of a loaded project exist, one per mesh_datas entry; the tile list stays {1001} for now
void project_format_load_mesh_tiles(project_t *raw) {
	mesh_object_t_array_t *objects = raw->_->paint_objects;
	for (i32 i = 0; raw->mesh_tiles != NULL && i < raw->mesh_tiles->length && i < objects->length; ++i) {
		i16_array_t *tiles = raw->mesh_tiles->buffer[i];
		if (tiles != NULL && tiles->length > 0) {
			util_udim_set(objects->buffer[i]->data, i16_array_create_from_array(tiles)); // Ignored when the length does not match
		}
	}
	raw->mesh_tiles = NULL; // Decoded arrays live in the file blob, the next save writes new ones
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
	raw->udim_tiles->length = 0; // Converted projects have a single tile, not the one of the project that is open now
	i32_array_push(raw->udim_tiles, 1001);
}

// Load, before the layer textures are created. The runtime list is the saved one ({1001} when missing),
// the project resolution becomes the per tile one: saved layers have the atlas size, which is the same for a single tile.
void project_format_load_tiles(project_t *raw, i32 layer_res) {
	util_udim_set_tiles(raw->udim_tiles);
	i32 res = layer_res / util_udim_stride();
	if (res * util_udim_stride() != layer_res) {
		console_info(string("Layer size %d is not a multiple of the udim atlas stride %d, using %d per tile", layer_res, util_udim_stride(), res));
	}
	base_res_x = (f32)res;
	base_res_y = (f32)res;
	base_res   = config_get_texture_res_pos(res);
}

// Upstream "23" object masks (layer_data_t.object_mask of the old format):
// 0 every object, 1..N paint object mask - 1 (N paint objects in file order, their ids become 1..N),
// N + 1 + k the k-th used atlas group of Export > Atlases (see project_format_atlas_ids).
// Upstream only applied a mask on paint layers, masks and filters used their parent layer's and groups ignored theirs,
// so those convert to every object (masks and filters still follow their parent, groups restrict nothing as before).

// Ids of the objects in the k-th used atlas group, NULL when it can not be resolved.
// Mirrors upstream project_get_used_atlases / project_get_atlas_objects: used atlases are the distinct atlas_objects values
// in order of first appearance, only counted when more than one is used, and an atlas is found again by its name.
static i32_array_t *project_format_atlas_ids(any_map_t *old, i32 k, i32 object_count) {
	i32_array_t    *atlas_objects = any_map_get(old, "atlas_objects");
	string_array_t *atlas_names   = any_map_get(old, "atlas_names");
	if (atlas_objects == NULL || atlas_names == NULL || atlas_objects->length == 0 || atlas_names->length == 0 || k < 0) {
		return NULL;
	}
	i32_array_t *used = i32_array_create(0);
	for (i32 i = 0; i < atlas_objects->length; ++i) {
		if (i32_array_index_of(used, atlas_objects->buffer[i]) == -1) {
			i32_array_push(used, atlas_objects->buffer[i]);
		}
	}
	i32_array_t *ids = NULL;
	if (used->length > 1 && k < used->length && used->buffer[k] >= 0 && used->buffer[k] < atlas_names->length) {
		i32 atlas_i = string_array_index_of(atlas_names, atlas_names->buffer[used->buffer[k]]);
		ids         = i32_array_create(0);
		for (i32 i = 0; i < object_count && i < atlas_objects->length; ++i) {
			if (atlas_objects->buffer[i] == atlas_i) {
				i32_array_push(ids, i + 1);
			}
		}
		if (ids->length == 0) {
			array_delete(ids);
			ids = NULL;
		}
	}
	array_delete(used);
	return ids;
}

static void project_format_convert_object_mask(any_map_t *old, any_map_t *old_ld, layer_data_t *ld, i32 object_count) {
	bool is_layer = ld->texpaint != NULL && ld->texpaint_nor != NULL;
	bool is_child = false; // Filter, a paint layer inside a paint layer
	if (is_layer && ld->parent >= 0) {
		any_array_t *lds = any_map_get(old, "layer_datas");
		if (ld->parent < lds->length) {
			any_map_t *parent = lds->buffer[ld->parent];
			is_child          = any_map_get(parent, "texpaint") != NULL && any_map_get(parent, "texpaint_nor") != NULL;
		}
	}
	i32 mask = armpack_map_get_i32(old_ld, "object_mask");
	if (!is_layer || is_child || mask <= 0) {
		return;
	}
	if (mask <= object_count) {
		i32_array_push(ld->objects, mask);
		ld->all_objects = false;
		return;
	}
	i32_array_t *ids = project_format_atlas_ids(old, mask - object_count - 1, object_count);
	if (ids != NULL) { // Unresolved groups stay on every object, as upstream fell back to every paint object
		for (i32 i = 0; i < ids->length; ++i) {
			i32_array_push(ld->objects, ids->buffer[i]);
		}
		ld->all_objects = false;
		array_delete(ids);
	}
}

// Upstream "23": layer object masks and atlas groups become object id sets, every layer covers all tiles.
// Name based udim tile objects (X.1001, ...) keep their objects, tile migration is phase B.
project_t *import_arm_from_version_23(any_map_t *old) {
	project_t *project = import_arm_from_map_to_arm(old);
	project_format_set_defaults(project);
	any_array_t *lds          = any_map_get(old, "layer_datas");
	i32          object_count = project->mesh_datas != NULL ? project->mesh_datas->length : 0;
	for (i32 i = 0; lds != NULL && project->layer_datas != NULL && i < project->layer_datas->length && i < lds->length; ++i) {
		project_format_convert_object_mask(old, lds->buffer[i], project->layer_datas->buffer[i], object_count);
	}
	return project;
}
