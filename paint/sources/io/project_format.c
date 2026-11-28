#include "../global.h"

// Save and load code of the asset metadata in project files.
//
// Version: metas came with manifest_version_project "24" (globals.h). Versions are compared by exact string, older files load
// through the import_arm_from_version_N converters of import_legacy.c and have no metadata.
// Projects decode by field position (armpack), so these types.h fields must stay in the order written here:
// - project_t: metas (after mesh_groups, before _)
// - meta_t:    category, index, tags, i32_keys, i32_values (the entry maps of metas, every field written in this order)
// History steps are not saved.
// Metadata (meta_t, see util_meta.c): one list of sparse entries, one per item that has metadata. category says which list
// the item is in, index its position there when the project was saved: textures in assets, fonts in font_assets (the default font
// is not saved, so runtime font index + 1), sounds in sound_assets, materials in material_nodes and brushes in brush_nodes.
// Save rebuilds the list from the live items, load copies each entry onto its item and ignores entries of unknown categories
// or out of range indices. Unknown key ids are kept as they are, tags are plain strings.
// An empty list is written as an empty array. Entry maps always list every meta_t field in order (armpack sizes an array of
// maps from its first element), NULL arrays encode as null.

static meta_t_array_t *project_format_saved_metas = NULL; // List project_t.metas points to since the last save

// One metas entry, fields in declaration order
static void project_format_encode_meta(meta_t *m) {
	armpack_encode_map(5);
	armpack_encode_string("category");
	armpack_encode_i32(m->category);
	armpack_encode_string("index");
	armpack_encode_i32(m->index);
	armpack_encode_string("tags");
	armpack_encode_array_string(m->tags);
	armpack_encode_string("i32_keys");
	armpack_encode_array_i32(m->i32_keys);
	armpack_encode_string("i32_values");
	armpack_encode_array_i32(m->i32_values);
}

// util_encode_project, after mesh_groups
void project_format_encode_project(project_t *raw) {
	armpack_encode_string("metas");
	armpack_encode_array(raw->metas != NULL ? raw->metas->length : 0);
	for (i32 i = 0; raw->metas != NULL && i < raw->metas->length; ++i) {
		project_format_encode_meta(raw->metas->buffer[i]);
	}
}

// Upper bound of the bytes project_format_encode_project writes
i32 project_format_encode_size(project_t *raw) {
	i32 size = 64;
	for (i32 i = 0; raw->metas != NULL && i < raw->metas->length; ++i) {
		meta_t *m = raw->metas->buffer[i];
		size += 256; // Map, keys and array headers
		for (i32 j = 0; m->tags != NULL && j < m->tags->length; ++j) {
			size += 8 + string_length(m->tags->buffer[j]);
		}
		size += 8 * (m->i32_keys != NULL ? m->i32_keys->length : 0);
	}
	return size;
}

// Adds the metadata of an item to a metas list being saved, nothing when it has none. The entry is the item's own meta with
// category and index set (the encoder only reads it).
void project_format_add_entry(meta_t_array_t *metas, meta_t *m, i32 category, i32 index) {
	if (meta_is_empty(m)) {
		return;
	}
	m->category = category;
	m->index    = index;
	any_array_push(metas, m);
}

// Before util_encode_project: the metas list of the live items, one entry per item that has metadata
void project_format_save_prep(project_t *raw) {
	array_delete(project_format_saved_metas);
	project_format_saved_metas = any_array_create(0);
	for (i32 category = 0; category < MC_COUNT; ++category) {
		i32 first = category == MC_FONT ? 1 : 0; // The default font is not saved, saved font i is runtime font i + 1
		for (i32 i = first; i < meta_count(category); ++i) {
			project_format_add_entry(project_format_saved_metas, meta_peek(category, i), category, i - first);
		}
	}
	raw->metas = project_format_saved_metas;
}

// Load, metadata of item index of a category as saved (font index is the font_assets one), a copy the caller owns,
// NULL when the item has none (decoded entries live in the file blob)
meta_t *project_format_load_meta(project_t *raw, i32 category, i32 index) {
	for (i32 i = 0; raw->metas != NULL && i < raw->metas->length; ++i) {
		meta_t *m = raw->metas->buffer[i];
		if (m->category == category && m->index == index && !meta_is_empty(m)) {
			return meta_copy(m);
		}
	}
	return NULL;
}
