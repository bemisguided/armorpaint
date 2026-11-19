#include "../global.h"

// Covered objects that share UV space inside a tile (informational label on the Slot Texture node).
// Per tile each covered object's triangles are rasterised into a small bitmap, two objects setting the same pixel overlap.
// Computed once per (covered object ids, covered tiles, util_mesh_version) and cached, only read from the node UI.
// The check rasterises every triangle of the covered objects, so it runs on a worker thread over a snapshot of the uvs and indices
// (the meshes can change while it runs). util_uv_overlap_covered starts it and answers false until the result is in; the result is
// handed over by util_uv_overlap_poll (every frame) and the node window redraws so the label appears.

#define UTIL_UV_OVERLAP_RES   128
#define UTIL_UV_OVERLAP_CACHE 4

typedef struct util_uv_overlap_entry {
	i32_array_t *key;
	i32          version;
	bool         overlap;
} util_uv_overlap_entry_t;

typedef struct util_uv_overlap_object {
	i16 *tex; // Copy of the atlas uv values
	u32 *ind; // Copy of the indices
	i32  ind_length;
} util_uv_overlap_object_t;

// One check: the snapshot the worker reads, and its answer
typedef struct util_uv_overlap_job {
	util_uv_overlap_object_t *objects;
	i32                       object_count;
	i32                      *slots; // Atlas slots of the covered tiles
	i32                       slot_count;
	i32                       stride;
	i32                       tile_count; // util_udim_count()
	bool                      overlap;
	i32_array_t              *key;
	i32                       version;
} util_uv_overlap_job_t;

static util_uv_overlap_job_t *util_uv_overlap_job = NULL; // Running or finished, NULL when idle (main thread only)
static iron_thread_t          util_uv_overlap_thread;
static iron_mutex_t           util_uv_overlap_mutex;
static bool                   util_uv_overlap_mutex_ready = false;
static bool                   util_uv_overlap_done        = false; // Guarded by the mutex, set by the worker when it is finished

static util_uv_overlap_entry_t util_uv_overlap_cache[UTIL_UV_OVERLAP_CACHE];
static i32                     util_uv_overlap_next = 0;

static i32 util_uv_overlap_cell(f32 atlas, i32 stride) {
	i32 c = (i32)(atlas * stride / 32767.0f);
	return c < 0 ? 0 : c >= stride ? stride - 1 : c;
}

// Slot of a triangle from its atlas uv centroid, -1 outside the tile list
static i32 util_uv_overlap_slot(i16 *tex, u32 *ind, i32 t, i32 stride, i32 tile_count) {
	f32 u = (tex[ind[t * 3] * 2] + tex[ind[t * 3 + 1] * 2] + tex[ind[t * 3 + 2] * 2]) / 3.0f;
	f32 v = (tex[ind[t * 3] * 2 + 1] + tex[ind[t * 3 + 1] * 2 + 1] + tex[ind[t * 3 + 2] * 2 + 1]) / 3.0f;
	i32 s = util_uv_overlap_cell(v, stride) * stride + util_uv_overlap_cell(u, stride);
	return s < tile_count ? s : -1;
}

// Sets the pixels whose centre is strictly inside the object's triangles of the slot (tile local uv)
static void util_uv_overlap_raster(u8 *bits, util_uv_overlap_object_t *o, i32 slot, i32 stride, i32 tile_count) {
	i16 *tex  = o->tex;
	u32 *ind  = o->ind;
	i32  n    = o->ind_length / 3;
	f32  step = 32767.0f / stride;
	f32  ox   = (slot % stride) * step;
	f32  oy   = (slot / stride) * step;
	f32  f    = UTIL_UV_OVERLAP_RES / step;
	for (i32 t = 0; t < n; ++t) {
		if (util_uv_overlap_slot(tex, ind, t, stride, tile_count) != slot) {
			continue;
		}
		f32 x[3];
		f32 y[3];
		for (i32 k = 0; k < 3; ++k) {
			x[k] = (tex[ind[t * 3 + k] * 2] - ox) * f;
			y[k] = (tex[ind[t * 3 + k] * 2 + 1] - oy) * f;
		}
		f32 area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
		if (area == 0.0f) {
			continue;
		}
		f32 sign = area > 0.0f ? 1.0f : -1.0f;
		i32 x0   = (i32)floorf(fminf(x[0], fminf(x[1], x[2])));
		i32 x1   = (i32)ceilf(fmaxf(x[0], fmaxf(x[1], x[2])));
		i32 y0   = (i32)floorf(fminf(y[0], fminf(y[1], y[2])));
		i32 y1   = (i32)ceilf(fmaxf(y[0], fmaxf(y[1], y[2])));
		x0       = x0 < 0 ? 0 : x0;
		y0       = y0 < 0 ? 0 : y0;
		x1       = x1 > UTIL_UV_OVERLAP_RES ? UTIL_UV_OVERLAP_RES : x1;
		y1       = y1 > UTIL_UV_OVERLAP_RES ? UTIL_UV_OVERLAP_RES : y1;
		for (i32 py = y0; py < y1; ++py) {
			for (i32 px = x0; px < x1; ++px) {
				f32 cx = px + 0.5f;
				f32 cy = py + 0.5f;
				f32 e0 = ((x[1] - x[0]) * (cy - y[0]) - (y[1] - y[0]) * (cx - x[0])) * sign;
				f32 e1 = ((x[2] - x[1]) * (cy - y[1]) - (y[2] - y[1]) * (cx - x[1])) * sign;
				f32 e2 = ((x[0] - x[2]) * (cy - y[2]) - (y[0] - y[2]) * (cx - x[2])) * sign;
				if (e0 > 0.0f && e1 > 0.0f && e2 > 0.0f) {
					bits[py * UTIL_UV_OVERLAP_RES + px] = 1;
				}
			}
		}
	}
}

// The check itself, only reads the job
static bool util_uv_overlap_find(util_uv_overlap_job_t *job) {
	i32  px_n  = UTIL_UV_OVERLAP_RES * UTIL_UV_OVERLAP_RES;
	u8  *all   = malloc(px_n);
	u8  *one   = malloc(px_n);
	bool found = false;
	for (i32 k = 0; k < job->slot_count && !found; ++k) {
		memset(all, 0, px_n);
		for (i32 i = 0; i < job->object_count && !found; ++i) {
			memset(one, 0, px_n);
			util_uv_overlap_raster(one, &job->objects[i], job->slots[k], job->stride, job->tile_count);
			bool any = false;
			for (i32 p = 0; p < px_n; ++p) {
				if (one[p]) {
					any = true;
					if (all[p]) {
						found = true;
						break;
					}
				}
			}
			if (any && !found) {
				for (i32 p = 0; p < px_n; ++p) {
					all[p] |= one[p];
				}
			}
		}
	}
	free(all);
	free(one);
	return found;
}

static void util_uv_overlap_worker(void *param) {
	util_uv_overlap_job_t *job = param;
	job->overlap               = util_uv_overlap_find(job);
	iron_mutex_lock(&util_uv_overlap_mutex);
	util_uv_overlap_done = true;
	iron_mutex_unlock(&util_uv_overlap_mutex);
}

// Copies what the check reads: the uv values and indices of the covered objects and the slots of the covered tiles
static util_uv_overlap_job_t *util_uv_overlap_snapshot(slot_layer_t *l, i32_array_t *key) {
	util_uv_overlap_job_t *job = calloc(1, sizeof(util_uv_overlap_job_t));
	job->key                   = key;
	job->version               = util_mesh_version;
	job->stride                = util_udim_stride();
	job->tile_count            = util_udim_count();
	job->slots                 = malloc(sizeof(i32) * (job->tile_count > 0 ? job->tile_count : 1));
	for (i32 s = 0; s < job->tile_count; ++s) {
		if (layer_covers_tile(l, util_udim_tile(s))) {
			job->slots[job->slot_count++] = s;
		}
	}
	job->objects = calloc(g_project->_->paint_objects->length > 0 ? g_project->_->paint_objects->length : 1, sizeof(util_uv_overlap_object_t));
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		mesh_object_t *o = g_project->_->paint_objects->buffer[i];
		if (o->paint_id <= 0 || o->data == NULL || !layer_covers_object(l, o)) {
			continue;
		}
		vertex_array_t *va = mesh_data_get_vertex_array(o->data, "tex");
		if (va == NULL) {
			continue;
		}
		util_uv_overlap_object_t *so = &job->objects[job->object_count++];
		so->ind_length               = o->data->index_array->length;
		so->tex                      = malloc(sizeof(i16) * (va->values->length > 0 ? va->values->length : 1));
		so->ind                      = malloc(sizeof(u32) * (so->ind_length > 0 ? so->ind_length : 1));
		memcpy(so->tex, va->values->buffer, sizeof(i16) * va->values->length);
		memcpy(so->ind, o->data->index_array->buffer, sizeof(u32) * so->ind_length);
	}
	return job;
}

static void util_uv_overlap_job_free(util_uv_overlap_job_t *job) {
	for (i32 i = 0; i < job->object_count; ++i) {
		free(job->objects[i].tex);
		free(job->objects[i].ind);
	}
	free(job->objects);
	free(job->slots);
	free(job);
}

// Covered paint ids and covered tiles of the layer, each part prefixed with its length
static i32_array_t *util_uv_overlap_make_key(slot_layer_t *l) {
	i32_array_t *key = i32_array_create(0);
	i32          len = 0;
	i32_array_push(key, 0);
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		mesh_object_t *o = g_project->_->paint_objects->buffer[i];
		if (layer_covers_object(l, o)) {
			i32_array_push(key, o->paint_id);
			len++;
		}
	}
	key->buffer[0] = len;
	i32 count      = util_udim_count();
	i32_array_push(key, count);
	for (i32 s = 0; s < count; ++s) {
		i32_array_push(key, util_udim_tile(s));
		i32_array_push(key, layer_covers_tile(l, util_udim_tile(s)) ? 1 : 0);
	}
	return key;
}

static bool util_uv_overlap_key_equals(i32_array_t *a, i32_array_t *b) {
	if (a == NULL || b == NULL || a->length != b->length) {
		return false;
	}
	for (i32 i = 0; i < a->length; ++i) {
		if (a->buffer[i] != b->buffer[i]) {
			return false;
		}
	}
	return true;
}

static util_uv_overlap_entry_t *util_uv_overlap_lookup(i32_array_t *key) {
	for (i32 i = 0; i < UTIL_UV_OVERLAP_CACHE; ++i) {
		util_uv_overlap_entry_t *e = &util_uv_overlap_cache[i];
		if (e->version == util_mesh_version && util_uv_overlap_key_equals(e->key, key)) {
			return e;
		}
	}
	return NULL;
}

// Per frame: takes the result of a finished check into the cache and redraws the node window so the label shows up. One flag read while idle.
void util_uv_overlap_poll() {
	if (util_uv_overlap_job == NULL) {
		return;
	}
	iron_mutex_lock(&util_uv_overlap_mutex);
	bool done = util_uv_overlap_done;
	iron_mutex_unlock(&util_uv_overlap_mutex);
	if (!done) {
		return;
	}
	iron_thread_wait_and_destroy(&util_uv_overlap_thread);
	util_uv_overlap_job_t *job = util_uv_overlap_job;
	util_uv_overlap_job        = NULL;
	if (job->version == util_mesh_version) { // The meshes changed while it ran: the result is stale, the next ask starts a new check
		util_uv_overlap_entry_t *e = &util_uv_overlap_cache[util_uv_overlap_next];
		util_uv_overlap_next       = (util_uv_overlap_next + 1) % UTIL_UV_OVERLAP_CACHE;
		if (e->key != NULL) {
			array_delete(e->key);
		}
		e->key     = job->key;
		e->version = job->version;
		e->overlap = job->overlap;
	}
	else {
		array_delete(job->key);
	}
	util_uv_overlap_job_free(job);
	ui_nodes_hwnd->redraws = 2;
}

// Whether two or more objects covered by the layer overlap in uv space inside a tile they share.
// Cached by the covered object ids, covered tiles and util_mesh_version. A new key starts the check on a worker thread and answers
// false until it is done (one check at a time, another key asks again after the redraw the result causes).
bool util_uv_overlap_covered(slot_layer_t *l) {
	if (l == NULL || g_project == NULL || g_project->_->paint_objects == NULL) {
		return false;
	}
	i32_array_t *key = util_uv_overlap_make_key(l);
	if (key->buffer[0] < 2) { // Fewer than two covered objects
		array_delete(key);
		return false;
	}
	util_uv_overlap_entry_t *e = util_uv_overlap_lookup(key);
	if (e != NULL) {
		array_delete(key);
		return e->overlap;
	}
	if (util_uv_overlap_job != NULL) {
		array_delete(key);
		return false;
	}
	if (!util_uv_overlap_mutex_ready) {
		iron_mutex_init(&util_uv_overlap_mutex);
		util_uv_overlap_mutex_ready = true;
	}
	util_uv_overlap_job  = util_uv_overlap_snapshot(l, key);
	util_uv_overlap_done = false;
	iron_thread_init(&util_uv_overlap_thread, &util_uv_overlap_worker, util_uv_overlap_job);
	return false;
}
