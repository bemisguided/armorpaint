
#include "../global.h"

// Layer coverage: a layer applies to (object in objects or all_objects) and (tile in tiles or all_tiles).
// Objects are persistent paint ids (mesh_object_t.paint_id), tiles are udim ids (1001..).
// A layer's effective coverage is its own set and every ancestor group's set (masks and filters use their parent's).
// A level only restricts when its list exists, so zero initialized temporary layers cover everything.

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

static bool _layer_level_objects(slot_layer_t *p) {
	return !p->all_objects && p->objects != NULL;
}

static bool _layer_level_tiles(slot_layer_t *p) {
	return !p->all_tiles && p->tiles != NULL;
}

bool layer_covers_object_id(slot_layer_t *l, i32 paint_id) {
	for (slot_layer_t *p = slot_layer_coverage_owner(l); p != NULL; p = p->parent) {
		if (_layer_level_objects(p) && i32_array_index_of(p->objects, paint_id) < 0) {
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
		if (_layer_level_tiles(p) && i32_array_index_of(p->tiles, tile) < 0) {
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

// The only live object of an object restricted layer, NULL otherwise
mesh_object_t *layer_coverage_single_object(slot_layer_t *l) {
	if (!layer_objects_restricted(l)) {
		return NULL;
	}
	mesh_object_t *found = NULL;
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		mesh_object_t *o = g_project->_->paint_objects->buffer[i];
		if (layer_covers_object(l, o)) {
			if (found != NULL) {
				return NULL;
			}
			found = o;
		}
	}
	return found;
}

// Deleted objects or removed tiles leave nothing to apply the layer to
bool layer_covers_nothing(slot_layer_t *l) {
	if (layer_tiles_restricted(l)) {
		bool any = false;
		for (i32 i = 0; i < util_udim_count() && !any; ++i) {
			any = layer_covers_tile(l, util_udim_tile(i));
		}
		if (!any) {
			return true;
		}
	}
	if (layer_objects_restricted(l)) {
		for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
			if (layer_covers_object(l, g_project->_->paint_objects->buffer[i])) {
				return false;
			}
		}
		return true;
	}
	return false;
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

void slot_layer_coverage_init(slot_layer_t *l) {
	l->all_objects = true;
	l->objects     = i32_array_create(0);
	l->all_tiles   = true;
	l->tiles       = i32_array_create(0);
}

static i32_array_t *_coverage_copy(i32_array_t *a) {
	return a != NULL ? i32_array_create_from_array(a) : i32_array_create(0);
}

static void _coverage_set(i32_array_t **dst, i32_array_t *src) {
	array_delete(*dst);
	*dst = _coverage_copy(src);
}

static void _coverage_union(i32_array_t *dst, i32_array_t *src) {
	for (i32 i = 0; src != NULL && i < src->length; ++i) {
		if (i32_array_index_of(dst, src->buffer[i]) < 0) {
			i32_array_push(dst, src->buffer[i]);
		}
	}
}

static void _coverage_intersect(i32_array_t *dst, i32_array_t *src) {
	for (i32 i = dst->length - 1; i >= 0; --i) {
		if (i32_array_index_of(src, dst->buffer[i]) < 0) {
			i32_array_splice(dst, i, 1);
		}
	}
}

// Deep copy, the arrays are never shared between layers
void slot_layer_coverage_copy(slot_layer_t *dst, slot_layer_t *src) {
	dst->all_objects = src->all_objects;
	dst->all_tiles   = src->all_tiles;
	_coverage_set(&dst->objects, src->objects);
	_coverage_set(&dst->tiles, src->tiles);
}

// Merged pixels keep showing wherever either layer did
void slot_layer_coverage_union(slot_layer_t *dst, slot_layer_t *src) {
	dst->all_objects = dst->all_objects || src->all_objects;
	dst->all_tiles   = dst->all_tiles || src->all_tiles;
	_coverage_union(dst->objects, src->objects);
	_coverage_union(dst->tiles, src->tiles);
}

// A group merged into its child restricts the result like it restricted the child
void slot_layer_coverage_intersect(slot_layer_t *dst, slot_layer_t *group) {
	if (_layer_level_objects(group)) {
		if (dst->all_objects) {
			_coverage_set(&dst->objects, group->objects);
		}
		else {
			_coverage_intersect(dst->objects, group->objects);
		}
		dst->all_objects = false;
	}
	if (_layer_level_tiles(group)) {
		if (dst->all_tiles) {
			_coverage_set(&dst->tiles, group->tiles);
		}
		else {
			_coverage_intersect(dst->tiles, group->tiles);
		}
		dst->all_tiles = false;
	}
}

void slot_layer_set_objects(slot_layer_t *l, i32 *ids, i32 count, bool all) {
	l->all_objects = all;
	if (ids != NULL || !all) {
		array_delete(l->objects);
		l->objects = i32_array_create_from_raw(ids, ids != NULL ? count : 0);
	}
}

void slot_layer_set_tiles(slot_layer_t *l, i32 *ids, i32 count, bool all) {
	l->all_tiles = all;
	if (ids != NULL || !all) {
		array_delete(l->tiles);
		l->tiles = i32_array_create_from_raw(ids, ids != NULL ? count : 0);
	}
}

// New layers made while the list is filtered only cover the filtered object
void slot_layer_coverage_from_filter(slot_layer_t *l) {
	if (g_context->layer_filter_object > 0) {
		slot_layer_set_objects(l, &g_context->layer_filter_object, 1, false);
	}
}

// Geometry of from now also lives in to (join, split), layers listing from also list to
void layers_coverage_add_alias(i32 from, i32 to) {
	for (i32 i = 0; g_project->_->layers != NULL && i < g_project->_->layers->length; ++i) {
		slot_layer_t *l = g_project->_->layers->buffer[i];
		if (l->objects != NULL && i32_array_index_of(l->objects, from) >= 0 && i32_array_index_of(l->objects, to) < 0) {
			i32_array_push(l->objects, to);
		}
	}
}
