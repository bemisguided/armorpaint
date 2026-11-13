
#include "../global.h"

// Layer coverage: a layer applies to (object in objects or all objects) and (tile in tiles or all tiles).
// Objects are persistent paint ids (mesh_object_t.paint_id), tiles are udim ids (1001..).
// The sets are keys of the layer's metadata (slot_layer_t.meta, keys in enums.h): all objects and all tiles are bools that are
// true when absent, the lists are i32 arrays that are empty when absent. A list is kept while all is on, so the ticks come back.
// A NULL meta covers everything, so new and zero initialized temporary layers cover everything.
// A layer's effective coverage is its own set and every ancestor group's set (masks and filters use their parent's).

// Project tile list, only {1001} until tile extraction lands
i32 util_udim_count() {
	return 1;
}

i32 util_udim_tile(i32 slot) {
	return 1001;
}

mesh_object_t *project_object_by_id(i32 paint_id) {
	for (i32 i = 0; paint_id > 0 && i < g_project->_->paint_objects->length; ++i) {
		mesh_object_t *o = g_project->_->paint_objects->buffer[i];
		if (o->paint_id == paint_id) {
			return o;
		}
	}
	return NULL;
}

slot_layer_t *slot_layer_coverage_owner(slot_layer_t *l) {
	return (slot_layer_is_mask(l) || slot_layer_is_filter(l)) ? l->parent : l;
}

bool slot_layer_all_objects(slot_layer_t *l) {
	return meta_get_bool(l->meta, META_ALL_OBJECTS, true);
}

static i32_array_t _coverage_none = {0}; // List of a kind with no ids

// Stored paint ids, never NULL (empty when none), owned by the layer's meta: read only, change them with the setters below
i32_array_t *slot_layer_objects(slot_layer_t *l) {
	i32_array_t *ids = meta_get_i32_array(l->meta, META_OBJECTS);
	return ids != NULL ? ids : &_coverage_none;
}

bool slot_layer_all_tiles(slot_layer_t *l) {
	return meta_get_bool(l->meta, META_ALL_TILES, true);
}

// Stored udim tile ids, never NULL (empty when none), owned by the layer's meta: read only, change them with the setters below
i32_array_t *slot_layer_tiles(slot_layer_t *l) {
	i32_array_t *ids = meta_get_i32_array(l->meta, META_TILES);
	return ids != NULL ? ids : &_coverage_none;
}

// ids may be NULL (an absent key read straight from a meta)
static bool _coverage_has(i32_array_t *ids, i32 id) {
	return ids != NULL && i32_array_index_of(ids, id) >= 0;
}

static bool _layer_level_objects(slot_layer_t *p) {
	return !slot_layer_all_objects(p);
}

static bool _layer_level_tiles(slot_layer_t *p) {
	return !slot_layer_all_tiles(p);
}

bool layer_covers_object_id(slot_layer_t *l, i32 paint_id) {
	for (slot_layer_t *p = slot_layer_coverage_owner(l); p != NULL; p = p->parent) {
		if (_layer_level_objects(p) && !_coverage_has(slot_layer_objects(p), paint_id)) {
			return false;
		}
	}
	return true;
}

bool layer_covers_object(slot_layer_t *l, mesh_object_t *o) {
	return layer_covers_object_id(l, o->paint_id);
}

bool layer_covers_tile(slot_layer_t *l, i32 tile) {
	for (slot_layer_t *p = slot_layer_coverage_owner(l); p != NULL; p = p->parent) {
		if (_layer_level_tiles(p) && !_coverage_has(slot_layer_tiles(p), tile)) {
			return false;
		}
	}
	return true;
}

// Whether the groups above the layer (not the layer itself) leave the object or tile in its coverage
bool layer_ancestors_cover_object_id(slot_layer_t *l, i32 paint_id) {
	for (slot_layer_t *p = slot_layer_coverage_owner(l)->parent; p != NULL; p = p->parent) {
		if (_layer_level_objects(p) && !_coverage_has(slot_layer_objects(p), paint_id)) {
			return false;
		}
	}
	return true;
}

bool layer_ancestors_cover_tile(slot_layer_t *l, i32 tile) {
	for (slot_layer_t *p = slot_layer_coverage_owner(l)->parent; p != NULL; p = p->parent) {
		if (_layer_level_tiles(p) && !_coverage_has(slot_layer_tiles(p), tile)) {
			return false;
		}
	}
	return true;
}

bool layer_objects_restricted(slot_layer_t *l) {
	for (slot_layer_t *p = slot_layer_coverage_owner(l); p != NULL; p = p->parent) {
		if (_layer_level_objects(p)) {
			return true;
		}
	}
	return false;
}

static bool layer_tiles_restricted(slot_layer_t *l) {
	for (slot_layer_t *p = slot_layer_coverage_owner(l); p != NULL; p = p->parent) {
		if (_layer_level_tiles(p)) {
			return true;
		}
	}
	return false;
}

bool layer_is_restricted(slot_layer_t *l) {
	return layer_objects_restricted(l) || layer_tiles_restricted(l);
}

// Live paint objects in the effective coverage, all of them when unrestricted, caller frees
mesh_object_t_array_t *layer_coverage_objects(slot_layer_t *l) {
	mesh_object_t_array_t *ar = any_array_create_from_raw((void *[]){}, 0);
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		mesh_object_t *o = g_project->_->paint_objects->buffer[i];
		if (layer_covers_object(l, o)) {
			any_array_push(ar, o);
		}
	}
	return ar;
}

// Ids every restricting level lists (the stored coverage, deleted ids included), NULL when objects are unrestricted, caller frees
static i32_array_t *_layer_stored_objects(slot_layer_t *l) {
	for (slot_layer_t *p = slot_layer_coverage_owner(l); p != NULL; p = p->parent) {
		if (!_layer_level_objects(p)) {
			continue;
		}
		i32_array_t *stored = slot_layer_objects(p);
		i32_array_t *ids    = i32_array_create(0);
		for (i32 i = 0; i < stored->length; ++i) {
			i32 id = stored->buffer[i];
			if (i32_array_index_of(ids, id) < 0 && layer_covers_object_id(l, id)) {
				i32_array_push(ids, id);
			}
		}
		return ids;
	}
	return NULL;
}

// The only id of the stored coverage of an object restricted layer, 0 otherwise.
// Deleted ids count, so deleting or adding objects never changes the result.
i32 layer_coverage_single_id(slot_layer_t *l) {
	i32_array_t *ids = _layer_stored_objects(l);
	i32          id  = ids != NULL && ids->length == 1 ? ids->buffer[0] : 0;
	array_delete(ids);
	return id;
}

// Why a layer has nothing to apply to, LAYER_COVERAGE_SOME when it covers something
layer_coverage_empty_t layer_coverage_empty_reason(slot_layer_t *l) {
	if (layer_objects_restricted(l)) {
		bool any = false;
		for (i32 i = 0; i < g_project->_->paint_objects->length && !any; ++i) {
			any = layer_covers_object(l, g_project->_->paint_objects->buffer[i]);
		}
		if (!any) {
			i32_array_t *ids    = _layer_stored_objects(l);
			bool         stored = ids != NULL && ids->length > 0;
			array_delete(ids);
			return stored ? LAYER_COVERAGE_OBJECTS_DELETED : LAYER_COVERAGE_NO_OBJECTS;
		}
	}
	if (layer_tiles_restricted(l)) {
		bool any = false;
		for (i32 i = 0; i < util_udim_count() && !any; ++i) {
			any = layer_covers_tile(l, util_udim_tile(i));
		}
		if (!any) {
			return LAYER_COVERAGE_NO_TILES;
		}
	}
	return LAYER_COVERAGE_SOME;
}

// Deleted objects, empty selections or removed tiles leave nothing to apply the layer to
bool layer_covers_nothing(slot_layer_t *l) {
	return layer_coverage_empty_reason(l) != LAYER_COVERAGE_SOME;
}

static int _util_udim_compare_i32(const void *a, const void *b) {
	return *(i32 *)a - *(i32 *)b;
}

// Opens "if (...) {" around the layer for objects outside its coverage, false when nothing is written
bool layer_write_coverage_test(node_shader_t *kong, slot_layer_t *l) {
	// Tiles: every project tile is 1001 for now, so a tile restriction either covers it or covers nothing
	if (!layer_objects_restricted(l)) {
		return false;
	}

	// Ranges of covered ids in the sorted live ids, gaps of deleted ids are skipped over
	i32_array_t *live = i32_array_create(0);
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		i32 pid = g_project->_->paint_objects->buffer[i]->paint_id;
		if (pid > 0 && i32_array_index_of(live, pid) < 0) {
			i32_array_push(live, pid);
		}
	}
	i32_array_sort(live, _util_udim_compare_i32);

	node_shader_add_constant(kong, "int object_pid", "_object_pid");
	node_shader_write_frag(kong, "if (");
	bool first = true;
	for (i32 i = 0; i < live->length; ++i) {
		if (!layer_covers_object_id(l, live->buffer[i])) {
			continue;
		}
		i32 a = live->buffer[i];
		while (i + 1 < live->length && layer_covers_object_id(l, live->buffer[i + 1])) {
			++i;
		}
		i32 b = live->buffer[i];
		if (!first) {
			node_shader_write_frag(kong, " || ");
		}
		first = false;
		if (a == b) {
			node_shader_write_frag(kong, string_tmp("constants.object_pid == %s", i32_to_string(a)));
		}
		else {
			node_shader_write_frag(kong, string_tmp("(constants.object_pid >= %s && ", i32_to_string(a)));
			node_shader_write_frag(kong, string_tmp("constants.object_pid <= %s)", i32_to_string(b)));
		}
	}
	if (first) {
		node_shader_write_frag(kong, "false");
	}
	node_shader_write_frag(kong, ") {");
	array_delete(live);
	return true;
}

// Stores one kind of coverage: all (absent when on) and the list (absent when empty), ids may point into the current list.
// A meta left without keys is freed, NULL covers everything.
static void _coverage_store(slot_layer_t *l, i32 all_key, i32 list_key, bool all, i32 *ids, i32 count) {
	if (l->meta == NULL) {
		l->meta = meta_create();
	}
	if (all) {
		meta_remove(l->meta, all_key);
	}
	else {
		meta_set_bool(l->meta, all_key, false);
	}
	if (ids != NULL && count > 0) {
		meta_set_i32_array(l->meta, list_key, ids, count);
	}
	else {
		meta_remove(l->meta, list_key);
	}
	if (meta_is_empty(l->meta)) {
		meta_free(l->meta);
		l->meta = NULL;
	}
}

static void _coverage_union(slot_layer_t *dst, slot_layer_t *src, i32 all_key, i32 list_key) {
	bool         all = meta_get_bool(dst->meta, all_key, true) || meta_get_bool(src->meta, all_key, true);
	i32_array_t *cur = meta_get_i32_array(dst->meta, list_key);
	i32_array_t *add = meta_get_i32_array(src->meta, list_key);
	i32_array_t *ids = cur != NULL ? i32_array_create_from_array(cur) : i32_array_create(0);
	for (i32 i = 0; add != NULL && i < add->length; ++i) {
		if (i32_array_index_of(ids, add->buffer[i]) < 0) {
			i32_array_push(ids, add->buffer[i]);
		}
	}
	_coverage_store(dst, all_key, list_key, all, ids->buffer, ids->length);
	array_delete(ids);
}

static void _coverage_intersect(slot_layer_t *dst, slot_layer_t *group, i32 all_key, i32 list_key) {
	if (meta_get_bool(group->meta, all_key, true)) {
		return;
	}
	i32_array_t *keep = meta_get_i32_array(group->meta, list_key);
	i32_array_t *cur  = meta_get_i32_array(dst->meta, list_key);
	i32_array_t *ids  = i32_array_create(0);
	if (meta_get_bool(dst->meta, all_key, true)) {
		for (i32 i = 0; keep != NULL && i < keep->length; ++i) {
			i32_array_push(ids, keep->buffer[i]);
		}
	}
	else {
		for (i32 i = 0; cur != NULL && i < cur->length; ++i) {
			if (_coverage_has(keep, cur->buffer[i])) {
				i32_array_push(ids, cur->buffer[i]);
			}
		}
	}
	_coverage_store(dst, all_key, list_key, false, ids->buffer, ids->length);
	array_delete(ids);
}

// Merged pixels keep showing wherever either layer did
void slot_layer_coverage_union(slot_layer_t *dst, slot_layer_t *src) {
	_coverage_union(dst, src, META_ALL_OBJECTS, META_OBJECTS);
	_coverage_union(dst, src, META_ALL_TILES, META_TILES);
}

// A group merged into its child restricts the result like it restricted the child
void slot_layer_coverage_intersect(slot_layer_t *dst, slot_layer_t *group) {
	_coverage_intersect(dst, group, META_ALL_OBJECTS, META_OBJECTS);
	_coverage_intersect(dst, group, META_ALL_TILES, META_TILES);
}

// The list is replaced when ids are given or all is off, otherwise only all changes and the list is kept
void slot_layer_set_objects(slot_layer_t *l, i32 *ids, i32 count, bool all) {
	if (ids == NULL && all) {
		i32_array_t *cur = slot_layer_objects(l);
		_coverage_store(l, META_ALL_OBJECTS, META_OBJECTS, true, cur->buffer, cur->length);
		return;
	}
	_coverage_store(l, META_ALL_OBJECTS, META_OBJECTS, all, ids, ids != NULL ? count : 0);
}

void slot_layer_set_tiles(slot_layer_t *l, i32 *ids, i32 count, bool all) {
	if (ids == NULL && all) {
		i32_array_t *cur = slot_layer_tiles(l);
		_coverage_store(l, META_ALL_TILES, META_TILES, true, cur->buffer, cur->length);
		return;
	}
	_coverage_store(l, META_ALL_TILES, META_TILES, all, ids, ids != NULL ? count : 0);
}

// Sets all of objects (tiles false) or tiles, the stored list stays
void slot_layer_coverage_set_all(slot_layer_t *l, bool tiles, bool all) {
	i32_array_t *cur = tiles ? slot_layer_tiles(l) : slot_layer_objects(l);
	_coverage_store(l, tiles ? META_ALL_TILES : META_ALL_OBJECTS, tiles ? META_TILES : META_OBJECTS, all, cur->buffer, cur->length);
}

// Adds or removes one id of the stored objects (tiles false) or tiles list, all stays as it is
void slot_layer_coverage_toggle(slot_layer_t *l, bool tiles, i32 id) {
	i32_array_t *ids = i32_array_create_from_array(tiles ? slot_layer_tiles(l) : slot_layer_objects(l));
	i32          pos = i32_array_index_of(ids, id);
	if (pos >= 0) {
		i32_array_splice(ids, pos, 1);
	}
	else {
		i32_array_push(ids, id);
	}
	bool all = tiles ? slot_layer_all_tiles(l) : slot_layer_all_objects(l);
	_coverage_store(l, tiles ? META_ALL_TILES : META_ALL_OBJECTS, tiles ? META_TILES : META_OBJECTS, all, ids->buffer, ids->length);
	array_delete(ids);
}

// Whether slot_layer_set_objects / slot_layer_set_tiles with these arguments would change anything, the list compares as a set
static bool _coverage_differs(bool cur_all, i32_array_t *cur, i32 *ids, i32 count, bool all) {
	if (cur_all != all) {
		return true;
	}
	if (ids == NULL && all) {
		return false;
	}
	if (ids == NULL) {
		count = 0;
	}
	for (i32 i = 0; i < count; ++i) {
		if (i32_array_index_of(cur, ids[i]) < 0) {
			return true;
		}
	}
	for (i32 i = 0; i < cur->length; ++i) {
		bool found = false;
		for (i32 j = 0; j < count && !found; ++j) {
			found = ids[j] == cur->buffer[i];
		}
		if (!found) {
			return true;
		}
	}
	return false;
}

bool slot_layer_objects_differ(slot_layer_t *l, i32 *ids, i32 count, bool all) {
	return _coverage_differs(slot_layer_all_objects(l), slot_layer_objects(l), ids, count, all);
}

bool slot_layer_tiles_differ(slot_layer_t *l, i32 *ids, i32 count, bool all) {
	return _coverage_differs(slot_layer_all_tiles(l), slot_layer_tiles(l), ids, count, all);
}

// New layers made while the list is filtered only cover the filtered object
void slot_layer_coverage_from_filter(slot_layer_t *l) {
	if (g_context->layer_filter_object > 0) {
		slot_layer_set_objects(l, &g_context->layer_filter_object, 1, false);
	}
}

// Geometry of from now also lives in to (join, split), layers listing from also list to.
// Tile-local layers of name based udim projects keep their single id, a second id would make them read their pixels as atlas.
void layers_coverage_add_alias(i32 from, i32 to) {
	for (i32 i = 0; g_project->_->layers != NULL && i < g_project->_->layers->length; ++i) {
		slot_layer_t *l = g_project->_->layers->buffer[i];
		if (layers_udim_tile_slot(l) >= 0) {
			continue;
		}
		i32_array_t *ids = slot_layer_objects(l);
		if (_coverage_has(ids, from) && !_coverage_has(ids, to)) {
			i32_array_t *add = i32_array_create_from_array(ids);
			i32_array_push(add, to);
			_coverage_store(l, META_ALL_OBJECTS, META_OBJECTS, slot_layer_all_objects(l), add->buffer, add->length);
			array_delete(add);
		}
	}
}
