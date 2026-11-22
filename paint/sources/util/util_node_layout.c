
#include "../global.h"

// Node layout resolver: a canvas saved by an older build holds every node with the inputs, outputs and settings (buttons) it had then, and
// the links address sockets by INDEX. When the definition of a node type changes (an item added, moved, removed, a type renamed) the saved
// indices point at other items. The names are saved too (English, the UI translates when it draws), so a loaded node is matched to the
// current definition BY NAME and the links and values follow the items. spec: specs/NODE_LAYOUT_NAMES.md.
//
// - Matching key: "name#ordinal", the ordinal is the occurrence among equal names in the node (Math "Value" twice, Brick "Frequency" twice).
// - A saved item the definition does not have any more is dropped with its links, a definition item the save does not have is created from
//   the definition (its default), a matched item keeps the saved value (settings: the saved data of non enum settings too).
// - A type alias maps a renamed node type to its new type (only when the new type has a definition), an item alias maps the old name of a
//   renamed socket or setting to its new name. Rule: renaming a label needs an alias entry in the same commit (host test of the names).
// - Nodes without a definition, group nodes and script nodes (their sockets are user data) keep the index logic, untouched.
// - A canvas whose nodes already have the layout of their definitions is not changed (names are compared, nothing is allocated).
// - The links dropped while a project, material or group is opened are collected and reported ONCE by util_node_layout_flush.
// - Extension point for Node Groups: group sockets are user text and keep the index (util_node_layout_is_open_layout); a future group
//   feature can decide its own key for group sockets here, in one place, without touching the callers.
//
// The core has no dependency on the fork: only the node structs, console_info / console_log and the string and array helpers of the base.

typedef struct util_node_layout_type_alias {
	char *old_type;
	char *new_type;
} util_node_layout_type_alias_t;

typedef struct util_node_layout_item_alias {
	char *type; // The node type after the type alias
	char *old_name;
	char *new_name;
} util_node_layout_item_alias_t;

// Renamed node types, old save type -> current type. Applied only when lookup finds a definition of the new type.
// To add when the pass-through versions of the generators land: TEX_GRADIENT_GENERATOR and TEX_SHAPE_GENERATOR -> their new type strings.
static util_node_layout_type_alias_t util_node_layout_type_aliases[] = {
    {"TEX_LEVELS", "LEVELS"}, // The first (baked) Levels build: same Image, Black, White, Gamma, Out Black, Out White, Position, Width, Contrast; Size and the state row are dropped
    {"TEX_GRADIENT_GENERATOR", "GRADIENT_GENERATOR"}, // The first (baked) Gradient Generator build
    {"TEX_SHAPE_GENERATOR", "SHAPE_GENERATOR"}, // The first (baked) Shape Generator build
    {NULL, NULL},
};

// Renamed sockets and settings, old name -> current name, per current node type (inputs, outputs and settings share one list).
static util_node_layout_item_alias_t util_node_layout_item_aliases[] = {
    {"LEVELS", "levels_node_auto_button", "levels_auto_button"},
    {"LEVELS", "levels_node_strip_button", "levels_strip_button"},
    {"TEX_TILE_UV", "Size", "Capture Size"}, // The baked Tile UV (Size, state row, Filter) became a pass-through node (Capture Size, Filter): same type, the state row is dropped
    {"TEX_SEAMLESS", "Size", "Capture Size"}, // The baked Seamless (Size, state row) became a pass-through node (Capture Size): same type, the state row is dropped
    {NULL, NULL, NULL},
};

// Node types whose sockets are user data, not defined by their node type: index logic
static char *util_node_layout_open_types[] = {"GROUP", "GROUP_INPUT", "GROUP_OUTPUT", "SCRIPT_CPU", "MESH_GROUP", "MESH_GROUP_INPUT", "MESH_GROUP_OUTPUT", NULL};

#define UTIL_NODE_LAYOUT_MAX_LINES 30

static i32    util_node_layout_dropped_total = 0;
static char **util_node_layout_dropped_lines = NULL;
static i32    util_node_layout_dropped_count = 0;

bool util_node_layout_is_open_layout(char *type) {
	for (i32 i = 0; util_node_layout_open_types[i] != NULL; ++i) {
		if (string_equals(type, util_node_layout_open_types[i])) {
			return true;
		}
	}
	return false;
}

static void util_node_layout_note_drop(char *type, char *side, char *name) {
	util_node_layout_dropped_total++;
	if (util_node_layout_dropped_count >= UTIL_NODE_LAYOUT_MAX_LINES) {
		return;
	}
	if (util_node_layout_dropped_lines == NULL) {
		util_node_layout_dropped_lines = calloc(UTIL_NODE_LAYOUT_MAX_LINES, sizeof(char *));
	}
	util_node_layout_dropped_lines[util_node_layout_dropped_count++] = string_copy(string("dropped link: %s %s \"%s\"", type, side, name != NULL ? name : "?"));
}

// One summary line for the status bar and the Console, one Console line per dropped link (capped). Called once at the end of an open.
void util_node_layout_flush() {
	if (util_node_layout_dropped_total == 0) {
		return;
	}
	console_info(string("%d %s", util_node_layout_dropped_total, tr("links dropped while opening this material")));
	for (i32 i = 0; i < util_node_layout_dropped_count; ++i) {
		console_log(util_node_layout_dropped_lines[i]);
	}
	if (util_node_layout_dropped_total > util_node_layout_dropped_count) {
		console_log(string("... and %d more", util_node_layout_dropped_total - util_node_layout_dropped_count));
	}
	util_node_layout_dropped_total = 0;
	util_node_layout_dropped_count = 0;
}

static ui_node_t *util_node_layout_find_def(ui_node_t *(*lookup)(char *), char *type) {
	if (lookup == NULL || type == NULL || util_node_layout_is_open_layout(type)) {
		return NULL;
	}
	return lookup(type);
}

static bool util_node_layout_has_name(char **names, i32 n, char *name) {
	for (i32 i = 0; i < n; ++i) {
		if (names[i] != NULL && string_equals(names[i], name)) {
			return true;
		}
	}
	return false;
}

// The current name of a saved item name: the item alias when the saved name is not a name of the definition itself
static char *util_node_layout_alias_name(char *type, char *name, char **def_names, i32 def_count) {
	if (name == NULL || util_node_layout_has_name(def_names, def_count, name)) {
		return name;
	}
	for (i32 i = 0; util_node_layout_item_aliases[i].type != NULL; ++i) {
		util_node_layout_item_alias_t *a = &util_node_layout_item_aliases[i];
		if (string_equals(a->type, type) && string_equals(a->old_name, name)) {
			return a->new_name;
		}
	}
	return name;
}

static i32 util_node_layout_ordinal(char **names, i32 upto) {
	i32 o = 0;
	for (i32 i = 0; i < upto; ++i) {
		if (names[i] != NULL && names[upto] != NULL && string_equals(names[i], names[upto])) {
			o++;
		}
	}
	return o;
}

// For every definition item j the index of the saved item that is the same item, or -1 (def_to_saved), and the reverse (saved_to_def).
// An item with an empty name is only matched to the item at its own index when that is empty too.
static void util_node_layout_match(char *type, char **saved, i32 ns, char **def, i32 nd, i32 *saved_to_def, i32 *def_to_saved) {
	char **resolved = calloc(ns > 0 ? ns : 1, sizeof(char *));
	for (i32 i = 0; i < ns; ++i) {
		resolved[i]      = util_node_layout_alias_name(type, saved[i], def, nd);
		saved_to_def[i] = -1;
	}
	for (i32 j = 0; j < nd; ++j) {
		def_to_saved[j] = -1;
		if (def[j] == NULL) {
			continue;
		}
		if (def[j][0] == '\0') {
			if (j < ns && saved[j] != NULL && saved[j][0] == '\0') {
				def_to_saved[j] = j;
				saved_to_def[j] = j;
			}
			continue;
		}
		i32 dord = util_node_layout_ordinal(def, j);
		for (i32 i = 0; i < ns; ++i) {
			if (resolved[i] == NULL || saved_to_def[i] >= 0 || !string_equals(resolved[i], def[j])) {
				continue;
			}
			if (util_node_layout_ordinal(resolved, i) == dord) {
				def_to_saved[j] = i;
				saved_to_def[i] = j;
				break;
			}
		}
	}
	free(resolved);
}

static ui_node_socket_t *util_node_layout_socket_from_def(ui_node_socket_t *d, i32 node_id, i32 id) {
	ui_node_socket_t *s = ALLOC_INIT(ui_node_socket_t, {0});
	s->id               = id;
	s->node_id          = node_id;
	s->name             = string_copy(d->name);
	s->type             = string_copy(d->type);
	s->color            = d->color;
	s->default_value    = f32_array_create_from_array(d->default_value);
	s->min              = d->min;
	s->max              = d->max;
	s->precision        = d->precision;
	s->display          = d->display;
	return s;
}

static ui_node_button_t *util_node_layout_button_from_def(ui_node_button_t *d) {
	ui_node_button_t *b = ALLOC_INIT(ui_node_button_t, {0});
	b->name             = string_copy(d->name);
	b->type             = string_copy(d->type);
	b->output           = d->output;
	b->default_value    = f32_array_create_from_array(d->default_value);
	if (d->data != NULL) {
		b->data = u8_array_create_from_array(d->data);
	}
	b->min       = d->min;
	b->max       = d->max;
	b->precision = d->precision;
	b->height    = d->height;
	return b;
}

// Number of items of an enum setting: its data is the item list separated by line breaks
static i32 util_node_layout_enum_count(u8_array_t *data) {
	if (data == NULL || data->length == 0) {
		return 0;
	}
	i32 n = 1;
	for (i32 i = 0; i < data->length; ++i) {
		if (data->buffer[i] == '\n') {
			n++;
		}
	}
	return n;
}

// A saved setting that is the same item as definition item d: the saved value (and the saved data of a setting that holds user data) on the
// structure of the definition. A different type (or a value that is out of the items of a changed enum) falls back to the definition.
static ui_node_button_t *util_node_layout_merge_button(ui_node_button_t *saved, ui_node_button_t *d) {
	if (saved->type == NULL || d->type == NULL || !string_equals(saved->type, d->type) || saved->default_value == NULL || saved->default_value->length < d->default_value->length) {
		return util_node_layout_button_from_def(d);
	}
	bool is_enum = string_equals(d->type, "ENUM");
	if (is_enum) {
		i32 n = util_node_layout_enum_count(d->data);
		if (n > 0) {
			saved->data = u8_array_create_from_array(d->data); // The items of the definition (the saved list may be an older one)
			f32 v       = saved->default_value->buffer[0];
			if (!(v >= 0.0f && v < (f32)n)) {
				saved->default_value->buffer[0] = d->default_value->buffer[0];
			}
		}
	}
	else if (saved->data == NULL && d->data != NULL) {
		saved->data = u8_array_create_from_array(d->data);
	}
	saved->name      = string_copy(d->name);
	saved->output    = d->output;
	saved->min       = d->min;
	saved->max       = d->max;
	saved->precision = d->precision;
	saved->height    = d->height;
	return saved;
}

// The links of a node whose items were remapped: old index -> new index, -1 = the item is gone
typedef struct util_node_layout_remap {
	ui_node_t         *node;
	i32                id;
	i32                in_count;
	i32                out_count;
	i32               *in_map;
	i32               *out_map;
	ui_node_socket_t **old_in;
	ui_node_socket_t **old_out;
} util_node_layout_remap_t;

// Rebuilds a socket list on the definition list, returns the old to new map and the old sockets (both freed by the caller)
static void util_node_layout_resolve_sockets(ui_node_t *node, ui_node_socket_t_array_t *list, ui_node_socket_t_array_t *def_list, i32 *next_id, i32 **map_out,
                                              ui_node_socket_t ***old_out) {
	i32   ns          = list->length;
	i32   nd          = def_list->length;
	char **saved      = calloc(ns > 0 ? ns : 1, sizeof(char *));
	char **def        = calloc(nd > 0 ? nd : 1, sizeof(char *));
	i32   *s2d        = calloc(ns > 0 ? ns : 1, sizeof(i32));
	i32   *d2s        = calloc(nd > 0 ? nd : 1, sizeof(i32));
	ui_node_socket_t **old = calloc(ns > 0 ? ns : 1, sizeof(ui_node_socket_t *));
	for (i32 i = 0; i < ns; ++i) {
		old[i]   = list->buffer[i];
		saved[i] = old[i]->name;
	}
	for (i32 j = 0; j < nd; ++j) {
		def[j] = def_list->buffer[j]->name;
	}
	util_node_layout_match(node->type, saved, ns, def, nd, s2d, d2s);
	list->length = 0;
	for (i32 j = 0; j < nd; ++j) {
		ui_node_socket_t *d = def_list->buffer[j];
		i32               i = d2s[j];
		if (i < 0) {
			any_array_push(list, util_node_layout_socket_from_def(d, node->id, (*next_id)++));
			continue;
		}
		ui_node_socket_t *s = old[i];
		bool same_type      = s->type != NULL && string_equals(s->type, d->type) && s->default_value != NULL && s->default_value->length == d->default_value->length;
		if (!same_type) {
			s->type          = string_copy(d->type);
			s->default_value = f32_array_create_from_array(d->default_value);
		}
		s->name      = string_copy(d->name);
		s->node_id   = node->id;
		s->color     = d->color;
		s->min       = d->min;
		s->max       = d->max;
		s->precision = d->precision;
		s->display   = d->display;
		any_array_push(list, s);
	}
	free(saved);
	free(def);
	free(d2s);
	*map_out = s2d;
	*old_out = old;
}

static void util_node_layout_resolve_buttons(ui_node_t *node, ui_node_button_t_array_t *def_list) {
	ui_node_button_t_array_t *list = node->buttons;
	i32                       ns   = list->length;
	i32                       nd   = def_list->length;
	char                    **saved = calloc(ns > 0 ? ns : 1, sizeof(char *));
	char                    **def   = calloc(nd > 0 ? nd : 1, sizeof(char *));
	i32                      *s2d   = calloc(ns > 0 ? ns : 1, sizeof(i32));
	i32                      *d2s   = calloc(nd > 0 ? nd : 1, sizeof(i32));
	ui_node_button_t        **old   = calloc(ns > 0 ? ns : 1, sizeof(ui_node_button_t *));
	for (i32 i = 0; i < ns; ++i) {
		old[i]   = list->buffer[i];
		saved[i] = old[i]->name;
	}
	for (i32 j = 0; j < nd; ++j) {
		def[j] = def_list->buffer[j]->name;
	}
	util_node_layout_match(node->type, saved, ns, def, nd, s2d, d2s);
	list->length = 0;
	for (i32 j = 0; j < nd; ++j) {
		i32 i = d2s[j];
		any_array_push(list, i < 0 ? util_node_layout_button_from_def(def_list->buffer[j]) : util_node_layout_merge_button(old[i], def_list->buffer[j]));
	}
	free(saved);
	free(def);
	free(s2d);
	free(d2s);
	free(old);
}

// Names of the items of a node and of its definition for the "already equal" test
static bool util_node_layout_sockets_equal(ui_node_socket_t_array_t *a, ui_node_socket_t_array_t *b) {
	if (a->length != b->length) {
		return false;
	}
	for (i32 i = 0; i < a->length; ++i) {
		if (a->buffer[i]->name == NULL || b->buffer[i]->name == NULL || !string_equals(a->buffer[i]->name, b->buffer[i]->name)) {
			return false;
		}
	}
	return true;
}

static bool util_node_layout_buttons_equal(ui_node_button_t_array_t *a, ui_node_button_t_array_t *b) {
	if (a->length != b->length) {
		return false;
	}
	for (i32 i = 0; i < a->length; ++i) {
		if (a->buffer[i]->name == NULL || b->buffer[i]->name == NULL || !string_equals(a->buffer[i]->name, b->buffer[i]->name)) {
			return false;
		}
	}
	return true;
}

static i32 util_node_layout_hash(i32 id, i32 mask) {
	return (i32)(((u32)id * 2654435761u) >> 7) & mask;
}

static util_node_layout_remap_t *util_node_layout_find_remap(i32 *table, i32 mask, util_node_layout_remap_t *remaps, i32 id) {
	i32 h = util_node_layout_hash(id, mask);
	while (table[h] >= 0) {
		if (remaps[table[h]].id == id) {
			return &remaps[table[h]];
		}
		h = (h + 1) & mask;
	}
	return NULL;
}

// Resolves every node of the canvas against lookup(type), the definitions of the kind of canvas (material nodes: nodes_material_get_node_t).
// Returns the number of nodes whose layout was changed. lookup NULL: nothing is changed.
i32 util_node_layout_resolve(ui_node_canvas_t *canvas, ui_node_t *(*lookup)(char *)) {
	if (canvas == NULL || canvas->nodes == NULL || lookup == NULL) {
		return 0;
	}
	i32                       count   = canvas->nodes->length;
	util_node_layout_remap_t *remaps  = NULL;
	i32                       changed = 0;
	i32                       next_id = -1;
	for (i32 k = 0; k < count; ++k) {
		ui_node_t *node = canvas->nodes->buffer[k];
		if (node == NULL || node->type == NULL || node->inputs == NULL || node->outputs == NULL || node->buttons == NULL) {
			continue;
		}
		ui_node_t *def = util_node_layout_find_def(lookup, node->type);
		bool       retyped = false;
		if (def == NULL) {
			for (i32 a = 0; util_node_layout_type_aliases[a].old_type != NULL; ++a) {
				if (string_equals(node->type, util_node_layout_type_aliases[a].old_type)) {
					def = util_node_layout_find_def(lookup, util_node_layout_type_aliases[a].new_type);
					if (def != NULL) {
						node->type  = string_copy(def->type);
						node->name  = string_copy(def->name);
						node->color = def->color;
						node->flags &= UI_NODE_FLAG_SECTION_0 - 1; // The section bits are those of the old layout
						retyped = true;
					}
					break;
				}
			}
		}
		if (def == NULL || def->inputs == NULL || def->outputs == NULL || def->buttons == NULL) {
			continue;
		}
		if (!retyped && util_node_layout_sockets_equal(node->inputs, def->inputs) && util_node_layout_sockets_equal(node->outputs, def->outputs) &&
		    util_node_layout_buttons_equal(node->buttons, def->buttons)) {
			continue; // Already the layout of the definition
		}
		if (next_id < 0) {
			next_id = ui_get_socket_id(canvas->nodes);
		}
		if (remaps == NULL) {
			remaps = calloc(count, sizeof(util_node_layout_remap_t));
		}
		util_node_layout_remap_t *r = &remaps[changed++];
		r->node                      = node;
		node->color                  = def->color; // The header follows the definition too (a node that became a pass-through node loses its orange header), like its sockets
		r->id                        = node->id;
		r->in_count                  = node->inputs->length;
		r->out_count                 = node->outputs->length;
		util_node_layout_resolve_sockets(node, node->inputs, def->inputs, &next_id, &r->in_map, &r->old_in);
		util_node_layout_resolve_sockets(node, node->outputs, def->outputs, &next_id, &r->out_map, &r->old_out);
		util_node_layout_resolve_buttons(node, def->buttons);
	}
	if (changed == 0) {
		return 0;
	}
	// One pass over the links: remap the sockets of the changed nodes, drop what lost its item
	i32 size = 2;
	while (size < changed * 2) {
		size <<= 1;
	}
	i32  mask  = size - 1;
	i32 *table = malloc(size * sizeof(i32));
	for (i32 i = 0; i < size; ++i) {
		table[i] = -1;
	}
	for (i32 i = 0; i < changed; ++i) {
		i32 h = util_node_layout_hash(remaps[i].id, mask);
		while (table[h] >= 0) {
			h = (h + 1) & mask;
		}
		table[h] = i;
	}
	i32 kept = 0;
	for (i32 q = 0; q < canvas->links->length; ++q) {
		ui_node_link_t           *l  = canvas->links->buffer[q];
		util_node_layout_remap_t *rf = util_node_layout_find_remap(table, mask, remaps, l->from_id);
		util_node_layout_remap_t *rt = util_node_layout_find_remap(table, mask, remaps, l->to_id);
		i32                       nf = l->from_socket;
		i32                       nt = l->to_socket;
		bool                      drop = false;
		if (rf != NULL) {
			nf = nf >= 0 && nf < rf->out_count ? rf->out_map[nf] : -1;
			if (nf < 0) {
				util_node_layout_note_drop(rf->node->type, "output", l->from_socket >= 0 && l->from_socket < rf->out_count ? rf->old_out[l->from_socket]->name : NULL);
				drop = true;
			}
		}
		if (!drop && rt != NULL) {
			nt = nt >= 0 && nt < rt->in_count ? rt->in_map[nt] : -1;
			if (nt < 0) {
				util_node_layout_note_drop(rt->node->type, "input", l->to_socket >= 0 && l->to_socket < rt->in_count ? rt->old_in[l->to_socket]->name : NULL);
				drop = true;
			}
		}
		if (drop) {
			continue;
		}
		l->from_socket                = nf;
		l->to_socket                  = nt;
		canvas->links->buffer[kept++] = l;
	}
	canvas->links->length = kept;
	for (i32 i = 0; i < changed; ++i) {
		free(remaps[i].in_map);
		free(remaps[i].out_map);
		free(remaps[i].old_in);
		free(remaps[i].old_out);
	}
	free(table);
	free(remaps);
	return changed;
}
