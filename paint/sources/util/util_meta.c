
#include "../global.h"

// Metadata (meta_t): user tags and typed app keys (meta_key_t) of an item of a category (meta_category_t).
// Keys are parallel key / value arrays per kind (bool, i32, f32, i32 array), bools are stored as 0 / 1. NULL arrays are empty.
// The metadata lives on the runtime item (slot_layer_t, asset_t, slot_font_t, slot_sound_t, slot_material_t, slot_brush_t) so it
// follows deletes, reorders and duplicates; a NULL meta pointer means empty. Layers keep their coverage here (see util_udim.c).
// Meshes are paint objects (the Meshes tray lists g_project->_->paint_objects) and the engine type has no field for it: their
// metadata is kept here by paint_id, which is never reused. Runtime metas are owned by their item and never shared: copy them
// with meta_copy, layers free theirs with meta_free. All helpers accept NULL.
// Saved as the entries of project_t.metas by io/project_format.c, decoded entries point into the file blob,
// so copy them (meta_copy) and never free them.

static i32_array_t *meta_mesh_ids    = NULL; // paint_id of meta_mesh_values[i]
static any_array_t *meta_mesh_values = NULL;

// Type of an app key, unknown keys are kept as i32
i32 meta_key_type(i32 key) {
	switch (key) {
	case META_ALL_OBJECTS:
	case META_ALL_TILES:
		return MT_BOOL;
	case META_OBJECTS:
	case META_TILES:
		return MT_I32_ARRAY;
	case META_BAKE_TYPE:
	case META_ENCODING:
	case META_OBJECT:
	case META_TILE:
		return MT_I32;
	}
	return MT_I32;
}

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
	c->f32_keys    = meta_copy_i32(m->f32_keys);
	c->f32_values  = m->f32_values != NULL && m->f32_values->length > 0 ? f32_array_create_from_array(m->f32_values) : NULL;
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
	array_delete(m->tags);
	array_delete(m->bool_keys);
	array_delete(m->bool_values);
	array_delete(m->i32_keys);
	array_delete(m->i32_values);
	array_delete(m->f32_keys);
	array_delete(m->f32_values);
	for (i32 i = 0; m->i32_array_values != NULL && i < m->i32_array_values->length; ++i) {
		array_delete(m->i32_array_values->buffer[i]);
	}
	array_delete(m->i32_array_keys);
	array_delete(m->i32_array_values);
	free(m);
}

bool meta_is_empty(meta_t *m) {
	return m == NULL || ((m->tags == NULL || m->tags->length == 0) && (m->bool_keys == NULL || m->bool_keys->length == 0) &&
	                     (m->i32_keys == NULL || m->i32_keys->length == 0) && (m->f32_keys == NULL || m->f32_keys->length == 0) &&
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

f32 meta_get_f32(meta_t *m, i32 key, f32 def) {
	if (m == NULL) {
		return def;
	}
	i32 i = meta_index(m->f32_keys, m->f32_values != NULL ? m->f32_values->length : 0, key);
	return i >= 0 ? m->f32_values->buffer[i] : def;
}

void meta_set_f32(meta_t *m, i32 key, f32 v) {
	if (m == NULL) {
		return;
	}
	if (m->f32_keys == NULL || m->f32_values == NULL || m->f32_keys->length != m->f32_values->length) {
		m->f32_keys   = i32_array_create(0);
		m->f32_values = f32_array_create(0);
	}
	i32 i = meta_index(m->f32_keys, m->f32_values->length, key);
	if (i >= 0) {
		m->f32_values->buffer[i] = v;
		return;
	}
	i32_array_push(m->f32_keys, key);
	f32_array_push(m->f32_values, v);
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

static void meta_remove_entry(i32_array_t *keys, i32_array_t *values, f32_array_t *fvalues, i32 key) {
	i32 vl = values != NULL ? values->length : (fvalues != NULL ? fvalues->length : 0);
	i32 i  = meta_index(keys, vl, key);
	if (i < 0) {
		return;
	}
	for (i32 j = i; j + 1 < keys->length; ++j) {
		keys->buffer[j] = keys->buffer[j + 1];
	}
	keys->length--;
	for (i32 j = i; values != NULL && j + 1 < values->length; ++j) {
		values->buffer[j] = values->buffer[j + 1];
	}
	if (values != NULL) {
		values->length--;
	}
	for (i32 j = i; fvalues != NULL && j + 1 < fvalues->length; ++j) {
		fvalues->buffer[j] = fvalues->buffer[j + 1];
	}
	if (fvalues != NULL) {
		fvalues->length--;
	}
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
	meta_remove_entry(m->bool_keys, m->bool_values, NULL, key);
	meta_remove_entry(m->i32_keys, m->i32_values, NULL, key);
	meta_remove_entry(m->f32_keys, NULL, m->f32_values, key);
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
	return substring(s, start, end);
}

static i32 meta_tag_index(meta_t *m, char *tag) {
	char *t = to_lower_case(meta_trim(tag));
	for (i32 i = 0; m != NULL && m->tags != NULL && i < m->tags->length; ++i) {
		if (string_equals(to_lower_case(m->tags->buffer[i]), t)) {
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
	for (i32 j = i; j + 1 < m->tags->length; ++j) {
		m->tags->buffer[j] = m->tags->buffer[j + 1];
	}
	m->tags->length--;
}

// Mesh metadata by paint_id, NULL when none (or the id is not a paint object)
static meta_t **meta_mesh_slot(i32 paint_id, bool create) {
	if (paint_id <= 0) {
		return NULL;
	}
	for (i32 i = 0; meta_mesh_ids != NULL && i < meta_mesh_ids->length; ++i) {
		if (meta_mesh_ids->buffer[i] == paint_id) {
			return (meta_t **)&meta_mesh_values->buffer[i];
		}
	}
	if (!create) {
		return NULL;
	}
	if (meta_mesh_ids == NULL) {
		meta_mesh_ids    = i32_array_create(0);
		meta_mesh_values = any_array_create(0);
	}
	i32_array_push(meta_mesh_ids, paint_id);
	any_array_push(meta_mesh_values, NULL);
	return (meta_t **)&meta_mesh_values->buffer[meta_mesh_values->length - 1];
}

// Forget all mesh metadata, on project cleanup
void meta_mesh_reset() {
	array_delete(meta_mesh_ids);
	array_delete(meta_mesh_values);
	meta_mesh_ids    = NULL;
	meta_mesh_values = NULL;
}

// Items of a category, the Meshes tray lists the paint objects
i32 meta_count(i32 category) {
	switch (category) {
	case MC_TEXTURE:
		return g_project->_->assets->length;
	case MC_FONT:
		return g_project->_->fonts->length;
	case MC_SOUND:
		return g_project->_->sounds->length;
	case MC_MESH:
		return g_project->_->paint_objects != NULL ? g_project->_->paint_objects->length : 0;
	case MC_MATERIAL:
		return g_project->_->materials->length;
	case MC_BRUSH:
		return g_project->_->brushes->length;
	}
	return 0;
}

// Where the item keeps its metadata pointer, NULL when out of range
static meta_t **meta_slot(i32 category, i32 index, bool create) {
	if (index < 0 || index >= meta_count(category)) {
		return NULL;
	}
	switch (category) {
	case MC_TEXTURE:
		return &g_project->_->assets->buffer[index]->meta;
	case MC_FONT:
		return &g_project->_->fonts->buffer[index]->meta;
	case MC_SOUND:
		return &g_project->_->sounds->buffer[index]->meta;
	case MC_MESH:
		return meta_mesh_slot(g_project->_->paint_objects->buffer[index]->paint_id, create);
	case MC_MATERIAL:
		return &g_project->_->materials->buffer[index]->meta;
	case MC_BRUSH:
		return &g_project->_->brushes->buffer[index]->meta;
	}
	return NULL;
}

// Metadata of a runtime item, NULL when it has none or the index is out of range
meta_t *meta_peek(i32 category, i32 index) {
	meta_t **slot = meta_slot(category, index, false);
	return slot != NULL ? *slot : NULL;
}

// Metadata of a runtime item, created when missing, NULL when the index is out of range
meta_t *meta_get(i32 category, i32 index) {
	meta_t **slot = meta_slot(category, index, true);
	if (slot == NULL) {
		return NULL;
	}
	if (*slot == NULL) {
		*slot = meta_create();
	}
	return *slot;
}

// Replaces the metadata of a runtime item, takes ownership of m
void meta_set(i32 category, i32 index, meta_t *m) {
	meta_t **slot = meta_slot(category, index, m != NULL);
	if (slot != NULL) {
		*slot = m;
	}
}

// First item whose metadata has the i32 key set to value, -1 when none
i32 meta_find(i32 category, i32 key, i32 value) {
	i32 count = meta_count(category);
	for (i32 i = 0; i < count; ++i) {
		meta_t *m = meta_peek(category, i);
		i32     k = m != NULL ? meta_index(m->i32_keys, m->i32_values != NULL ? m->i32_values->length : 0, key) : -1;
		if (k >= 0 && m->i32_values->buffer[k] == value) {
			return i;
		}
	}
	return -1;
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
	case MC_MESH:
		name = g_project->_->paint_objects->buffer[index]->base->name;
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
