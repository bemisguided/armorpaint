
#include "../global.h"

// Project tile list (g_project->_->udim_tiles): sorted, distinct, never empty, {1001} by default.
// Slots are the positions in the list, the atlas has stride x stride slots (see util_udim_stride).
// The configured project resolution (base_res_x/y) is per tile, layers and targets use the atlas size config_get_texture_res_x/y.

// Side of the atlas of a project with several tiles, the per tile resolution is lowered to fit. One tile keeps the configured size.
#define UTIL_UDIM_MAX_SIDE 8192

#define UTIL_UDIM_FIRST_TILE 1001
#define UTIL_UDIM_LAST_TILE 2000

int util_udim_compare_i32(const void *a, const void *b) {
	i32 x = *(i32 *)a;
	i32 y = *(i32 *)b;
	return (x > y) - (x < y);
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

// Per tile resolution, the configured one unless the atlas of several tiles would pass the max side
static i32 _util_udim_fit(i32 res) {
	if (util_udim_count() < 2) {
		return res;
	}
	i32 max = UTIL_UDIM_MAX_SIDE / util_udim_stride();
	return res < max ? res : max;
}

i32 util_udim_tile_res_x() {
	return _util_udim_fit(config_get_tile_res_x());
}

i32 util_udim_tile_res_y() {
	return _util_udim_fit(config_get_tile_res_y());
}

// The per tile resolution was lowered to fit the atlas
bool util_udim_clamped() {
	return util_udim_tile_res_x() < config_get_tile_res_x() || util_udim_tile_res_y() < config_get_tile_res_y();
}

// Sets the project tile list (copied, sorted, distinct, ids outside 1001..2000 dropped, NULL or empty gives {1001}), the layout is not changed
void util_udim_set_tiles(i32_array_t *ids) {
	i32_array_t *t = i32_array_create(0);
	for (i32 i = 0; ids != NULL && i < ids->length; ++i) {
		i32 id = ids->buffer[i];
		if (id >= UTIL_UDIM_FIRST_TILE && id <= UTIL_UDIM_LAST_TILE && i32_array_index_of(t, id) < 0) {
			i32_array_push(t, ids->buffer[i]);
		}
	}
	if (t->length == 0) {
		i32_array_push(t, 1001);
	}
	i32_array_sort(t, util_udim_compare_i32);
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

// Console message when the tile resolution was lowered to keep the atlas within the max side
void util_udim_warn_clamped() {
	if (util_udim_count() < 2 || !util_udim_clamped()) {
		return;
	}
	any_map_t *vars = any_map_create();
	any_map_set(vars, "tiles", i32_to_string(util_udim_count()));
	any_map_set(vars, "res", i32_to_string(util_udim_tile_res_x()));
	any_map_set(vars, "atlas", i32_to_string(config_get_texture_res_x()));
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
	util_udim_warn_clamped();
}

// Meshes just imported (tex tile-local, tile data or none for all in 1001) join the project layout.
// replace: the import replaced every mesh of the project (fresh import or Reimport Mesh), the list becomes exactly the tiles
// the imported meshes use, so tiles of the previous meshes do not linger. Otherwise (append) the tiles are added to the list.
// cleared: the replacing import also cleared the layers, they are resized instead of relaid out.
void util_udim_import_meshes(any_array_t *mds, bool replace, bool cleared) {
	bool fresh = replace && cleared;
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
	if (replace) {
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
		if (l0 != NULL && l0->texpaint != NULL && (l0->texpaint->width != config_get_texture_res_x() || l0->texpaint->height != config_get_texture_res_y())) {
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
		util_udim_warn_clamped();
	}
	else if (replace) {
		// Layers kept: tiles in both lists keep their pixels, tiles only the previous meshes used are dropped
		bool same = ids->length == util_udim_count();
		for (i32 i = 0; i < ids->length && same; ++i) {
			same = util_udim_slot(ids->buffer[i]) >= 0;
		}
		bool shared = false;
		for (i32 i = 0; i < ids->length && !shared; ++i) {
			shared = util_udim_slot(ids->buffer[i]) >= 0;
		}
		if (same) {
			for (i32 i = 0; i < mds->length; ++i) {
				util_udim_apply_layout(mds->buffer[i]);
			}
		}
		else if (!shared && ids->length == util_udim_count()) {
			// No tile in common and as many tiles (for example 1001 -> 1002 after Auto changed its decision): the tiles are
			// renamed slot by slot, the atlas layout and every pixel stay, instead of dropping all paint
			util_udim_set_tiles(ids);
			for (i32 i = 0; i < mds->length; ++i) {
				if (util_udim_active()) {
					util_udim_set_default(mds->buffer[i]);
				}
				util_udim_apply_layout(mds->buffer[i]);
			}
			if (!util_udim_active()) {
				for (i32 i = 0; i < mds->length; ++i) {
					util_udim_clear(mds->buffer[i]);
				}
			}
		}
		else {
			// Tiles only the previous meshes used are dropped with their pixels, say which
			char dropped[512] = "";
			for (i32 i = 0; i < util_udim_count(); ++i) {
				i32 len = (i32)strlen(dropped);
				if (i32_array_index_of(ids, util_udim_tile(i)) < 0 && len < (i32)sizeof(dropped) - 16) {
					snprintf(dropped + len, sizeof(dropped) - len, "%s%d", len == 0 ? "" : ", ", util_udim_tile(i));
				}
			}
			// Every mesh carries tile data before the relayout, else it would be derived from its tile-local uvs as if they were atlas uvs
			for (i32 i = 0; i < mds->length; ++i) {
				util_udim_set_default(mds->buffer[i]);
			}
			util_udim_relayout(ids);
			if (dropped[0] != '\0') {
				any_map_t *vars = any_map_create();
				any_map_set(vars, "tiles", dropped);
				console_info(vtr("Tiles {tiles} are not used by the imported meshes and were removed with their pixels", vars));
				map_free(vars);
			}
		}
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

// Whether the scanned file is imported with tiles (UDIM tiles import option).
// Auto: the uvs use a tile other than 1001 (several tiles, or one tile such as 1002) and no triangle crosses a tile border.
bool util_udim_scan_end() {
	udim_import_t mode     = g_context->udim_import;
	bool          not_1001 = util_udim_scan_multi || util_udim_scan_first > 0;
	bool          tiles    = mode == UDIM_IMPORT_ON || (mode == UDIM_IMPORT_AUTO && not_1001 && util_udim_scan_straddles == 0);
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
