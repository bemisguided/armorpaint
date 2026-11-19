
#include "../global.h"

// Coverage texture of the sculpt displacement pass, built from the sculpt texel layout of make_sculpt.c (included after it)
static gpu_texture_t *sculpt_coverage_texture     = NULL; // Per texel 1 where the active layer covers the vertex
static u8            *sculpt_coverage_key         = NULL; // Per object covered flags the texture was built from
static i32            sculpt_coverage_key_version = -1;
static i32            sculpt_coverage_key_width   = 0;
static i32            sculpt_coverage_key_length  = 0;  // Object flags, then per project tile its id (4 bytes) and covered flag
static i32            sculpt_coverage_key_mesh    = -1; // util_mesh_version when tiles were read, -1 without the tile test

// Covered texels of one object whose vertices are on several tiles, a texel is covered when one of its vertices is on a covered tile
static void sculpt_coverage_tiles(u8 *texels, i32 o, i32 voff, u8 *tile_key, i32 slots) {
	mesh_object_t *p  = g_project->_->paint_objects->buffer[o];
	mesh_data_t   *md = p->data;
	i32            n  = md->vertex_arrays->buffer[0]->values->length / 4;
	util_udim_ensure(md);
	bool has     = util_udim_has_tiles(md) && md->_->udim_tile->length == n;
	i32  last    = -1;
	bool covered = false;
	for (i32 i = 0; i < n && voff + i < sculpt_vertex_count; ++i) {
		i32 tile = has ? 1001 + md->_->udim_tile->buffer[i] : util_udim_tile(0);
		if (tile != last) {
			i32 slot = util_udim_slot(tile);
			covered  = slot >= 0 && slot < slots && tile_key[slot * 5 + 4];
			last     = tile;
		}
		i32 t = sculpt_remap[voff + i];
		if (covered && t >= sculpt_object_texel_offsets[o] && t < sculpt_object_texel_offsets[o + 1]) {
			texels[t] = 255;
		}
	}
}

// Rebuild the active layer's coverage texels when the covered objects or tiles, the texel layout, the tile data or the width changed
void sculpt_update_coverage_texture() {
	slot_layer_t *l     = g_context->layer;
	i32           count = sculpt_object_count;
	i32           w     = config_get_tile_res_x();
	// Per vertex tiles are read only when the layer leaves out one of several tiles
	bool tiles  = layer_tiles_tested(l);
	bool tile   = tiles || layer_covers_tile(l, util_udim_tile(0));
	i32  slots  = tiles ? util_udim_count() : 0;
	i32  length = count + slots * 5;
	u8  *key    = malloc(length > 0 ? length : 1);
	for (i32 o = 0; o < count; ++o) {
		mesh_object_t *p = o < g_project->_->paint_objects->length ? g_project->_->paint_objects->buffer[o] : NULL;
		key[o]           = tile && p != NULL && layer_covers_object(l, p);
	}
	for (i32 s = 0; s < slots; ++s) {
		i32 id = util_udim_tile(s);
		memcpy(key + count + s * 5, &id, 4);
		key[count + s * 5 + 4] = layer_covers_tile(l, id);
	}
	i32 mesh = tiles ? util_mesh_version : -1;
	if (sculpt_coverage_texture != NULL && sculpt_coverage_key_version == sculpt_weld_version && sculpt_coverage_key_width == w &&
	    sculpt_coverage_key_length == length && sculpt_coverage_key_mesh == mesh && memcmp(key, sculpt_coverage_key, length) == 0) {
		free(key);
		return;
	}
	free(sculpt_coverage_key);
	sculpt_coverage_key         = key;
	sculpt_coverage_key_version = sculpt_weld_version;
	sculpt_coverage_key_width   = w;
	sculpt_coverage_key_length  = length;
	sculpt_coverage_key_mesh    = mesh;

	i32       h = sculpt_texel_count > 0 ? (sculpt_texel_count + w - 1) / w : 1;
	buffer_t *b = buffer_create(w * h);
	memset(b->buffer, 0, w * h);
	i32 voff = 0;
	for (i32 o = 0; o < count; ++o) {
		if (key[o] && tiles) {
			sculpt_coverage_tiles(b->buffer, o, voff, key + count, slots);
		}
		else if (key[o]) {
			memset(b->buffer + sculpt_object_texel_offsets[o], 255, sculpt_object_texel_offsets[o + 1] - sculpt_object_texel_offsets[o]);
		}
		voff += o < g_project->_->paint_objects->length ? g_project->_->paint_objects->buffer[o]->data->vertex_arrays->buffer[0]->values->length / 4 : 0;
	}
	sculpt_coverage_texture = sculpt_create_index_texture(b, w, h, GPU_TEXTURE_FORMAT_R8, sculpt_coverage_texture);
}

gpu_texture_t *sculpt_get_coverage_texture() {
	return sculpt_coverage_texture;
}
