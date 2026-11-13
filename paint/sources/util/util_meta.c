#include "../global.h"

// Metadata (meta_t): typed app keys (meta_key_t) of an item, layers keep their coverage here (slot_layer_t.meta, see util_udim.c).
// Keys are parallel key / value arrays per kind (bool, i32, f32, i32 array), bools are stored as 0 / 1. NULL arrays are empty.
// A NULL meta pointer on an item means no metadata. Runtime metas are owned by their item and never shared:
// copy them with meta_copy and free them with meta_free. All helpers accept NULL.
// Saved as the entries of project_t.metas by io/project_format.c, decoded entries point into the file blob,
// so copy them (meta_copy) and never free them.

meta_t *meta_create() {
	return calloc(1, sizeof(meta_t));
}

static i32_array_t *meta_copy_i32(i32_array_t *a) {
	return a != NULL && a->length > 0 ? i32_array_create_from_array(a) : NULL;
}

// Deep copy of the keys, category and index are left at 0. NULL gives an empty meta.
meta_t *meta_copy(meta_t *m) {
	meta_t *c = meta_create();
	if (m == NULL) {
		return c;
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
	return m == NULL || ((m->bool_keys == NULL || m->bool_keys->length == 0) && (m->i32_keys == NULL || m->i32_keys->length == 0) &&
	                     (m->f32_keys == NULL || m->f32_keys->length == 0) && (m->i32_array_keys == NULL || m->i32_array_keys->length == 0));
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
