
#include "../global.h"

// Tags of assets (meta_t tags), shared by the Textures, Materials, Brushes, Fonts and Sounds trays:
// the tag filter combo of a tray. The tags of an item are edited in the Details dialog (ui_asset_details.c).

static char *ui_asset_tags_selected[MC_COUNT]; // Selected filter tag per category, NULL = All

static char *ui_asset_tags_selected_lower[MC_COUNT]; // The selected filter tag in lower case (owned), compared by ui_asset_tags_visible

// Selects the filter tag of a category, NULL = All
static void ui_asset_tags_select(i32 category, char *tag) {
	ui_asset_tags_selected[category] = tag != NULL ? string_copy(tag) : NULL;
	free(ui_asset_tags_selected_lower[category]);
	ui_asset_tags_selected_lower[category] = NULL;
	if (tag != NULL) {
		char *lower = malloc(strlen(tag) + 1);
		for (i32 i = 0; tag[i] != 0; ++i) {
			lower[i] = (char)tolower((unsigned char)tag[i]);
		}
		lower[strlen(tag)]                     = 0;
		ui_asset_tags_selected_lower[category] = lower;
	}
}

// Case-insensitive compare of a tag with a lower case text, no allocation
static bool ui_asset_tags_equals_lower(char *tag, char *lower) {
	for (; *tag != 0 && *lower != 0; ++tag, ++lower) {
		if ((char)tolower((unsigned char)*tag) != *lower) {
			return false;
		}
	}
	return *tag == *lower;
}

// Hash of what the tag lists of metadata depend on, without reading a string: the metadata and tag array pointers, the number of tags
// and the pointer of every tag. A tag added, removed or replaced, a metadata set, replaced or freed (undo, import, duplicate) changes it.
u64 ui_asset_tags_hash(meta_t *m, u64 h) {
	h = (h ^ (u64)(uintptr_t)m) * 0x100000001b3ull;
	if (m != NULL && m->tags != NULL) {
		h = (h ^ (u64)(uintptr_t)m->tags) * 0x100000001b3ull;
		h = (h ^ (u64)m->tags->length) * 0x100000001b3ull;
		for (i32 j = 0; j < m->tags->length; ++j) {
			h = (h ^ (u64)(uintptr_t)m->tags->buffer[j]) * 0x100000001b3ull;
		}
	}
	return h;
}

// Sorted unique (case-insensitive, first spelling) tags in use in a category. Cached per category: rebuilt only when the hash of the
// metadata of its items changes (checked once per frame), so a tray with many items costs nothing while no tag is edited.
// The cache owns copies of the strings. Never delete the result.
static string_array_t *ui_asset_tags_cache[MC_COUNT];
static u64             ui_asset_tags_cache_hash[MC_COUNT];
static i32             ui_asset_tags_cache_frame[MC_COUNT];
static bool            ui_asset_tags_cache_valid[MC_COUNT];

static string_array_t *ui_asset_tags_in_use(i32 category) {
	if (ui_asset_tags_cache_valid[category] && ui_asset_tags_cache_frame[category] == g_context->frame) {
		return ui_asset_tags_cache[category];
	}
	i32 count = meta_count(category);
	u64 hash  = 0xcbf29ce484222325ull ^ (u64)count;
	for (i32 i = 0; i < count; ++i) {
		hash = ui_asset_tags_hash(meta_peek(category, i), hash);
	}
	ui_asset_tags_cache_frame[category] = g_context->frame;
	if (ui_asset_tags_cache_valid[category] && ui_asset_tags_cache_hash[category] == hash) {
		return ui_asset_tags_cache[category];
	}

	string_array_t *tags = ui_asset_tags_cache[category];
	for (i32 i = 0; tags != NULL && i < tags->length; ++i) {
		free(tags->buffer[i]);
	}
	array_delete(tags);
	tags                  = string_array_create(0);
	string_array_t *lower = string_array_create(0);
	for (i32 i = 0; i < count; ++i) {
		meta_t *m = meta_peek(category, i);
		for (i32 j = 0; m != NULL && m->tags != NULL && j < m->tags->length; ++j) {
			char *l = string_to_lower_tmp(m->tags->buffer[j]);
			if (string_array_index_of(lower, l) >= 0) {
				continue;
			}
			i32 pos = 0; // Insertion sort
			while (pos < lower->length && strcmp(lower->buffer[pos], l) < 0) {
				pos++;
			}
			array_insert((any_array_t *)lower, pos, l);
			char *copy = malloc(strlen(m->tags->buffer[j]) + 1);
			strcpy(copy, m->tags->buffer[j]);
			array_insert((any_array_t *)tags, pos, copy);
		}
	}
	array_delete(lower);
	ui_asset_tags_cache[category]       = tags;
	ui_asset_tags_cache_hash[category]  = hash;
	ui_asset_tags_cache_valid[category] = true;
	return tags;
}

// True when any tag is in use in the category, so the tray gives the filter combo a cell in its row
bool ui_asset_tags_any(i32 category) {
	return ui_asset_tags_in_use(category)->length > 0;
}

// Draws the tag filter combo of a category as one cell of the tray row, only when a tag is in use there (otherwise draws nothing).
// The row must have a cell for it when ui_asset_tags_any is true. Call it every frame before ui_asset_tags_visible. Returns true when the combo was drawn
bool ui_asset_tags_filter(i32 category) {
	string_array_t *tags = ui_asset_tags_in_use(category);
	char          **sel  = &ui_asset_tags_selected[category];
	i32             idx  = 0;
	if (*sel != NULL) { // The selected tag may be gone, fall back to All
		for (i32 i = 0; i < tags->length; ++i) {
			if (string_equals(string_to_lower_tmp(tags->buffer[i]), string_to_lower_tmp(*sel))) {
				idx = i + 1;
				break;
			}
		}
		if (idx == 0) {
			ui_asset_tags_select(category, NULL);
		}
	}
	if (tags->length == 0) {
		return false;
	}
	string_array_t *texts = string_array_create(0);
	string_array_push(texts, tr("All"));
	for (i32 i = 0; i < tags->length; ++i) {
		string_array_push(texts, tags->buffer[i]);
	}
	ui_combo(&idx, texts, tr("Tags"), false, UI_ALIGN_LEFT, true);
	if (ui_item_changed()) { // Copied only on change, the combo runs every frame
		ui_asset_tags_select(category, idx > 0 ? texts->buffer[idx] : NULL);
		asset_selection_clear(); // Items the filter hides must not stay selected
	}
	array_delete(texts);
	return true;
}

// True when a tag filter is selected in the category
bool ui_asset_tags_active(i32 category) {
	return ui_asset_tags_selected[category] != NULL;
}

// True when the filter of the category is All or the item has the selected tag
bool ui_asset_tags_visible(i32 category, i32 index) {
	char *sel = ui_asset_tags_selected_lower[category];
	if (sel == NULL) {
		return true;
	}
	meta_t *m = meta_peek(category, index);
	for (i32 i = 0; m != NULL && m->tags != NULL && i < m->tags->length; ++i) {
		if (ui_asset_tags_equals_lower(m->tags->buffer[i], sel)) {
			return true;
		}
	}
	return false;
}
