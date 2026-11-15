#ifdef WITH_PLUGINS

// Gpu per object bake of the Bake Maps plugin: occlusion, bent normal and thickness of a udim project, one result
// per (object, tile) pair at the tile size, traced with the core raytrace bake shaders.
// It runs beside the core bake (bake_to_target) and changes none of it, like the per object lightmap job of
// render_path_raytrace_bake.c: own textures and dispatches, and the shared raytrace state is handed back with
// render_path_raytrace_ready = false so the viewport and the core bake rebuild theirs.
// Positions and normals per texel come from the cpu bake's texmap of the pair (bake_maps.c), the results go
// into its per object buffers, so saving, naming, preview and inpaint are the cpu per object ones.
// Driven by bake_maps_step, which the script polls every update.

#include "engine.h"
#include "iron_array.h"
#include "iron_draw.h"
#include "iron_gpu.h"
#include "iron_map.h"
#include "iron_obj.h"
#include "iron_string.h"
#include "iron_ui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../sources/types.h"

extern context_t *g_context;
extern project_t *g_project;
extern bool       bake_texture_node_baking;
extern char      *render_path_raytrace_ext;
extern bool       render_path_raytrace_ready;
extern bool       render_path_raytrace_is_bake;
extern bool       render_path_raytrace_init_shader;
bool              render_path_raytrace_bake_lightmap_busy(void);
float             render_path_raytrace_half_to_float(uint16_t h);
void              context_set_viewport_mode(viewport_mode_t mode);
gpu_texture_t    *gpu_create_texture_from_bytes_raw(buffer_t *data, i32 width, i32 height, i32 format);
gpu_texture_t    *gpu_create_render_target(i32 width, i32 height, i32 format);
void              gpu_delete_texture(gpu_texture_t *texture);
buffer_t         *gpu_get_texture_pixels(gpu_texture_t *image);
void              console_info(char *s);
void              console_error(char *s);
char             *vtr(char *id, any_map_t *vars);

// bake_maps.c
bool bake_maps_gpo_busy();
u64  bake_maps_gpo_key();
bool bake_maps_gpo_prepare(i32 w, i32 h);
i32  bake_maps_gpo_pair_count();
i32 *bake_maps_gpo_use_pair(i32 pair);
i32  bake_maps_gpo_surface(i32 ti, f32 px, f32 py, f32 *p, f32 *nor);
u8  *bake_maps_gpo_result(i32 pair);
void bake_maps_gpo_finish_result(i32 pair);
void bake_maps_gpo_done();
void bake_maps_gpo_drop_low();

#define GPO_DISPATCHES_PER_POLL 2
#define GPO_CONSTANTS           24 // Floats of the raytrace constant buffer

typedef enum {
	GPO_INIT,     // Fill and upload the positions and normals of the next pair
	GPO_DISPATCH, // Trace samples into the target
	GPO_READBACK, // Result into the pair's buffer
} gpo_state_t;

static bool            gpo_running = false;
static bool            gpo_failed  = false;
static gpo_state_t     gpo_state;
static i32             gpo_type;
static i32             gpo_samples;
static f32             gpo_ao[3];
static i32             gpo_w;
static i32             gpo_h;
static i32             gpo_pair;
static i32             gpo_pairs;
static i32             gpo_sample;
static u64             gpo_key;
static f32            *gpo_pos      = NULL; // Staging, w x h RGBA float
static f32            *gpo_nor      = NULL;
static gpu_texture_t  *gpo_pos_tex  = NULL;
static gpu_texture_t  *gpo_nor_tex  = NULL;
static gpu_texture_t  *gpo_target   = NULL;
static bool            gpo_viewport = false; // The viewport was switched out of path tracing
static viewport_mode_t gpo_viewport_mode;
static f32             gpo_constants[GPO_CONSTANTS];

// The update may run inside a draw pass, the ray dispatches and readbacks run outside of it (like lightmap_update)
static gpu_texture_t *gpo_draw_current;
static bool           gpo_draw_in_use;

static void gpo_draw_pause() {
	gpo_draw_current = _draw_current;
	gpo_draw_in_use  = gpu_in_use;
	if (gpo_draw_in_use) {
		draw_end();
	}
}

static void gpo_draw_resume() {
	if (gpo_draw_in_use) {
		draw_begin(gpo_draw_current, false, 0);
	}
}

static char *gpo_shader_name(i32 type) {
	char *name = type == BAKE_TYPE_OCCLUSION ? "raytrace_bake_ao" : type == BAKE_TYPE_BENT_NORMAL ? "raytrace_bake_bent" : "raytrace_bake_thick";
	return string("%s%s", name, render_path_raytrace_ext);
}

// Paint objects with geometry, each one is a geometry of the acceleration structure
static i32 gpo_object_count() {
	i32 count = 0;
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		mesh_object_t *o = g_project->_->paint_objects->buffer[i];
		if (o->data != NULL && o->data->_->vertex_buffer != NULL && o->data->_->index_buffer != NULL) {
			count++;
		}
	}
	return count;
}

// Every paint object, hidden ones too, so other objects occlude like on the cpu.
// The loop of render_path_raytrace_raytrace_init without the merged mesh and override textures.
static void gpo_build_scene() {
	_gpu_raytrace_as_init();
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		mesh_object_t *o = g_project->_->paint_objects->buffer[i];
		if (o->data == NULL || o->data->_->vertex_buffer == NULL || o->data->_->index_buffer == NULL) {
			continue;
		}
		_gpu_raytrace_as_add(o->data->_->vertex_buffer, o->data->_->index_buffer, o->base->transform->world_unpack, NULL);
	}
	_gpu_raytrace_as_build();
}

// World positions and normals of a pair's covered texels, alpha 1, empty texels 0 (skipped by the shaders).
// The only atlas specific part: the texmap of bake_maps_gpo_use_pair takes the atlas uvs to tile local texels.
static void gpo_fill_pair(i32 pair) {
	i32 *texmap = bake_maps_gpo_use_pair(pair);
	i32  w      = gpo_w;
	i32  h      = gpo_h;
	memset(gpo_pos, 0, sizeof(f32) * 4 * w * h);
	memset(gpo_nor, 0, sizeof(f32) * 4 * w * h);
	if (texmap == NULL) {
		return;
	}
	mesh_object_t_array_t *objects = g_project->_->paint_objects;
	for (i32 y = 0; y < h; ++y) {
		for (i32 x = 0; x < w; ++x) {
			i32 i  = y * w + x;
			i32 ti = texmap[i];
			if (ti < 0) {
				continue;
			}
			f32 p[3];
			f32 n[3];
			i32 object = bake_maps_gpo_surface(ti, x + 0.5f, y + 0.5f, p, n);
			if (object < 0 || object >= objects->length) {
				continue;
			}
			// The surface position is scaled already, world (not world_unpack) places it like the scene geometry
			mat4_t m       = objects->buffer[object]->base->transform->world;
			vec4_t wp      = vec4_apply_mat4((vec4_t){p[0], p[1], p[2], 1.0f}, m);
			vec4_t wn      = vec4_norm(vec4_apply_mat4((vec4_t){n[0], n[1], n[2], 0.0f}, m));
			gpo_pos[i * 4] = wp.x;
			gpo_pos[i * 4 + 1] = wp.y;
			gpo_pos[i * 4 + 2] = wp.z;
			gpo_pos[i * 4 + 3] = 1.0f;
			gpo_nor[i * 4]     = wn.x;
			gpo_nor[i * 4 + 1] = wn.y;
			gpo_nor[i * 4 + 2] = wn.z;
			gpo_nor[i * 4 + 3] = 1.0f;
		}
	}
}

static void gpo_free_pair_textures() {
	gpu_texture_t **ts[] = {&gpo_pos_tex, &gpo_nor_tex, &gpo_target};
	for (i32 i = 0; i < 3; ++i) {
		if (*ts[i] != NULL) {
			gpu_delete_texture(*ts[i]);
			*ts[i] = NULL;
		}
	}
}

// Positions, normals and the target of one pair, false without memory
static bool gpo_upload_pair(i32 pair) {
	gpo_fill_pair(pair);
	buffer_t pos = {.buffer = (u8 *)gpo_pos, .length = sizeof(f32) * 4 * gpo_w * gpo_h};
	buffer_t nor = {.buffer = (u8 *)gpo_nor, .length = sizeof(f32) * 4 * gpo_w * gpo_h};
	gpo_pos_tex  = gpu_create_texture_from_bytes_raw(&pos, gpo_w, gpo_h, GPU_TEXTURE_FORMAT_RGBA128);
	gpo_nor_tex  = gpu_create_texture_from_bytes_raw(&nor, gpo_w, gpo_h, GPU_TEXTURE_FORMAT_RGBA128);
	gpo_target   = gpu_create_render_target(gpo_w, gpo_h, GPU_TEXTURE_FORMAT_RGBA64);
	return gpo_pos_tex != NULL && gpo_nor_tex != NULL && gpo_target != NULL;
}

// One sample (64 rays, 4 on metal) of every covered texel, the shaders average the samples in the target
static void gpo_dispatch() {
	// Set before every dispatch, the textures are shared with the viewport and the core bake
	gpu_texture_t *sobol = data_get_texture("bnoise_sobol.k");
	gpu_raytrace_set_textures(gpo_pos_tex, gpo_nor_tex, gpo_pos_tex, sobol, sobol, data_get_texture("bnoise_scramble.k"),
	                          data_get_texture("bnoise_rank.k"), sobol); // The environment is not sampled by these maps
	// Same constants as render_path_raytrace_bake_commands
	memset(gpo_constants, 0, sizeof(gpo_constants));
	gpo_constants[0] = gpo_sample;
	gpo_constants[1] = gpo_ao[0];
	gpo_constants[2] = gpo_ao[1];
	gpo_constants[3] = gpo_ao[2];
	gpo_constants[4] = scene_world != NULL ? scene_world->strength : 1.0f;
	gpo_constants[5] = g_context->bake_up_axis;
	gpo_constants[6] = g_context->envmap_angle;
	buffer_t b       = {.buffer = (u8 *)gpo_constants, .length = sizeof(gpo_constants)};
	_gpu_raytrace_dispatch_rays(gpo_target, &b);
	gpo_sample++;
}

// Half float target into the pair's RGBA8 buffer, then dilated like a cpu result
static void gpo_readback(i32 pair) {
	buffer_t *pixels = gpu_get_texture_pixels(gpo_target);
	u8       *out    = bake_maps_gpo_result(pair);
	if (pixels == NULL || out == NULL || pixels->length < (u32)(gpo_w * gpo_h * 8)) {
		return;
	}
	u16 *hp = (u16 *)pixels->buffer;
	for (i32 i = 0; i < gpo_w * gpo_h; ++i) {
		for (i32 c = 0; c < 3; ++c) {
			f32 v          = render_path_raytrace_half_to_float(hp[i * 4 + c]);
			v              = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
			out[i * 4 + c] = (u8)(v * 255.0f + 0.5f);
		}
		out[i * 4 + 3] = 255;
	}
	bake_maps_gpo_finish_result(pair);
}

// Hands the raytrace state back, the viewport and the core bake rebuild their scene and shader
static void gpo_release_raytrace() {
	render_path_raytrace_ready       = false;
	render_path_raytrace_is_bake     = true;
	render_path_raytrace_init_shader = true;
}

#endif
