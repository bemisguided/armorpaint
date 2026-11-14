
#include "../global.h"

// Layer coverage: a layer applies to (object in objects or all_objects) and (tile in tiles or all_tiles).
// Objects are persistent paint ids (mesh_object_t.paint_id), tiles are udim ids (1001..).
// A layer's effective coverage is its own set and every ancestor group's set (masks and filters use their parent's).
// A level only restricts when its list exists, so zero initialized temporary layers cover everything.

// Project tile list (g_project->_->udim_tiles): sorted, distinct, never empty, {1001} by default.
// Slots are the positions in the list, the atlas has stride x stride slots (see util_udim_stride).
// The configured project resolution (base_res_x/y) is per tile, layers and targets use the atlas size util_udim_res_x/y.

#define UTIL_UDIM_MAX_SIDE 16384

static int _util_udim_compare_i32(const void *a, const void *b) {
	return *(i32 *)a - *(i32 *)b;
}

i32 util_udim_count() {
	i32_array_t *t = g_project->_->udim_tiles;
	return t != NULL && t->length > 0 ? t->length : 1;
}

i32 util_udim_tile(i32 slot) {
	i32_array_t *t = g_project->_->udim_tiles;
	if (t == NULL || t->length == 0) {
		return slot == 0 ? 1001 : -1;
	}
	return slot >= 0 && slot < t->length ? t->buffer[slot] : -1;
}

i32 util_udim_slot(i32 tile) {
	i32_array_t *t = g_project->_->udim_tiles;
	if (t == NULL || t->length == 0) {
		return tile == 1001 ? 0 : -1;
	}
	return i32_array_index_of(t, tile);
}

// Smallest s with s * s >= count
i32 util_udim_stride() {
	i32 count = util_udim_count();
	i32 s     = 1;
	while (s * s < count) {
		++s;
	}
	return s;
}

bool util_udim_active() {
	return util_udim_count() > 1 || util_udim_tile(0) != 1001;
}

// Per tile resolution, the configured one unless the atlas would pass the max side
i32 util_udim_tile_res_x() {
	i32 max = UTIL_UDIM_MAX_SIDE / util_udim_stride();
	i32 res = config_get_texture_res_x();
	return res < max ? res : max;
}

i32 util_udim_tile_res_y() {
	i32 max = UTIL_UDIM_MAX_SIDE / util_udim_stride();
	i32 res = config_get_texture_res_y();
	return res < max ? res : max;
}

// Atlas size, the configured resolution when the project has one slot
i32 util_udim_res_x() {
	return util_udim_tile_res_x() * util_udim_stride();
}

i32 util_udim_res_y() {
	return util_udim_tile_res_y() * util_udim_stride();
}

// The per tile resolution was lowered to fit the atlas
bool util_udim_clamped() {
	return util_udim_tile_res_x() < config_get_texture_res_x() || util_udim_tile_res_y() < config_get_texture_res_y();
}

// Bytes of the layer textures (3 per layer) at the atlas size
f64 util_udim_memory_estimate() {
	i32 layers = g_project->_->layers != NULL && g_project->_->layers->length > 0 ? g_project->_->layers->length : 1;
	i32 bytes  = base_bits == TEXTURE_BITS_BITS8 ? 4 : base_bits == TEXTURE_BITS_BITS16 ? 8 : 16;
	return (f64)layers * 3.0 * (f64)util_udim_res_x() * (f64)util_udim_res_y() * (f64)bytes;
}

// Sets the project tile list (copied, sorted, distinct, NULL or empty gives {1001}), the layout is not changed
void util_udim_set_tiles(i32_array_t *ids) {
	i32_array_t *t = i32_array_create(0);
	for (i32 i = 0; ids != NULL && i < ids->length; ++i) {
		if (ids->buffer[i] > 0 && i32_array_index_of(t, ids->buffer[i]) < 0) {
			i32_array_push(t, ids->buffer[i]);
		}
	}
	if (t->length == 0) {
		i32_array_push(t, 1001);
	}
	i32_array_sort(t, _util_udim_compare_i32);
	array_delete(g_project->_->udim_tiles);
	g_project->_->udim_tiles = t;
}

// Tile extraction at import (obj, fbx). A triangle takes the tile under its uv centroid (u in 0..10, v >= 0, outside clamps),
// its corners get uvs local to that tile. Corners outside the tile (by more than UTIL_UDIM_EPS) cross a tile border and are clamped.
// Uvs are per corner and vertices are not shared yet, so a vertex on a tile border becomes one vertex per tile.

#define UTIL_UDIM_EPS 1e-4f

static i32  util_udim_scan_first     = -1;
static bool util_udim_scan_multi     = false;
static i32  util_udim_scan_straddles = 0;

static void _util_udim_triangle_tile(f32 *uv, i32 *tu, i32 *tv) {
	f32 cu = (uv[0] + uv[2] + uv[4]) / 3.0f;
	f32 cv = (uv[1] + uv[3] + uv[5]) / 3.0f;
	*tu    = !(cu >= 0.0f) ? 0 : cu >= 10.0f ? 9 : (i32)cu;
	*tv    = !(cv >= 0.0f) ? 0 : cv >= 100.0f ? 99 : (i32)cv;
}

static bool _util_udim_triangle_straddles(f32 *uv, i32 tu, i32 tv) {
	for (i32 k = 0; k < 3; ++k) {
		f32 u = uv[k * 2] - tu;
		f32 v = uv[k * 2 + 1] - tv;
		if (!(u >= -UTIL_UDIM_EPS && u <= 1.0f + UTIL_UDIM_EPS && v >= -UTIL_UDIM_EPS && v <= 1.0f + UTIL_UDIM_EPS)) {
			return true;
		}
	}
	return false;
}

// Tiles are decided over a whole imported file: begin, scan the uvs of every mesh, end
void util_udim_scan_begin() {
	util_udim_scan_first     = -1;
	util_udim_scan_multi     = false;
	util_udim_scan_straddles = 0;
}

// uv: per corner, three corners per triangle
void util_udim_scan(f32 *uv, i32 corners) {
	for (i32 i = 0; uv != NULL && i + 2 < corners; i += 3) {
		i32 tu;
		i32 tv;
		_util_udim_triangle_tile(uv + i * 2, &tu, &tv);
		i32 tile = tu + tv * 10;
		if (util_udim_scan_first < 0) {
			util_udim_scan_first = tile;
		}
		else if (tile != util_udim_scan_first) {
			util_udim_scan_multi = true;
		}
		if (_util_udim_triangle_straddles(uv + i * 2, tu, tv)) {
			util_udim_scan_straddles++;
		}
	}
}

// Whether the scanned file is imported with tiles (UDIM tiles import option)
bool util_udim_scan_end() {
	udim_import_t mode  = g_context->udim_import;
	bool          tiles = mode == UDIM_IMPORT_ON || (mode == UDIM_IMPORT_AUTO && util_udim_scan_multi && util_udim_scan_straddles == 0);
	if (tiles && util_udim_scan_straddles > 0) {
		any_map_t *vars = any_map_create();
		any_map_set(vars, "count", i32_to_string(util_udim_scan_straddles));
		console_info(vtr("{count} triangles cross UDIM tile borders and were clamped", vars));
		map_free(vars);
	}
	return tiles;
}

// Writes raw->texa from per corner uvs (three corners per triangle, vertices not shared yet).
// With tiles texa is tile-local and raw->tilea holds the tile index per vertex (NULL when every triangle is in 1001),
// without tiles uvs above 1 wrap into 0..1 as the obj importer always did.
void util_udim_split_raw(raw_mesh_t *raw, f32 *uv, i32 corners, bool tiles) {
	if (uv == NULL) {
		return;
	}
	i16_array_t *texa = i16_array_create(corners * 2);
	if (!tiles) {
		for (i32 i = 0; i < corners; ++i) {
			float uvx = uv[i * 2];
			if (uvx > 1.0) {
				uvx = uvx - (int)(uvx);
			}
			float uvy = uv[i * 2 + 1];
			if (uvy > 1.0) {
				uvy = uvy - (int)(uvy);
			}
			texa->buffer[i * 2]     = (int)(uvx * 32767);
			texa->buffer[i * 2 + 1] = (int)((1.0 - uvy) * 32767);
		}
	}
	else {
		i16_array_t *tilea = i16_array_create(corners);
		bool         any   = false;
		for (i32 i = 0; i < corners; i += 3) {
			i32 n  = corners - i < 3 ? corners - i : 3;
			i32 tu = 0;
			i32 tv = 0;
			if (n == 3) {
				_util_udim_triangle_tile(uv + i * 2, &tu, &tv);
			}
			for (i32 k = i; k < i + n; ++k) {
				f32 u                   = uv[k * 2] - tu;
				f32 v                   = uv[k * 2 + 1] - tv;
				u                       = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
				v                       = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
				texa->buffer[k * 2]     = (int)(u * 32767);
				texa->buffer[k * 2 + 1] = (int)((1.0 - v) * 32767);
				tilea->buffer[k]        = tu + tv * 10;
			}
			any = any || tu + tv * 10 != 0;
		}
		array_delete(raw->tilea);
		raw->tilea = NULL;
		if (any) {
			raw->tilea = tilea;
		}
		else {
			array_delete(tilea);
		}
	}
	array_delete(raw->texa);
	raw->texa = texa;
}

// Tile data of a mesh: tile-local uvs (udim_tex) and the tile index per vertex (udim_tile), both or neither
bool util_udim_has_tiles(mesh_data_t *md) {
	if (md == NULL || md->_ == NULL || md->_->udim_tex == NULL || md->_->udim_tile == NULL) {
		return false;
	}
	vertex_array_t *tex = mesh_data_get_vertex_array(md, "tex");
	return tex != NULL && md->_->udim_tex->length == tex->values->length && md->_->udim_tile->length * 2 == tex->values->length;
}

void util_udim_clear(mesh_data_t *md) {
	array_delete(md->_->udim_tex);
	array_delete(md->_->udim_tile);
	md->_->udim_tex  = NULL;
	md->_->udim_tile = NULL;
}

// Takes ownership of tile; tex is tile-local while the project has a single tile slot, so udim_tex is a copy of it
void util_udim_set(mesh_data_t *md, i16_array_t *tile) {
	util_udim_clear(md);
	vertex_array_t *tex = mesh_data_get_vertex_array(md, "tex");
	if (tile == NULL || tex == NULL || tile->length * 2 != tex->values->length) {
		array_delete(tile);
		return;
	}
	md->_->udim_tile = tile;
	md->_->udim_tex  = i16_array_create_from_array(tex->values);
}

// Moves the tile data of an imported mesh into its runtime
void util_udim_take_raw(mesh_data_t *md, raw_mesh_t *raw) {
	if (raw->tilea != NULL) {
		util_udim_set(md, raw->tilea);
		raw->tilea = NULL;
	}
}

// tex was edited in place (uv editor): tex is tile-local while the project has a single tile slot
void util_udim_tex_changed(mesh_data_t *md) {
	if (md->_->udim_tex == NULL) {
		return;
	}
	vertex_array_t *tex = mesh_data_get_vertex_array(md, "tex");
	if (tex != NULL && tex->values->length == md->_->udim_tex->length) {
		memcpy(md->_->udim_tex->buffer, tex->values->buffer, tex->values->length * sizeof(i16));
	}
	else {
		util_udim_clear(md);
	}
}

// Udim uv of vertex i for mesh export (tile-local uv + tile offset, v up as in obj), the mesh must have tile data
void util_udim_vertex_uv(mesh_data_t *md, i32 i, f32 *u, f32 *v) {
	f32 inv  = 1 / 32767.0;
	i32 tile = md->_->udim_tile->buffer[i];
	*u       = md->_->udim_tex->buffer[i * 2] * inv + tile % 10;
	*v       = 1.0 - md->_->udim_tex->buffer[i * 2 + 1] * inv + tile / 10;
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

// Whether the groups above the layer (not the layer itself) leave the object or tile in its coverage
bool layer_ancestors_cover_object_id(slot_layer_t *l, i32 paint_id) {
	for (slot_layer_t *p = slot_layer_coverage_owner(l)->parent; p != NULL; p = p->parent) {
		if (_layer_level_objects(p) && i32_array_index_of(p->objects, paint_id) < 0) {
			return false;
		}
	}
	return true;
}

bool layer_ancestors_cover_tile(slot_layer_t *l, i32 tile) {
	for (slot_layer_t *p = slot_layer_coverage_owner(l)->parent; p != NULL; p = p->parent) {
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

// Ids every restricting level lists (the stored coverage, deleted ids included), NULL when objects are unrestricted, caller frees
static i32_array_t *_layer_stored_objects(slot_layer_t *l) {
	for (slot_layer_t *p = slot_layer_coverage_owner(l); p != NULL; p = p->parent) {
		if (!_layer_level_objects(p)) {
			continue;
		}
		i32_array_t *ids = i32_array_create(0);
		for (i32 i = 0; i < p->objects->length; ++i) {
			i32 id = p->objects->buffer[i];
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

// Copies before freeing, src may be *dst
static void _coverage_set(i32_array_t **dst, i32_array_t *src) {
	i32_array_t *old = *dst;
	*dst             = _coverage_copy(src);
	array_delete(old);
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

// The list is replaced when ids are given or all is off, ids may point into the current list
static void _coverage_replace(bool *dst_all, i32_array_t **dst, i32 *ids, i32 count, bool all) {
	*dst_all = all;
	if (ids != NULL || !all) {
		i32_array_t *old = *dst;
		*dst             = i32_array_create_from_raw(ids, ids != NULL ? count : 0);
		array_delete(old);
	}
}

// Whether _coverage_replace with these arguments would change anything, the list compares as a set
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
	i32 cur_count = cur != NULL ? cur->length : 0;
	for (i32 i = 0; i < count; ++i) {
		if (cur == NULL || i32_array_index_of(cur, ids[i]) < 0) {
			return true;
		}
	}
	for (i32 i = 0; i < cur_count; ++i) {
		bool found = false;
		for (i32 j = 0; j < count && !found; ++j) {
			found = ids[j] == cur->buffer[i];
		}
		if (!found) {
			return true;
		}
	}
	return cur == NULL && !all;
}

void slot_layer_set_objects(slot_layer_t *l, i32 *ids, i32 count, bool all) {
	_coverage_replace(&l->all_objects, &l->objects, ids, count, all);
}

void slot_layer_set_tiles(slot_layer_t *l, i32 *ids, i32 count, bool all) {
	_coverage_replace(&l->all_tiles, &l->tiles, ids, count, all);
}

bool slot_layer_objects_differ(slot_layer_t *l, i32 *ids, i32 count, bool all) {
	return _coverage_differs(l->all_objects, l->objects, ids, count, all);
}

bool slot_layer_tiles_differ(slot_layer_t *l, i32 *ids, i32 count, bool all) {
	return _coverage_differs(l->all_tiles, l->tiles, ids, count, all);
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
		if (l->objects != NULL && i32_array_index_of(l->objects, from) >= 0 && i32_array_index_of(l->objects, to) < 0) {
			i32_array_push(l->objects, to);
		}
	}
}
