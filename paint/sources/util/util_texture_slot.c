
#include "../global.h"

// Hidden atlas of the Texture Slot node (interim, see notes/PAINT_PATH.md verdict B).
// A fill layer is one draw of the merged mesh with the atlas uv as position, so the (object, tile) pair is not known when
// the texture is bound. Until the draw is split per pair, the textures of one bake type are composed into one internal
// render target of the project atlas layout (one slot per udim tile, a project without tiles has the single slot 1001).
// Slot images are added with additive blending: per object images are black outside their uv islands, islands are disjoint.
// Per (object, tile) the exact image wins over an object or tile "any" one (META_OBJECT / META_TILE absent).
// Built from an update (never inside a draw) when the signature of the matching textures, the tile list or the size changes.

#define TEXTURE_SLOT_MAX 32

typedef struct texture_slot_entry {
	i32            btp;
	gpu_texture_t *atlas; // NULL when no texture has this bake type
	u64            sig;   // 0 = no matching texture
} texture_slot_entry_t;

static texture_slot_entry_t texture_slot_entries[TEXTURE_SLOT_MAX];
static i32                  texture_slot_count       = 0;
static bool                 texture_slot_pending     = false; // Atlas changed, fill layers and previews still show the old one
static gpu_pipeline_t      *texture_slot_pipe        = NULL;
static gpu_texture_t       *texture_slot_flat_normal = NULL;

static texture_slot_entry_t *texture_slot_entry(i32 btp, bool create) {
	for (i32 i = 0; i < texture_slot_count; ++i) {
		if (texture_slot_entries[i].btp == btp) {
			return &texture_slot_entries[i];
		}
	}
	if (!create || texture_slot_count >= TEXTURE_SLOT_MAX) {
		return NULL;
	}
	texture_slot_entry_t *e = &texture_slot_entries[texture_slot_count++];
	e->btp                  = btp;
	e->atlas                = NULL;
	e->sig                  = 0;
	return e;
}

// A Texture Slot node of this bake type was parsed or bound, its atlas is kept up to date from now on
void texture_slot_want(i32 btp) {
	texture_slot_entry(btp, true);
}

static u64 texture_slot_hash(u64 h, i32 v) {
	return (h ^ (u64)(u32)v) * 1099511628211ULL;
}

static u64 texture_slot_hash_ptr(u64 h, void *p) {
	u64 v = (u64)(uintptr_t)p;
	return texture_slot_hash(texture_slot_hash(h, (i32)(v & 0xffffffff)), (i32)(v >> 32));
}

// Textures of the bake type with an image
static bool texture_slot_matches(asset_t *a, i32 btp) {
	return a->image != NULL && a->meta != NULL && meta_get_i32(a->meta, META_BAKE_TYPE, BTP_NONE) == btp;
}

// Signature of everything the atlas of a bake type depends on, 0 when no texture has the bake type
static u64 texture_slot_signature(i32 btp) {
	u64 h       = 14695981039346656037ULL;
	i32 matched = 0;
	for (i32 i = 0; i < g_project->_->assets->length; ++i) {
		asset_t *a = g_project->_->assets->buffer[i];
		if (!texture_slot_matches(a, btp)) {
			continue;
		}
		matched++;
		h = texture_slot_hash(h, i);
		h = texture_slot_hash_ptr(h, a);
		h = texture_slot_hash_ptr(h, a->image);
		h = texture_slot_hash(h, a->image->width);
		h = texture_slot_hash(h, a->image->height);
		h = texture_slot_hash(h, meta_get_i32(a->meta, META_BAKE_TYPE, BTP_NONE));
		h = texture_slot_hash(h, meta_get_i32(a->meta, META_ENCODING, AE_NONE));
		h = texture_slot_hash(h, meta_get_i32(a->meta, META_OBJECT, -1));
		h = texture_slot_hash(h, meta_get_i32(a->meta, META_TILE, -1));
	}
	if (matched == 0) {
		return 0;
	}
	i32 count = util_udim_count();
	h         = texture_slot_hash(h, count);
	for (i32 s = 0; s < count; ++s) {
		h = texture_slot_hash(h, util_udim_tile(s));
	}
	h = texture_slot_hash(h, util_udim_res_x());
	h = texture_slot_hash(h, util_udim_res_y());
	return h != 0 ? h : 1;
}

// Textures drawn into a tile: tile exact wins over tile any per object, the first of equal ones is used,
// object any textures are dropped when the tile has per object ones (a texture can not tell the islands apart)
static i32_array_t *texture_slot_select(i32 btp, i32 tile) {
	i32_array_t *cand = i32_array_create(0);
	for (i32 i = 0; i < g_project->_->assets->length; ++i) {
		asset_t *a = g_project->_->assets->buffer[i];
		if (!texture_slot_matches(a, btp)) {
			continue;
		}
		i32 t = meta_get_i32(a->meta, META_TILE, -1);
		if (t == tile || t == -1) {
			i32_array_push(cand, i);
		}
	}
	i32_array_t *keep = i32_array_create(0);
	bool         per_object = false;
	for (i32 k = 0; k < cand->length; ++k) {
		meta_t       *mk   = g_project->_->assets->buffer[cand->buffer[k]]->meta;
		i32           ok   = meta_get_i32(mk, META_OBJECT, -1);
		i32           tk   = meta_get_i32(mk, META_TILE, -1);
		bool          drop = false;
		for (i32 j = 0; j < cand->length && !drop; ++j) {
			if (j == k) {
				continue;
			}
			meta_t *mj = g_project->_->assets->buffer[cand->buffer[j]]->meta;
			if (meta_get_i32(mj, META_OBJECT, -1) != ok) {
				continue;
			}
			i32 tj = meta_get_i32(mj, META_TILE, -1);
			drop   = (tk == -1 && tj == tile) || (tk == tj && j < k);
		}
		if (!drop) {
			i32_array_push(keep, cand->buffer[k]);
			per_object = per_object || ok != -1;
		}
	}
	i32_array_t *out = i32_array_create(0);
	for (i32 k = 0; k < keep->length; ++k) {
		meta_t *m = g_project->_->assets->buffer[keep->buffer[k]]->meta;
		if (!per_object || meta_get_i32(m, META_OBJECT, -1) != -1) {
			i32_array_push(out, keep->buffer[k]);
		}
	}
	array_free(cand);
	free(cand);
	array_free(keep);
	free(keep);
	return out;
}

// Copy pipeline of the layer copy shaders with additive blending
static gpu_pipeline_t *texture_slot_get_pipe() {
	if (texture_slot_pipe == NULL) {
		gpu_pipeline_t *p = gpu_create_pipeline();
		p->vertex_shader   = sys_get_shader("layer_copy.vert");
		p->fragment_shader = sys_get_shader("layer_copy.frag");
		gpu_vertex_structure_t *vs = ALLOC_INIT(gpu_vertex_structure_t, {0});
		gpu_vertex_structure_add(vs, "pos", GPU_VERTEX_DATA_F32_2X);
		gpu_vertex_structure_add(vs, "tex", GPU_VERTEX_DATA_F32_2X);
		gpu_vertex_structure_add(vs, "col", GPU_VERTEX_DATA_F32_4X);
		p->input_layout            = vs;
		p->blend_source            = GPU_BLEND_ONE;
		p->blend_destination       = GPU_BLEND_ONE;
		p->alpha_blend_source      = GPU_BLEND_ONE;
		p->alpha_blend_destination = GPU_BLEND_ONE;
		gpu_pipeline_compile(p);
		texture_slot_pipe = p;
	}
	return texture_slot_pipe;
}

// Draws every slot of the atlas, black where nothing is drawn
static void texture_slot_build(texture_slot_entry_t *e) {
	i32 w = util_udim_res_x();
	i32 h = util_udim_res_y();
	if (e->atlas != NULL && (e->atlas->width != w || e->atlas->height != h)) {
		gpu_delete_texture(e->atlas);
		e->atlas = NULL;
	}
	if (e->atlas == NULL) {
		e->atlas = gpu_create_render_target(w, h, GPU_TEXTURE_FORMAT_RGBA32);
	}
	i32 stride = util_udim_stride();
	i32 tw     = util_udim_tile_res_x();
	i32 th     = util_udim_tile_res_y();
	draw_begin(e->atlas, true, 0xff000000);
	draw_set_pipeline(texture_slot_get_pipe());
	for (i32 s = 0; s < util_udim_count(); ++s) {
		i32_array_t *sel = texture_slot_select(e->btp, util_udim_tile(s));
		for (i32 k = 0; k < sel->length; ++k) {
			asset_t *a = g_project->_->assets->buffer[sel->buffer[k]];
			draw_scaled_image(a->image, (s % stride) * tw, (s / stride) * th, tw, th);
		}
		array_free(sel);
		free(sel);
	}
	draw_set_pipeline(NULL);
	draw_end();
}

static void texture_slot_invalidate() {
	make_material_parse_paint_material(true);
	if (layers_is_fill_material()) {
		layers_update_fill_layers();
	}
	if (layers_is_path_material()) {
		layers_update_path_layers();
	}
	util_render_make_material_preview();
	ui_dock_redraw(TAB_AREA_SIDEBAR1);
}

// Per frame from base_update: rebuilds the atlases whose textures changed, then refreshes the layers and previews once.
// One check when no Texture Slot node was ever parsed.
void texture_slot_update() {
	if (texture_slot_count == 0 || gpu_in_use || _draw_current != NULL) {
		return;
	}
	for (i32 i = 0; i < texture_slot_count; ++i) {
		texture_slot_entry_t *e   = &texture_slot_entries[i];
		u64                   sig = texture_slot_signature(e->btp);
		if (sig == e->sig) {
			continue;
		}
		e->sig = sig;
		if (sig != 0) {
			texture_slot_build(e);
		}
		else if (e->atlas != NULL) {
			gpu_delete_texture(e->atlas);
			e->atlas = NULL;
		}
		texture_slot_pending = true;
	}
	// Wait for a running bake, its layer textures are swapped (pdirty is not checked, it stays set while no stroke runs)
	if (texture_slot_pending && !bake_texture_node_baking && !render_path_paint_baking) {
		texture_slot_pending = false;
		texture_slot_invalidate();
	}
}

// The atlas of a bake type, NULL when no texture has it (or it is not built yet)
gpu_texture_t *texture_slot_atlas(i32 btp) {
	texture_slot_entry_t *e = texture_slot_entry(btp, true);
	return e != NULL ? e->atlas : NULL;
}

// Texture bound to _texslot_<btp>: the atlas, else black (flat normal for the Normal encoding)
gpu_texture_t *texture_slot_resolve(i32 btp) {
	gpu_texture_t *atlas = texture_slot_atlas(btp);
	if (atlas != NULL) {
		return atlas;
	}
	if (asset_bake_default_encoding(btp) == AE_NORMAL) {
		if (texture_slot_flat_normal == NULL) {
			buffer_t *b = buffer_create(4);
			b->buffer[0] = 128;
			b->buffer[1] = 128;
			b->buffer[2] = 255;
			b->buffer[3] = 255;
			texture_slot_flat_normal = gpu_create_texture_from_bytes(b, 1, 1, GPU_TEXTURE_FORMAT_RGBA32);
			array_free(b);
			free(b);
		}
		return texture_slot_flat_normal;
	}
	render_target_t *rt = any_map_get(render_path_render_targets, "empty_black");
	return rt->_image;
}

// True while an atlas changed and the layers and previews are not refreshed yet
bool texture_slot_changed() {
	return texture_slot_pending;
}
