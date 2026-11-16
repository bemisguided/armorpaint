#include "../global.h"

// Metadata (meta_t): user tags and typed app keys (meta_key_t) of an item of a category (meta_category_t).
// Keys are parallel key / value arrays per kind (bool, i32, i32 array), bools are stored as 0 / 1. NULL arrays are empty.
// The metadata lives on the runtime item (slot_layer_t, asset_t, slot_font_t, slot_sound_t, slot_material_t, slot_brush_t) so it
// follows deletes, reorders and duplicates; a NULL meta pointer means empty. Layers keep their coverage here (see util_layer_coverage.c).
// All helpers accept NULL.
// Ownership: a live item owns its meta and frees it when it leaves its list (a duplicate gets a meta_copy, never a shared pointer;
// layers free theirs with the layer).
// A history step owns a private meta of its own or none: material and brush slots hand their meta over to the step when they are
// removed and take it back when the step puts the slot back, edits swap the two pointers (meta_swap), history_step_free frees what is left.
// Saved as the entries of project_t.metas by io/project_format.c, decoded entries point into the file blob, so copy them (meta_copy)
// and never free them.

meta_t *meta_create() {
	return calloc(1, sizeof(meta_t));
}

static i32_array_t *meta_copy_i32(i32_array_t *a) {
	return a != NULL && a->length > 0 ? i32_array_create_from_array(a) : NULL;
}

// Deep copy of the tags and keys, category and index are left at 0. NULL gives an empty meta.
meta_t *meta_copy(meta_t *m) {
	meta_t *c = meta_create();
	if (m == NULL) {
		return c;
	}
	if (m->tags != NULL && m->tags->length > 0) {
		c->tags = string_array_create(0);
		for (i32 i = 0; i < m->tags->length; ++i) {
			string_array_push(c->tags, string_copy(m->tags->buffer[i]));
		}
	}
	c->bool_keys   = meta_copy_i32(m->bool_keys);
	c->bool_values = meta_copy_i32(m->bool_values);
	c->i32_keys    = meta_copy_i32(m->i32_keys);
	c->i32_values  = meta_copy_i32(m->i32_values);
	if (m->i32_array_keys != NULL && m->i32_array_values != NULL && m->i32_array_keys->length > 0) {
		c->i32_array_keys   = i32_array_create_from_array(m->i32_array_keys);
		c->i32_array_values = any_array_create(0);
		for (i32 i = 0; i < m->i32_array_values->length; ++i) {
			i32_array_t *a = m->i32_array_values->buffer[i];
			any_array_push(c->i32_array_values, a != NULL ? i32_array_create_from_array(a) : i32_array_create(0));
		}
	}
	return c;
}

// Frees a meta made by meta_create or meta_copy, never one decoded from a file
void meta_free(meta_t *m) {
	if (m == NULL) {
		return;
	}
	for (i32 i = 0; m->tags != NULL && i < m->tags->length; ++i) {
		free(m->tags->buffer[i]);
	}
	array_delete(m->tags);
	array_delete(m->bool_keys);
	array_delete(m->bool_values);
	array_delete(m->i32_keys);
	array_delete(m->i32_values);
	for (i32 i = 0; m->i32_array_values != NULL && i < m->i32_array_values->length; ++i) {
		array_delete(m->i32_array_values->buffer[i]);
	}
	array_delete(m->i32_array_keys);
	array_delete(m->i32_array_values);
	free(m);
}

bool meta_is_empty(meta_t *m) {
	return m == NULL || ((m->tags == NULL || m->tags->length == 0) && (m->bool_keys == NULL || m->bool_keys->length == 0) &&
	                     (m->i32_keys == NULL || m->i32_keys->length == 0) &&
	                     (m->i32_array_keys == NULL || m->i32_array_keys->length == 0));
}

// Index of key in keys, only counting entries that have a value
static i32 meta_index(i32_array_t *keys, i32 values_length, i32 key) {
	for (i32 i = 0; keys != NULL && i < keys->length && i < values_length; ++i) {
		if (keys->buffer[i] == key) {
			return i;
		}
	}
	return -1;
}

static void meta_set_entry(i32_array_t **keys, i32_array_t **values, i32 key, i32 v) {
	if (*keys == NULL || *values == NULL || (*keys)->length != (*values)->length) { // Missing or broken pair, start over
		*keys   = i32_array_create(0);
		*values = i32_array_create(0);
	}
	i32 i = meta_index(*keys, (*values)->length, key);
	if (i >= 0) {
		(*values)->buffer[i] = v;
		return;
	}
	i32_array_push(*keys, key);
	i32_array_push(*values, v);
}

static i32 meta_pair_get(i32_array_t *keys, i32_array_t *values, i32 key, i32 def) {
	i32 i = meta_index(keys, values != NULL ? values->length : 0, key);
	return i >= 0 ? values->buffer[i] : def;
}

i32 meta_get_i32(meta_t *m, i32 key, i32 def) {
	return m != NULL ? meta_pair_get(m->i32_keys, m->i32_values, key, def) : def;
}

void meta_set_i32(meta_t *m, i32 key, i32 v) {
	if (m != NULL) {
		meta_set_entry(&m->i32_keys, &m->i32_values, key, v);
	}
}

bool meta_get_bool(meta_t *m, i32 key, bool def) {
	return m != NULL ? meta_pair_get(m->bool_keys, m->bool_values, key, def ? 1 : 0) != 0 : def;
}

void meta_set_bool(meta_t *m, i32 key, bool v) {
	if (m != NULL) {
		meta_set_entry(&m->bool_keys, &m->bool_values, key, v ? 1 : 0);
	}
}

// The i32 array of a key, NULL when the key is absent. The meta keeps ownership, the array may be changed in place.
i32_array_t *meta_get_i32_array(meta_t *m, i32 key) {
	if (m == NULL) {
		return NULL;
	}
	i32 i = meta_index(m->i32_array_keys, m->i32_array_values != NULL ? m->i32_array_values->length : 0, key);
	return i >= 0 ? m->i32_array_values->buffer[i] : NULL;
}

// Sets the i32 array of a key to a copy of count values, values may point into the current array of the key
void meta_set_i32_array(meta_t *m, i32 key, i32 *values, i32 count) {
	if (m == NULL) {
		return;
	}
	if (m->i32_array_keys == NULL || m->i32_array_values == NULL || m->i32_array_keys->length != m->i32_array_values->length) {
		m->i32_array_keys   = i32_array_create(0);
		m->i32_array_values = any_array_create(0);
	}
	i32_array_t *copy = i32_array_create_from_raw(values, values != NULL ? count : 0);
	i32          i    = meta_index(m->i32_array_keys, m->i32_array_values->length, key);
	if (i >= 0) {
		i32_array_t *old               = m->i32_array_values->buffer[i];
		m->i32_array_values->buffer[i] = copy;
		array_delete(old);
		return;
	}
	i32_array_push(m->i32_array_keys, key);
	any_array_push(m->i32_array_values, copy);
}

static void meta_remove_entry(i32_array_t *keys, i32_array_t *values, i32 key) {
	i32 i = meta_index(keys, values != NULL ? values->length : 0, key);
	if (i < 0) {
		return;
	}
	i32_array_splice(keys, i, 1);
	i32_array_splice(values, i, 1);
}

static void meta_remove_i32_array(meta_t *m, i32 key) {
	i32 i = meta_index(m->i32_array_keys, m->i32_array_values != NULL ? m->i32_array_values->length : 0, key);
	if (i < 0) {
		return;
	}
	array_delete(m->i32_array_values->buffer[i]);
	array_splice(m->i32_array_values, i, 1);
	i32_array_splice(m->i32_array_keys, i, 1);
}

// Removes the key of every kind
void meta_remove(meta_t *m, i32 key) {
	if (m == NULL) {
		return;
	}
	meta_remove_entry(m->bool_keys, m->bool_values, key);
	meta_remove_entry(m->i32_keys, m->i32_values, key);
	meta_remove_i32_array(m, key);
}

static char *meta_trim(char *s) {
	if (s == NULL) {
		return "";
	}
	i32 start = 0;
	i32 end   = string_length(s);
	while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) {
		start++;
	}
	while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' || s[end - 1] == '\r')) {
		end--;
	}
	char *r = string_tmp_alloc(end - start + 1);
	memcpy(r, s + start, end - start);
	return r;
}

static i32 meta_tag_index(meta_t *m, char *tag) {
	char *t = string_to_lower_tmp(meta_trim(tag));
	for (i32 i = 0; m != NULL && m->tags != NULL && i < m->tags->length; ++i) {
		if (string_equals(string_to_lower_tmp(m->tags->buffer[i]), t)) {
			return i;
		}
	}
	return -1;
}

bool meta_has_tag(meta_t *m, char *tag) {
	return meta_tag_index(m, tag) >= 0;
}

void meta_add_tag(meta_t *m, char *tag) {
	char *t = meta_trim(tag);
	if (m == NULL || string_length(t) == 0 || meta_tag_index(m, t) >= 0) {
		return;
	}
	if (m->tags == NULL) {
		m->tags = string_array_create(0);
	}
	string_array_push(m->tags, string_copy(t));
}

void meta_remove_tag(meta_t *m, char *tag) {
	i32 i = meta_tag_index(m, tag);
	if (i < 0) {
		return;
	}
	free(m->tags->buffer[i]);
	for (i32 j = i; j + 1 < m->tags->length; ++j) {
		m->tags->buffer[j] = m->tags->buffer[j + 1];
	}
	m->tags->length--;
}

// Items of a category
i32 meta_count(i32 category) {
	switch (category) {
	case MC_TEXTURE:
		return g_project->_->assets->length;
	case MC_FONT:
		return g_project->_->fonts->length;
	case MC_SOUND:
		return g_project->_->sounds->length;
	case MC_MATERIAL:
		return g_project->_->materials->length;
	case MC_BRUSH:
		return g_project->_->brushes->length;
	}
	return 0;
}

// Where the item keeps its metadata pointer, NULL when out of range
static meta_t **meta_slot(i32 category, i32 index) {
	if (index < 0 || index >= meta_count(category)) {
		return NULL;
	}
	switch (category) {
	case MC_TEXTURE:
		return &g_project->_->assets->buffer[index]->meta;
	case MC_FONT: // The built-in default font (index 0) is not saved, so it takes no metadata
		return index > 0 ? &g_project->_->fonts->buffer[index]->meta : NULL;
	case MC_SOUND:
		return &g_project->_->sounds->buffer[index]->meta;
	case MC_MATERIAL:
		return &g_project->_->materials->buffer[index]->meta;
	case MC_BRUSH:
		return &g_project->_->brushes->buffer[index]->meta;
	}
	return NULL;
}

// Metadata of a runtime item, NULL when it has none or the index is out of range
meta_t *meta_peek(i32 category, i32 index) {
	meta_t **slot = meta_slot(category, index);
	return slot != NULL ? *slot : NULL;
}

// Metadata of a runtime item, created when missing, NULL when the index is out of range
meta_t *meta_get(i32 category, i32 index) {
	meta_t **slot = meta_slot(category, index);
	if (slot == NULL) {
		return NULL;
	}
	if (*slot == NULL) {
		*slot = meta_create();
	}
	return *slot;
}

// Exchanges the metadata of a runtime item with *other (either may be NULL), no copy
void meta_swap(i32 category, i32 index, meta_t **other) {
	meta_t **slot = meta_slot(category, index);
	if (slot != NULL) {
		meta_t *t = *slot;
		*slot     = *other;
		*other    = t;
	}
}

// Name of an item, "" when out of range
char *meta_item_name(i32 category, i32 index) {
	if (index < 0 || index >= meta_count(category)) {
		return "";
	}
	char *name = NULL;
	switch (category) {
	case MC_TEXTURE:
		name = g_project->_->assets->buffer[index]->name;
		break;
	case MC_FONT:
		name = g_project->_->fonts->buffer[index]->name;
		break;
	case MC_SOUND:
		name = g_project->_->sounds->buffer[index]->name;
		break;
	case MC_MATERIAL:
		name = g_project->_->materials->buffer[index]->canvas != NULL ? g_project->_->materials->buffer[index]->canvas->name : NULL;
		break;
	case MC_BRUSH:
		name = g_project->_->brushes->buffer[index]->canvas != NULL ? g_project->_->brushes->buffer[index]->canvas->name : NULL;
		break;
	}
	return name != NULL ? name : "";
}

// The item (asset_t, slot_font_t, slot_sound_t, slot_material_t, slot_brush_t) at index of a category, NULL when out of range
void *meta_item(i32 category, i32 index) {
	if (index < 0 || index >= meta_count(category)) {
		return NULL;
	}
	switch (category) {
	case MC_TEXTURE:
		return g_project->_->assets->buffer[index];
	case MC_FONT:
		return g_project->_->fonts->buffer[index];
	case MC_SOUND:
		return g_project->_->sounds->buffer[index];
	case MC_MATERIAL:
		return g_project->_->materials->buffer[index];
	case MC_BRUSH:
		return g_project->_->brushes->buffer[index];
	}
	return NULL;
}

// Index of an item in the live list of a category, -1 when it is not there any more (deleted)
i32 meta_item_index(i32 category, void *item) {
	if (item == NULL) {
		return -1;
	}
	i32 count = meta_count(category);
	for (i32 i = 0; i < count; ++i) {
		if (meta_item(category, i) == item) {
			return i;
		}
	}
	return -1;
}

// Built-in bake types in display order (after None)
static i32 asset_bake_types[] = {BTP_AO,         BTP_CURVATURE, BTP_THICKNESS, BTP_HEIGHT,       BTP_NORMAL,    BTP_NORMAL_OBJECT, BTP_BENT_NORMAL,
                                 BTP_DERIVATIVE, BTP_POSITION,  BTP_TEXCOORD,  BTP_MATERIAL_ID,  BTP_OBJECT_ID, BTP_VERTEX_COLOR,  BTP_LIGHTMAP,  BTP_CAVITY};

i32 asset_bake_type_count() {
	return sizeof(asset_bake_types) / sizeof(asset_bake_types[0]);
}

// Built-in bake type at display position i, BTP_NONE when out of range
i32 asset_bake_type_at(i32 i) {
	return i >= 0 && i < asset_bake_type_count() ? asset_bake_types[i] : BTP_NONE;
}

// Encoding a bake of this type is usually read with, unknown types have none
i32 asset_bake_default_encoding(i32 btp) {
	switch (btp) {
	case BTP_AO:
	case BTP_CURVATURE:
	case BTP_THICKNESS:
	case BTP_CAVITY:
	case BTP_HEIGHT:
	case BTP_DERIVATIVE:
	case BTP_POSITION:
	case BTP_TEXCOORD:
		return AE_LINEAR;
	case BTP_NORMAL:
	case BTP_NORMAL_OBJECT:
	case BTP_BENT_NORMAL:
		return AE_NORMAL;
	case BTP_MATERIAL_ID:
	case BTP_OBJECT_ID:
	case BTP_VERTEX_COLOR:
	case BTP_LIGHTMAP:
		return AE_COLOR;
	}
	return AE_NONE;
}

char *asset_unknown_label(i32 id) {
	any_map_t *vars = any_map_create();
	any_map_set(vars, "n", string_tmp("%d", id));
	char *s = vtr("Unknown ({n})", vars);
	map_free(vars);
	return s;
}

char *asset_bake_type_label(i32 btp) {
	switch (btp) {
	case BTP_NONE:
		return tr("None");
	case BTP_AO:
		return tr("Ambient Occlusion");
	case BTP_CURVATURE:
		return tr("Curvature");
	case BTP_NORMAL:
		return tr("Normal");
	case BTP_NORMAL_OBJECT:
		return tr("Object Normal");
	case BTP_HEIGHT:
		return tr("Height");
	case BTP_DERIVATIVE:
		return tr("Derivative");
	case BTP_POSITION:
		return tr("Position");
	case BTP_TEXCOORD:
		return tr("TexCoord");
	case BTP_MATERIAL_ID:
		return tr("Material ID");
	case BTP_OBJECT_ID:
		return tr("Object ID");
	case BTP_VERTEX_COLOR:
		return tr("Vertex Color");
	case BTP_LIGHTMAP:
		return tr("Lightmap");
	case BTP_BENT_NORMAL:
		return tr("Bent Normal");
	case BTP_THICKNESS:
		return tr("Thickness");
	case BTP_CAVITY:
		return tr("Cavity");
	}
	return asset_unknown_label(btp);
}

char *asset_encoding_label(i32 enc) {
	switch (enc) {
	case AE_NONE:
		return tr("None");
	case AE_COLOR:
		return tr("Color");
	case AE_LINEAR:
		return tr("Linear");
	case AE_NORMAL:
		return tr("Normal");
	}
	return asset_unknown_label(enc);
}

// Sets the bake properties, object (paint_id) or tile -1 = any (key removed). BTP_NONE clears them all.
void meta_set_bake(meta_t *m, i32 btp, i32 enc, i32 object_paint_id, i32 tile) {
	if (m == NULL) {
		return;
	}
	if (btp == BTP_NONE) {
		meta_clear_bake(m);
		return;
	}
	meta_set_i32(m, META_BAKE_TYPE, btp);
	meta_set_i32(m, META_ENCODING, enc);
	object_paint_id >= 0 ? meta_set_i32(m, META_OBJECT, object_paint_id) : meta_remove(m, META_OBJECT);
	tile >= 0 ? meta_set_i32(m, META_TILE, tile) : meta_remove(m, META_TILE);
}

void meta_clear_bake(meta_t *m) {
	meta_remove(m, META_BAKE_TYPE);
	meta_remove(m, META_ENCODING);
	meta_remove(m, META_OBJECT);
	meta_remove(m, META_TILE);
}

// Renames a material, the name gets the .001 style suffix when taken. An empty or whitespace-only name is rejected and the old name stays.
// Returns true when the name changed.
bool asset_material_rename(slot_material_t *m, char *name) {
	char *n = meta_trim(name);
	if (m == NULL || string_equals(n, "")) {
		return false;
	}
	n = slot_material_unique_name(m->canvas, n);
	if (string_equals(m->canvas->name, n)) {
		return false;
	}
	m->canvas->name = string_copy(n);
	return true;
}

// Renames a brush, same rules as asset_material_rename
bool asset_brush_rename(slot_brush_t *b, char *name) {
	char *n = meta_trim(name);
	if (b == NULL || string_equals(n, "")) {
		return false;
	}
	n = slot_brush_unique_name(b->canvas, n);
	if (string_equals(b->canvas->name, n)) {
		return false;
	}
	b->canvas->name = string_copy(n);
	return true;
}

// Renames the material or brush at index of a category (MC_MATERIAL, MC_BRUSH) and pushes one undo step.
// No step when the name did not change. Returns true when the name changed.
bool asset_rename(i32 category, i32 index, char *name) {
	if (category == MC_MATERIAL && index >= 0 && index < g_project->_->materials->length) {
		slot_material_t *m    = g_project->_->materials->buffer[index];
		char            *prev = string_copy(m->canvas->name);
		if (asset_material_rename(m, name)) {
			history_rename_asset(MC_MATERIAL, index, prev);
			return true;
		}
	}
	else if (category == MC_BRUSH && index >= 0 && index < g_project->_->brushes->length) {
		slot_brush_t *b    = g_project->_->brushes->buffer[index];
		char         *prev = string_copy(b->canvas->name);
		if (asset_brush_rename(b, name)) {
			history_rename_asset(MC_BRUSH, index, prev);
			return true;
		}
	}
	return false;
}
