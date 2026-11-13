
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
// project_t atlas_objects / atlas_names of upstream "23" are only read to convert atlas group object masks (Export > Atlases was removed).
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

// Skips one encoded armpack value (see traverse in iron_armpack.c), returns the offset after it, len + 1 when it does not fit
static u64 project_format_skip(u8 *p, u64 i, u64 len) {
	if (i >= len) {
		return len + 1;
	}
	u8  flag = p[i++];
	u32 n    = 0;
	switch (flag) {
	case 0xc0:
	case 0xc2:
	case 0xc3:
		return i;
	case 0xca:
	case 0xd2:
		return i + 4 > len ? len + 1 : i + 4;
	case 0xdb:
		if (i + 4 > len) {
			return len + 1;
		}
		memcpy(&n, p + i, 4);
		return i + 4 + n > len ? len + 1 : i + 4 + n;
	case 0xdd: {
		if (i + 4 > len) {
			return len + 1;
		}
		memcpy(&n, p + i, 4);
		i += 4;
		if (n == 0) {
			return i;
		}
		if (i >= len) {
			return len + 1;
		}
		u8  flag2 = p[i];
		u64 size  = flag2 == 0xca || flag2 == 0xd2 ? 4 : flag2 == 0xd1 ? 2 : flag2 == 0xc4 ? 1 : 0;
		if (size > 0) {
			return i + 1 + size * n > len ? len + 1 : i + 1 + size * n;
		}
		for (u32 j = 0; j < n && i <= len; ++j) { // Dynamic, every element has its own flag
			i = project_format_skip(p, i, len);
		}
		return i;
	}
	case 0xdf: {
		if (i + 4 > len) {
			return len + 1;
		}
		memcpy(&n, p + i, 4);
		i += 4;
		for (u32 j = 0; j < n && i <= len; ++j) {
			if (i + 5 > len || p[i] != 0xdb) {
				return len + 1;
			}
			u32 key_len;
			memcpy(&key_len, p + i + 1, 4);
			i = project_format_skip(p, i + 5 + key_len > len ? len : i + 5 + key_len, len);
		}
		return i;
	}
	}
	return len + 1;
}

// Whether the top level map of an encoded project has the key
static bool project_format_has_key(buffer_t *b, char *key) {
	u8 *p   = b->buffer;
	u64 len = b->length;
	u32 n   = string_length(key);
	if (len <= 5 || p[0] != 0xdf) {
		return false;
	}
	i32 count;
	memcpy(&count, p + 1, 4);
	u64 i = 5;
	for (i32 k = 0; k < count && i <= len; ++k) {
		if (i + 5 > len || p[i] != 0xdb) {
			return false;
		}
		u32 key_len;
		memcpy(&key_len, p + i + 1, 4);
		if (i + 5 + key_len > len) {
			return false;
		}
		if (key_len == n && memcmp(p + i + 5, key, n) == 0) {
			return true;
		}
		i = project_format_skip(p, i + 5 + key_len, len);
	}
	return false;
}

// Decodes a project file. A "99" file of an earlier build of the fork has no metas key and another layout, armpack would
// decode it with shifted fields, so it is refused: NULL, the caller reports an unsupported version. Older versions decode as before.
project_t *project_format_decode(buffer_t *b) {
	if (!import_arm_is_old(b) && !project_format_has_key(b, "metas")) {
		return NULL;
	}
	return armpack_decode(b);
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

// Layer i of a converted project: its object mask becomes a metas entry with the object set
static void project_format_convert_object_mask(any_map_t *old, project_t *project, i32 i, i32 object_count) {
	layer_data_t *ld       = project->layer_datas->buffer[i];
	bool          is_layer = ld->texpaint != NULL && ld->texpaint_nor != NULL;
	bool          is_child = false; // Filter, a paint layer inside a paint layer
	if (is_layer && ld->parent >= 0) {
		any_array_t *lds = any_map_get(old, "layer_datas");
		if (ld->parent < lds->length) {
			any_map_t *parent = lds->buffer[ld->parent];
			is_child          = any_map_get(parent, "texpaint") != NULL && any_map_get(parent, "texpaint_nor") != NULL;
		}
	}
	i32 mask = ld->object_mask;
	if (!is_layer || is_child || mask <= 0) {
		return;
	}
	i32_array_t *ids = NULL;
	if (mask <= object_count) {
		ids = i32_array_create(0);
		i32_array_push(ids, mask);
	}
	else {
		ids = project_format_atlas_ids(old, mask - object_count - 1, object_count);
	}
	if (ids == NULL) { // Unresolved groups stay on every object, as upstream fell back to every paint object
		return;
	}
	meta_t *m   = meta_create();
	m->category = MC_LAYER;
	m->index    = i;
	meta_set_bool(m, META_ALL_OBJECTS, false);
	meta_set_i32_array(m, META_OBJECTS, ids->buffer, ids->length);
	any_array_push(project->metas, m);
	array_delete(ids);
}

// Upstream "23": layer object masks and atlas groups become object id sets in metas, every layer covers all tiles.
// Name based udim tile objects (X.1001, ...) keep their objects, tile migration is phase B.
project_t *import_arm_from_version_23(any_map_t *old) {
	project_t *project = import_arm_from_map_to_arm(old);
	project_format_set_defaults(project);
	any_array_t *lds          = any_map_get(old, "layer_datas");
	i32          object_count = project->mesh_datas != NULL ? project->mesh_datas->length : 0;
	for (i32 i = 0; lds != NULL && project->layer_datas != NULL && i < project->layer_datas->length && i < lds->length; ++i) {
		project_format_convert_object_mask(old, project, i, object_count);
	}
	return project;
}
