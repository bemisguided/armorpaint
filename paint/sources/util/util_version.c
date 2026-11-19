#include "../global.h"

// Change counters, so a cache of something computed from a material graph or from the image assets can tell whether it is stale
// without recomputing it. A cache stores util_version_graph(m) (and util_version_assets()) next to its result and uses the result
// while both still read the same.
//
// Graph version of a material: changes whenever the canvas of the material may have changed. The editor, undo and redo, group
// edits, scripts and imports bump it (util_version_graph_bump, util_version_groups_bump); a replaced canvas object (project open,
// material import or duplicate) is caught by the caller comparing m->canvas with the pointer it stored. It also changes when a
// material the graph embeds through a Material node changes. Not tracked: a Material node inside a group of the graph.
// Asset version: changes when the list of image assets or one of their images changes (reimport, delete, add, reorder: the
// version folds in the asset and image pointers, so no call site has to bump it); an Image Texture node reads the asset by index.
//
// The counters only grow and are never saved. A bump is cheap (no scan), call it at every edit and whenever unsure: a spurious
// bump costs one recompute of the caches, a missed one shows a stale result.

#define UTIL_VERSION_MATERIAL_DEPTH 4 // Material nodes inside Material nodes

static i32 *util_version_ids    = NULL; // Material ids that were bumped on their own
static u32 *util_version_values = NULL;
static i32  util_version_count  = 0;
static i32  util_version_cap    = 0;
static u32  util_version_epoch  = 1; // Bumped for everything at once
static u32  util_version_asset  = 1;

static u32 util_version_own(i32 material_id) {
	for (i32 i = 0; i < util_version_count; ++i) {
		if (util_version_ids[i] == material_id) {
			return util_version_values[i];
		}
	}
	return 0;
}

static u32 util_version_graph_depth(slot_material_t *m, i32 depth) {
	u32 v = util_version_own(m->id);
	if (depth >= UTIL_VERSION_MATERIAL_DEPTH || m->canvas == NULL || m->canvas->nodes == NULL || g_project == NULL) {
		return v;
	}
	for (i32 i = 0; i < m->canvas->nodes->length; ++i) {
		ui_node_t *n = m->canvas->nodes->buffer[i];
		if (string_equals(n->type, "MATERIAL") && n->buttons->length > 0) {
			i32 mi = (i32)n->buttons->buffer[0]->default_value->buffer[0];
			if (mi >= 0 && mi < g_project->_->materials->length && g_project->_->materials->buffer[mi] != m) {
				slot_material_t *ref = g_project->_->materials->buffer[mi];
				v += (u32)ref->id * 7919u + util_version_graph_depth(ref, depth + 1); // The id: a deleted material shifts the indices
			}
		}
	}
	return v;
}

// Version of the graph of material m, never 0
u32 util_version_graph(slot_material_t *m) {
	return util_version_epoch + (m != NULL ? util_version_graph_depth(m, 0) : 0);
}

// The canvas of material m changed. NULL: any canvas may have changed (undo, redo, import, a group edit: groups are shared by every material).
void util_version_graph_bump(slot_material_t *m) {
	if (m == NULL) {
		util_version_epoch++;
		return;
	}
	for (i32 i = 0; i < util_version_count; ++i) {
		if (util_version_ids[i] == m->id) {
			util_version_values[i]++;
			return;
		}
	}
	if (util_version_count == util_version_cap) {
		i32 cap = util_version_cap > 0 ? util_version_cap * 2 : 16;
		i32 *ids = realloc(util_version_ids, (size_t)cap * sizeof(i32));
		u32 *vals = realloc(util_version_values, (size_t)cap * sizeof(u32));
		if (ids == NULL || vals == NULL) {
			util_version_ids    = ids != NULL ? ids : util_version_ids;
			util_version_values = vals != NULL ? vals : util_version_values;
			util_version_epoch++; // Out of memory: the safe side
			return;
		}
		util_version_ids    = ids;
		util_version_values = vals;
		util_version_cap    = cap;
	}
	util_version_ids[util_version_count]    = m->id;
	util_version_values[util_version_count] = 1;
	util_version_count++;
}

// The node editor changed a canvas that is open now: the material canvas when no group is open, else (a group, shared by
// every material) everything. Brush and mesh canvases hold no image nodes.
void util_version_graph_bump_editor() {
	if (ui_nodes_canvas_type != CANVAS_TYPE_MATERIAL) {
		return;
	}
	util_version_graph_bump(ui_nodes_group_stack->length > 0 ? NULL : g_context->material);
}

u32 util_version_assets() {
	u32 v = util_version_asset;
	for (i32 i = 0; g_project != NULL && i < g_project->_->assets->length; ++i) {
		asset_t *a = g_project->_->assets->buffer[i];
		v          = v * 31u + (u32)((uintptr_t)a >> 4) + (u32)((uintptr_t)a->image >> 4) * 17u;
	}
	return v;
}

// Something other than the asset list changed what the assets show (an in place reload)
void util_version_assets_bump() {
	util_version_asset++;
}
