
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

// Atlas layout of the mesh uvs. The "tex" vertex array (what the gpu and every cpu reader uses) is in atlas space:
// (slot xy + local) / stride in the short encoding, the slot being the position of the tile in the project list.
// md->_->udim_tex keeps the tile-local uvs and md->_->udim_tile the tile index (id - 1001), files store those.
// A project with the single tile 1001 has tex == local and no tile data, nothing here runs for it.
// Meshes made by tools that do not carry the tile data get it back from the atlas uvs (util_udim_ensure).

// Kept strictly inside the cell, so floor(uv * stride) in shaders and _util_udim_cell give the cell back even on tile borders
static i16 _util_udim_to_atlas(i16 local, i32 cell, i32 stride) {
	f32 step = 32767.0f / stride;
	f32 v    = local / (f32)stride + cell * step;
	f32 lo   = ceilf(cell * step);
	f32 hi   = cell >= stride - 1 ? 32767.0f : ceilf((cell + 1) * step) - 1.0f;
	v        = v < lo ? lo : v > hi ? hi : v;
	return (i16)(v + 0.5f > hi ? hi : v + 0.5f);
}

static i16 _util_udim_from_atlas(i16 atlas, i32 cell, i32 stride) {
	f32 local = (atlas - cell * (32767.0f / stride)) * stride;
	return local < 0.0f ? 0 : local > 32767.0f ? 32767 : (i16)(local + 0.5f);
}

// Atlas cell (column or row) of an atlas uv coordinate
static i32 _util_udim_cell(f32 atlas, i32 stride) {
	i32 c = (i32)(atlas * stride / 32767.0f);
	return c < 0 ? 0 : c >= stride ? stride - 1 : c;
}

// tex from udim_tex and udim_tile in the current layout, the gpu buffer is left as it is
static void _util_udim_layout_tex(mesh_data_t *md) {
	vertex_array_t *va     = mesh_data_get_vertex_array(md, "tex");
	i32             stride = util_udim_stride();
	i32             n      = md->_->udim_tile->length;
	for (i32 i = 0; i < n; ++i) {
		i32 slot                      = util_udim_slot(1001 + md->_->udim_tile->buffer[i]);
		slot                          = slot < 0 ? 0 : slot; // Tile missing from the list
		va->values->buffer[i * 2]     = _util_udim_to_atlas(md->_->udim_tex->buffer[i * 2], slot % stride, stride);
		va->values->buffer[i * 2 + 1] = _util_udim_to_atlas(md->_->udim_tex->buffer[i * 2 + 1], slot / stride, stride);
	}
}

void util_udim_apply_layout(mesh_data_t *md) {
	if (!util_udim_has_tiles(md)) {
		return;
	}
	_util_udim_layout_tex(md);
	if (md->_->vertex_buffer != NULL) {
		mesh_data_build_vertices(md->_->vertex_buffer, md->vertex_arrays);
	}
	util_mesh_changed();
}

// Tile data from the atlas uvs in the current layout. A triangle takes the slot under its uv centroid, its vertices that slot.
// A vertex shared by triangles of different slots (or used by none) takes the slot under its own uv instead (quantised).
static void _util_udim_derive(mesh_data_t *md) {
	util_udim_clear(md);
	vertex_array_t *va = mesh_data_get_vertex_array(md, "tex");
	if (va == NULL) {
		return;
	}
	i16_array_t *uv     = va->values;
	i32          n      = uv->length / 2;
	i32          stride = util_udim_stride();
	i32         *slots  = malloc((n > 0 ? n : 1) * sizeof(i32));
	for (i32 i = 0; i < n; ++i) {
		slots[i] = -1;
	}
	u32_array_t *ind = md->index_array;
	for (i32 t = 0; ind != NULL && t + 2 < ind->length; t += 3) {
		u32 a = ind->buffer[t];
		u32 b = ind->buffer[t + 1];
		u32 c = ind->buffer[t + 2];
		if (a >= (u32)n || b >= (u32)n || c >= (u32)n) {
			continue;
		}
		f32 cu   = (uv->buffer[a * 2] + uv->buffer[b * 2] + uv->buffer[c * 2]) / 3.0f;
		f32 cv   = (uv->buffer[a * 2 + 1] + uv->buffer[b * 2 + 1] + uv->buffer[c * 2 + 1]) / 3.0f;
		i32 slot = _util_udim_cell(cu, stride) + _util_udim_cell(cv, stride) * stride;
		for (i32 k = 0; k < 3; ++k) {
			u32 v    = ind->buffer[t + k];
			slots[v] = slots[v] == -1 || slots[v] == slot ? slot : -2;
		}
	}
	i16_array_t *tex  = i16_array_create(n * 2);
	i16_array_t *tile = i16_array_create(n);
	for (i32 i = 0; i < n; ++i) {
		i32 slot = slots[i];
		if (slot < 0) {
			slot = _util_udim_cell(uv->buffer[i * 2], stride) + _util_udim_cell(uv->buffer[i * 2 + 1], stride) * stride;
		}
		i32 id                 = util_udim_tile(slot);
		id                     = id < 0 ? util_udim_tile(0) : id; // Empty atlas cell
		tex->buffer[i * 2]     = _util_udim_from_atlas(uv->buffer[i * 2], slot % stride, stride);
		tex->buffer[i * 2 + 1] = _util_udim_from_atlas(uv->buffer[i * 2 + 1], slot / stride, stride);
		tile->buffer[i]        = id - 1001;
	}
	free(slots);
	md->_->udim_tex  = tex;
	md->_->udim_tile = tile;
}

// Tile data for a mesh built without it (mesh tools, mesh nodes, sculpt), call before reading udim_tex or udim_tile
void util_udim_ensure(mesh_data_t *md) {
	if (md != NULL && util_udim_active() && !util_udim_has_tiles(md)) {
		_util_udim_derive(md);
	}
}

// A mesh with no tile data is in 1001, tex must still be tile-local
void util_udim_set_default(mesh_data_t *md) {
	vertex_array_t *va = mesh_data_get_vertex_array(md, "tex");
	if (va == NULL || util_udim_has_tiles(md)) {
		return;
	}
	i16_array_t *tile = i16_array_create(va->values->length / 2);
	for (i32 i = 0; i < (i32)tile->length; ++i) {
		tile->buffer[i] = 0; // Every vertex in 1001
	}
	util_udim_set(md, tile);
}

// Same vertices in the same order (data duplicate, sculpt repack)
void util_udim_copy(mesh_data_t *dst, mesh_data_t *src) {
	util_udim_clear(dst);
	vertex_array_t *va = mesh_data_get_vertex_array(dst, "tex");
	if (util_udim_has_tiles(src) && va != NULL && va->values->length == src->_->udim_tex->length) {
		dst->_->udim_tex  = i16_array_create_from_array(src->_->udim_tex);
		dst->_->udim_tile = i16_array_create_from_array(src->_->udim_tile);
	}
}

// dst holds the vertices of objects one after the other (join), tex copied as is
void util_udim_concat(mesh_data_t *dst, mesh_object_t_array_t *objects) {
	util_udim_clear(dst);
	i32 n = 0;
	for (i32 i = 0; i < objects->length; ++i) {
		mesh_data_t *md = objects->buffer[i]->data;
		util_udim_ensure(md);
		if (!util_udim_has_tiles(md)) {
			return; // Single tile 1001 project, or derived later
		}
		n += md->_->udim_tile->length;
	}
	vertex_array_t *va = mesh_data_get_vertex_array(dst, "tex");
	if (va == NULL || va->values->length != n * 2) {
		return;
	}
	i16_array_t *tex  = i16_array_create(n * 2);
	i16_array_t *tile = i16_array_create(n);
	i32          off  = 0;
	for (i32 i = 0; i < objects->length; ++i) {
		mesh_data_t *md = objects->buffer[i]->data;
		i32          m  = md->_->udim_tile->length;
		memcpy(tex->buffer + off * 2, md->_->udim_tex->buffer, m * 2 * sizeof(i16));
		memcpy(tile->buffer + off, md->_->udim_tile->buffer, m * sizeof(i16));
		off += m;
	}
	dst->_->udim_tex  = tex;
	dst->_->udim_tile = tile;
}

// Uv unwrap wrote new 0..1 uvs into tex: in a udim project they go to tile 1001, call before the mesh is built
static bool util_udim_unwrap_missing = false;

void util_udim_unwrapped(mesh_data_t *md) {
	util_udim_clear(md);
	if (!util_udim_active()) {
		return;
	}
	util_udim_set_default(md);
	if (!util_udim_has_tiles(md)) {
		return;
	}
	if (util_udim_slot(1001) >= 0) {
		_util_udim_layout_tex(md);
	}
	else {
		util_udim_unwrap_missing = true;
	}
}

// After the unwrapped meshes are built, adds 1001 to the list when it was missing
void util_udim_unwrap_done() {
	if (util_udim_unwrap_missing) {
		util_udim_unwrap_missing = false;
		i32_array_t *ids         = i32_array_create(0);
		i32_array_push(ids, 1001);
		util_udim_tiles_add(ids);
		array_delete(ids);
	}
}

// Mesh datas laid out in the atlas: paint objects and the original meshes mesh nodes keep, caller frees
static any_array_t *_util_udim_meshes() {
	any_array_t *ar = nodes_mesh_held_inputs();
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		mesh_data_t *md = g_project->_->paint_objects->buffer[i]->data;
		if (md != NULL && array_index_of(ar, md) < 0) {
			any_array_push(ar, md);
		}
	}
	return ar;
}

bool util_udim_tile_used(i32 tile) {
	any_array_t *mds  = _util_udim_meshes();
	bool         used = false;
	for (i32 i = 0; i < mds->length && !used; ++i) {
		mesh_data_t *md = mds->buffer[i];
		util_udim_ensure(md);
		if (!util_udim_has_tiles(md)) {
			used = tile == 1001; // Single tile 1001 project
			continue;
		}
		for (i32 j = 0; j < md->_->udim_tile->length && !used; ++j) {
			used = md->_->udim_tile->buffer[j] + 1001 == tile;
		}
	}
	array_delete(mds);
	return used;
}

// Adds tiles to the project list (union, the list never shrinks here), the layout changes when the list does
void util_udim_tiles_add(i32_array_t *ids) {
	i32_array_t *tiles = i32_array_create(0);
	for (i32 i = 0; i < util_udim_count(); ++i) {
		i32_array_push(tiles, util_udim_tile(i));
	}
	for (i32 i = 0; ids != NULL && i < ids->length; ++i) {
		if (ids->buffer[i] > 1000 && i32_array_index_of(tiles, ids->buffer[i]) < 0) {
			i32_array_push(tiles, ids->buffer[i]);
		}
	}
	if (tiles->length != util_udim_count()) {
		util_udim_relayout(tiles);
	}
	array_delete(tiles);
}

// Removes an unused tile from the project list (Meshes tab), its pixels are dropped
void util_udim_tile_remove(i32 tile) {
	if (util_udim_count() < 2 || util_udim_slot(tile) < 0 || util_udim_tile_used(tile)) {
		return;
	}
	i32_array_t *tiles = i32_array_create(0);
	for (i32 i = 0; i < util_udim_count(); ++i) {
		if (util_udim_tile(i) != tile) {
			i32_array_push(tiles, util_udim_tile(i));
		}
	}
	util_udim_relayout(tiles);
	array_delete(tiles);
}

static void _util_udim_warn_clamped() {
	if (util_udim_count() < 2 || !util_udim_clamped()) {
		return;
	}
	any_map_t *vars = any_map_create();
	any_map_set(vars, "tiles", i32_to_string(util_udim_count()));
	any_map_set(vars, "res", i32_to_string(util_udim_tile_res_x()));
	any_map_set(vars, "atlas", i32_to_string(util_udim_res_x()));
	console_info(vtr("UDIM atlas too large: {tiles} tiles use {res} px per tile ({atlas} px atlas)", vars));
	map_free(vars);
}

static void _util_udim_update_fill_layers(void *_) {
	layers_update_fill_layers();
}

// Mesh uvs, merged mesh, uv maps and materials follow a changed layout
static void _util_udim_layout_changed(any_array_t *mds) {
	for (i32 i = 0; i < mds->length; ++i) {
		util_udim_apply_layout(mds->buffer[i]);
	}
	util_mesh_changed();
	if (g_context->merged_object != NULL) {
		util_mesh_remove_merged();
	}
	if (g_context->layer != NULL) {
		layers_update_paint_set(); // Merges the painted objects again
	}
	else if (g_project->_->paint_objects->length > 1) {
		util_mesh_merge(NULL);
	}
	util_uv_uvmap_cached       = false;
	util_uv_trianglemap_cached = false;
	util_uv_seammap_cached     = false;
	util_uv_dilatemap_cached   = false;
	render_path_raytrace_ready = false;
	nodes_mesh_request_refresh();
	make_material_parse_paint_material(false); // Materials repeat per tile only with more than one slot
	make_material_parse_mesh_material();
	g_context->ddirty = 2;
}

// Sets the project tile list to tiles and moves meshes and layer textures to the new layout.
// Tiles in both lists keep their pixels, new tiles start empty, removed tiles are dropped. Undo history is cleared.
void util_udim_relayout(i32_array_t *tiles) {
	// Tile data of every mesh, read under the old layout
	any_array_t *mds = _util_udim_meshes();
	for (i32 i = 0; i < mds->length; ++i) {
		if (!util_udim_has_tiles(mds->buffer[i])) {
			_util_udim_derive(mds->buffer[i]);
		}
	}
	i32_array_t *old_tiles  = i32_array_create(0);
	i32          old_stride = util_udim_stride();
	for (i32 i = 0; i < util_udim_count(); ++i) {
		i32_array_push(old_tiles, util_udim_tile(i));
	}
	util_udim_set_tiles(tiles);

	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	layers_udim_relayout(old_tiles, old_stride);
	if (in_use) {
		draw_begin(current, false, 0);
	}

	_util_udim_layout_changed(mds);
	if (!util_udim_active()) {
		// Back to the single tile 1001, tex is local again and the tile data is dropped
		for (i32 i = 0; i < mds->length; ++i) {
			util_udim_clear(mds->buffer[i]);
		}
	}
	array_delete(mds);
	array_delete(old_tiles);

	history_reset();
	sys_notify_on_next_frame(&_util_udim_update_fill_layers, NULL);
	console_info(tr("Tile layout changed, undo history was cleared"));
	_util_udim_warn_clamped();
}

// Meshes just imported (tex tile-local, tile data or none for all in 1001) join the project layout.
// fresh: the import replaced the project and cleared its layers, the list becomes exactly the imported tiles.
void util_udim_import_meshes(any_array_t *mds, bool fresh) {
	i32_array_t *ids = i32_array_create(0);
	for (i32 i = 0; i < mds->length; ++i) {
		mesh_data_t *md = mds->buffer[i];
		if (!util_udim_has_tiles(md)) {
			if (i32_array_index_of(ids, 1001) < 0) {
				i32_array_push(ids, 1001);
			}
			continue;
		}
		for (i32 j = 0; j < md->_->udim_tile->length; ++j) {
			i32 id = md->_->udim_tile->buffer[j] + 1001;
			if (i32_array_index_of(ids, id) < 0) {
				i32_array_push(ids, id);
			}
		}
	}

	// The list the import leads to, meshes without tiles need tile data unless it is the single tile 1001
	bool active = false;
	if (fresh) {
		active = ids->length > 1 || (ids->length == 1 && ids->buffer[0] != 1001);
	}
	else {
		active = util_udim_active();
		for (i32 i = 0; i < ids->length && !active; ++i) {
			active = ids->buffer[i] != 1001;
		}
	}
	if (active) {
		for (i32 i = 0; i < mds->length; ++i) {
			util_udim_set_default(mds->buffer[i]);
		}
	}

	if (fresh) {
		util_udim_set_tiles(ids);
		if (util_udim_count() > 1) {
			base_res = TEXTURE_RES_RES2048; // New udim projects default to 2k per tile
			config_set_texture_res(TEXTURE_RES_RES2048);
		}
		// The layers were just cleared to a single color, resizing them loses nothing
		slot_layer_t *l0 = g_project->_->layers->length > 0 ? g_project->_->layers->buffer[0] : NULL;
		if (l0 != NULL && l0->texpaint != NULL && (l0->texpaint->width != util_udim_res_x() || l0->texpaint->height != util_udim_res_y())) {
			gpu_texture_t *current = _draw_current;
			bool           in_use  = gpu_in_use;
			if (in_use) {
				draw_end();
			}
			layers_resize();
			if (in_use) {
				draw_begin(current, false, 0);
			}
		}
		for (i32 i = 0; i < mds->length; ++i) {
			util_udim_apply_layout(mds->buffer[i]);
		}
		_util_udim_warn_clamped();
	}
	else {
		i32 count = util_udim_count();
		util_udim_tiles_add(ids);
		if (util_udim_count() == count) {
			for (i32 i = 0; i < mds->length; ++i) { // Relayout lays out every mesh
				util_udim_apply_layout(mds->buffer[i]);
			}
		}
	}
	array_delete(ids);
}

// Load: meshes have their saved tile data and tile-local tex, the list is already the saved one
void util_udim_apply_layout_loaded() {
	if (!util_udim_active()) {
		return;
	}
	mesh_object_t_array_t *objects = util_mesh_get_unique_data();
	for (i32 i = 0; i < objects->length; ++i) {
		util_udim_set_default(objects->buffer[i]->data); // Saved without tiles, in 1001
		util_udim_apply_layout(objects->buffer[i]->data);
	}
	array_delete(objects);
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

// Takes ownership of tile, tex must still be tile-local (imported or loaded mesh before util_udim_apply_layout)
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

// tex was edited in place (uv editor), the tile data is taken again from the atlas uvs
void util_udim_tex_changed(mesh_data_t *md) {
	util_udim_clear(md);
	util_udim_ensure(md);
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

// Whether shaders test the tile for the layer: it leaves out one of several project tiles
bool layer_tiles_tested(slot_layer_t *l) {
	if (util_udim_count() < 2 || !layer_tiles_restricted(l)) {
		return false;
	}
	for (i32 s = 0; s < util_udim_count(); ++s) {
		if (!layer_covers_tile(l, util_udim_tile(s))) {
			return true;
		}
	}
	return false;
}

// Declares "int udim_slot" in the fragment from an atlas uv, use the primary uv: the tile is a property of the geometry
void util_udim_write_slot(node_shader_t *kong, char *uv) {
	node_shader_add_constant(kong, "float atlas_stride", "_atlas_stride");
	// uv 1.0 at the atlas edge stays in the last row or column
	node_shader_write_frag(kong, string_tmp("int udim_slot = int(min(floor(%s.x * constants.atlas_stride), constants.atlas_stride - 1.0)) + "
	                                        "int(min(floor(%s.y * constants.atlas_stride), constants.atlas_stride - 1.0)) * int(constants.atlas_stride);",
	                                        uv, uv));
}

// OR of the slot ranges the layer covers (covered) or leaves out (!covered, empty atlas cells included), "false" when there are none
static void _layer_write_slot_ranges(node_shader_t *kong, slot_layer_t *l, bool covered) {
	i32  count = util_udim_count();
	bool first = true;
	for (i32 s = 0; s < count; ++s) {
		if (layer_covers_tile(l, util_udim_tile(s)) != covered) {
			continue;
		}
		i32 a = s;
		while (s + 1 < count && layer_covers_tile(l, util_udim_tile(s + 1)) == covered) {
			++s;
		}
		if (!first) {
			node_shader_write_frag(kong, " || ");
		}
		first = false;
		if (a == s) {
			node_shader_write_frag(kong, string_tmp("udim_slot == %s", i32_to_string(a)));
		}
		else {
			node_shader_write_frag(kong, string_tmp("(udim_slot >= %s && ", i32_to_string(a)));
			node_shader_write_frag(kong, string_tmp("udim_slot <= %s)", i32_to_string(s)));
		}
	}
	i32 stride = util_udim_stride();
	if (!covered && count < stride * stride) {
		if (!first) {
			node_shader_write_frag(kong, " || ");
		}
		first = false;
		node_shader_write_frag(kong, string_tmp("udim_slot >= %s", i32_to_string(count)));
	}
	if (first) {
		node_shader_write_frag(kong, "false");
	}
}

// Discards fragments on tiles the layer leaves out, udim_slot must be declared (util_udim_write_slot) when layer_tiles_tested
void layer_write_tile_discard(node_shader_t *kong, slot_layer_t *l) {
	if (!layer_tiles_tested(l)) {
		return;
	}
	node_shader_write_frag(kong, "if (");
	_layer_write_slot_ranges(kong, l, false);
	node_shader_write_frag(kong, ") { discard; }");
}

// Opens "if (...) {" around the layer for objects or tiles outside its coverage, false when nothing is written.
// The tile part needs udim_slot (util_udim_write_slot) when layer_tiles_tested. With a single tile a tile restriction
// either covers it (no test) or covers nothing (false, such layers are skipped before).
bool layer_write_coverage_test(node_shader_t *kong, slot_layer_t *l) {
	bool objects    = layer_objects_restricted(l);
	bool tiles      = layer_tiles_tested(l);
	bool tiles_none = !tiles && layer_tiles_restricted(l) && !layer_covers_tile(l, util_udim_tile(0)) && util_udim_count() < 2;
	if (!objects && !tiles && !tiles_none) {
		return false;
	}
	if (!objects) {
		node_shader_write_frag(kong, "if (");
		if (tiles) {
			_layer_write_slot_ranges(kong, l, true);
		}
		else {
			node_shader_write_frag(kong, "false");
		}
		node_shader_write_frag(kong, ") {");
		return true;
	}
	bool both = tiles || tiles_none;

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
	node_shader_write_frag(kong, both ? "if ((" : "if (");
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
	if (both) {
		node_shader_write_frag(kong, ") && (");
		if (tiles) {
			_layer_write_slot_ranges(kong, l, true);
		}
		else {
			node_shader_write_frag(kong, "false");
		}
		node_shader_write_frag(kong, ")");
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

// New layers made while the list is filtered only cover the filtered object or tile
void slot_layer_coverage_from_filter(slot_layer_t *l) {
	if (g_context->layer_filter_object > 0) {
		slot_layer_set_objects(l, &g_context->layer_filter_object, 1, false);
	}
	else if (g_context->layer_filter_tile > 0 && util_udim_slot(g_context->layer_filter_tile) >= 0) {
		slot_layer_set_tiles(l, &g_context->layer_filter_tile, 1, false);
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

// Uv overlap between a covered and an uncovered object (DECISIONS item 5): a texture can not tell them apart, so exports
// and path tracing apply the layer to both. A bbox test per object and slot first, then a low res raster of the slot.

#define UTIL_UDIM_OVERLAP_RES 64

static i32_array_t *_layer_overlap_key     = NULL;
static i32          _layer_overlap_version = -1;
static i32          _layer_overlap_a       = 0;
static i32          _layer_overlap_b       = 0;
static i32          _layer_overlap_tile    = 0;

// Slot of a triangle from its atlas uv centroid, -1 on an empty atlas cell
static i32 _util_udim_triangle_slot(i16 *tex, u32 *ind, i32 t, i32 stride) {
	f32 u = (tex[ind[t * 3] * 2] + tex[ind[t * 3 + 1] * 2] + tex[ind[t * 3 + 2] * 2]) / 3.0f;
	f32 v = (tex[ind[t * 3] * 2 + 1] + tex[ind[t * 3 + 1] * 2 + 1] + tex[ind[t * 3 + 2] * 2 + 1]) / 3.0f;
	i32 s = _util_udim_cell(v, stride) * stride + _util_udim_cell(u, stride);
	return s < util_udim_count() ? s : -1;
}

// Uv bounds (min u, min v, max u, max v, atlas short units) of each object's triangles per slot, min > max when empty
static f32 *_util_udim_object_bounds(mesh_object_t *o, i32 count, i32 stride) {
	f32 *b = malloc(sizeof(f32) * 4 * count);
	for (i32 s = 0; s < count; ++s) {
		b[s * 4]     = 32767.0f;
		b[s * 4 + 1] = 32767.0f;
		b[s * 4 + 2] = -1.0f;
		b[s * 4 + 3] = -1.0f;
	}
	vertex_array_t *va = o->data != NULL ? mesh_data_get_vertex_array(o->data, "tex") : NULL;
	if (va == NULL) {
		return b;
	}
	i16 *tex = va->values->buffer;
	u32 *ind = o->data->index_array->buffer;
	i32  n   = o->data->index_array->length / 3;
	for (i32 t = 0; t < n; ++t) {
		i32 s = _util_udim_triangle_slot(tex, ind, t, stride);
		if (s < 0) {
			continue;
		}
		for (i32 k = 0; k < 3; ++k) {
			f32 u        = tex[ind[t * 3 + k] * 2];
			f32 v        = tex[ind[t * 3 + k] * 2 + 1];
			b[s * 4]     = u < b[s * 4] ? u : b[s * 4];
			b[s * 4 + 1] = v < b[s * 4 + 1] ? v : b[s * 4 + 1];
			b[s * 4 + 2] = u > b[s * 4 + 2] ? u : b[s * 4 + 2];
			b[s * 4 + 3] = v > b[s * 4 + 3] ? v : b[s * 4 + 3];
		}
	}
	return b;
}

// Marks (or with test, looks for marked) raster cells whose centre is strictly inside the object's triangles on the slot.
// Strict, so objects that only touch along an edge do not count.
static bool _util_udim_raster_object(u8 *bits, mesh_object_t *o, i32 slot, i32 stride, bool test) {
	vertex_array_t *va   = mesh_data_get_vertex_array(o->data, "tex");
	i16            *tex  = va->values->buffer;
	u32            *ind  = o->data->index_array->buffer;
	i32             n    = o->data->index_array->length / 3;
	f32             step = 32767.0f / stride;
	f32             ox   = (slot % stride) * step;
	f32             oy   = (slot / stride) * step;
	f32             f    = UTIL_UDIM_OVERLAP_RES / step;
	for (i32 t = 0; t < n; ++t) {
		if (_util_udim_triangle_slot(tex, ind, t, stride) != slot) {
			continue;
		}
		f32 x[3];
		f32 y[3];
		for (i32 k = 0; k < 3; ++k) {
			x[k] = (tex[ind[t * 3 + k] * 2] - ox) * f;
			y[k] = (tex[ind[t * 3 + k] * 2 + 1] - oy) * f;
		}
		f32 area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
		if (area == 0.0f) {
			continue;
		}
		f32 sign = area > 0.0f ? 1.0f : -1.0f;
		i32 x0   = (i32)floorf(fminf(x[0], fminf(x[1], x[2])));
		i32 x1   = (i32)ceilf(fmaxf(x[0], fmaxf(x[1], x[2])));
		i32 y0   = (i32)floorf(fminf(y[0], fminf(y[1], y[2])));
		i32 y1   = (i32)ceilf(fmaxf(y[0], fmaxf(y[1], y[2])));
		x0       = x0 < 0 ? 0 : x0;
		y0       = y0 < 0 ? 0 : y0;
		x1       = x1 > UTIL_UDIM_OVERLAP_RES ? UTIL_UDIM_OVERLAP_RES : x1;
		y1       = y1 > UTIL_UDIM_OVERLAP_RES ? UTIL_UDIM_OVERLAP_RES : y1;
		for (i32 py = y0; py < y1; ++py) {
			for (i32 px = x0; px < x1; ++px) {
				f32 cx = px + 0.5f;
				f32 cy = py + 0.5f;
				f32 e0 = ((x[1] - x[0]) * (cy - y[0]) - (y[1] - y[0]) * (cx - x[0])) * sign;
				f32 e1 = ((x[2] - x[1]) * (cy - y[1]) - (y[2] - y[1]) * (cx - x[1])) * sign;
				f32 e2 = ((x[0] - x[2]) * (cy - y[2]) - (y[0] - y[2]) * (cx - x[2])) * sign;
				if (e0 <= 0.0f || e1 <= 0.0f || e2 <= 0.0f) {
					continue;
				}
				u8 *c = &bits[py * UTIL_UDIM_OVERLAP_RES + px];
				if (test && *c) {
					return true;
				}
				if (!test) {
					*c = 1;
				}
			}
		}
	}
	return false;
}

// Covered live ids, covered tiles, live ids and the project tile list, each list prefixed with its length
static i32_array_t *_layer_overlap_make_key(slot_layer_t *l) {
	i32_array_t *covered = i32_array_create(0);
	i32_array_t *live    = i32_array_create(0);
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		mesh_object_t *o = g_project->_->paint_objects->buffer[i];
		i32_array_push(live, o->paint_id);
		if (layer_covers_object(l, o)) {
			i32_array_push(covered, o->paint_id);
		}
	}
	i32_array_sort(covered, _util_udim_compare_i32);
	i32_array_sort(live, _util_udim_compare_i32);
	i32_array_t *key = i32_array_create(0);
	i32_array_push(key, covered->length);
	for (i32 i = 0; i < covered->length; ++i) {
		i32_array_push(key, covered->buffer[i]);
	}
	i32_array_push(key, live->length);
	for (i32 i = 0; i < live->length; ++i) {
		i32_array_push(key, live->buffer[i]);
	}
	i32 count = util_udim_count();
	i32_array_push(key, count);
	for (i32 s = 0; s < count; ++s) {
		i32_array_push(key, util_udim_tile(s));
		i32_array_push(key, layer_covers_tile(l, util_udim_tile(s)) ? 1 : 0);
	}
	array_delete(covered);
	array_delete(live);
	return key;
}

static bool _layer_overlap_key_equals(i32_array_t *a, i32_array_t *b) {
	if (a == NULL || b == NULL || a->length != b->length) {
		return false;
	}
	for (i32 i = 0; i < a->length; ++i) {
		if (a->buffer[i] != b->buffer[i]) {
			return false;
		}
	}
	return true;
}

static void _layer_overlap_find(slot_layer_t *l) {
	_layer_overlap_a               = 0;
	_layer_overlap_b               = 0;
	_layer_overlap_tile            = 0;
	mesh_object_t_array_t *objects = g_project->_->paint_objects;
	i32                    count   = util_udim_count();
	i32                    stride  = util_udim_stride();
	f32                  **bounds  = malloc(sizeof(f32 *) * (objects->length > 0 ? objects->length : 1));
	for (i32 i = 0; i < objects->length; ++i) {
		bounds[i] = _util_udim_object_bounds(objects->buffer[i], count, stride);
	}
	u8 *bits = malloc(UTIL_UDIM_OVERLAP_RES * UTIL_UDIM_OVERLAP_RES);
	for (i32 i = 0; i < objects->length && _layer_overlap_a == 0; ++i) {
		mesh_object_t *a = objects->buffer[i];
		if (a->paint_id <= 0 || !layer_covers_object(l, a)) {
			continue;
		}
		for (i32 s = 0; s < count && _layer_overlap_a == 0; ++s) {
			f32 *ba = &bounds[i][s * 4];
			if (ba[0] > ba[2] || !layer_covers_tile(l, util_udim_tile(s))) {
				continue;
			}
			bool drawn = false;
			for (i32 j = 0; j < objects->length; ++j) {
				mesh_object_t *b  = objects->buffer[j];
				f32           *bb = &bounds[j][s * 4];
				if (b->paint_id <= 0 || layer_covers_object(l, b) || bb[0] > bb[2]) {
					continue;
				}
				if (ba[2] <= bb[0] || bb[2] <= ba[0] || ba[3] <= bb[1] || bb[3] <= ba[1]) {
					continue;
				}
				if (!drawn) {
					memset(bits, 0, UTIL_UDIM_OVERLAP_RES * UTIL_UDIM_OVERLAP_RES);
					_util_udim_raster_object(bits, a, s, stride, false);
					drawn = true;
				}
				if (_util_udim_raster_object(bits, b, s, stride, true)) {
					_layer_overlap_a    = a->paint_id;
					_layer_overlap_b    = b->paint_id;
					_layer_overlap_tile = util_udim_tile(s);
					break;
				}
			}
		}
	}
	free(bits);
	for (i32 i = 0; i < objects->length; ++i) {
		free(bounds[i]);
	}
	free(bounds);
}

// First covered object that shares uv space with an uncovered live object on a tile the layer covers (paint ids, udim id).
// Only object restricted layers, cached by coverage, live objects, util_mesh_version and the tile list.
bool layer_uv_overlap(slot_layer_t *l, i32 *object, i32 *other, i32 *tile) {
	if (!layer_objects_restricted(l)) {
		return false;
	}
	i32_array_t *key = _layer_overlap_make_key(l);
	if (_layer_overlap_version != util_mesh_version || !_layer_overlap_key_equals(key, _layer_overlap_key)) {
		_layer_overlap_find(l);
		array_delete(_layer_overlap_key);
		_layer_overlap_key     = key;
		_layer_overlap_version = util_mesh_version;
	}
	else {
		array_delete(key);
	}
	*object = _layer_overlap_a;
	*other  = _layer_overlap_b;
	*tile   = _layer_overlap_tile;
	return _layer_overlap_a != 0;
}
