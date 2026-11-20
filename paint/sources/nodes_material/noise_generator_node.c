
#include "../global.h"

// Noise Generator: one shader node (per pixel, resolution independent, 3D) with a Mode setting. The mode decides the
// maths and which inputs, outputs and settings are shown (ui_nodes_hidden_rules, a linked socket is always shown).
// Modes are rows of a table (noise_generator_modes_init); specs/noise/NOISE_GENERATOR.md describes how to add one.
//
// Mode Perlin: gradient noise (improved Perlin, the 12 edge midpoint gradients), fade 6t^5 - 15t^4 + 10t^3.
//   - Lattice coordinates wrap at 65536 cells (a bound for the int conversion), or at the period when Tileable (at most 65536).
//   - Integer hash (no sin) of the lattice corner and the seed, so the CPU host test and the GPU agree.
//   - fBM like Noise Texture: Detail 0 is one octave, Detail d has floor(d) + 1 octaves and a fractional part blends
//     the next octave in, Roughness is the gain, Lacunarity the frequency step; the sum is divided by the sum of the
//     gains. Each octave has its own seed, Color uses three seeds.
//   - Distortion displaces the point by a noise vector (in lattice units of Scale) before the fBM.
//   - Output 0.5 + 0.5 * clamp(PERLIN_NORM * n, -1, 1): about 0..1 centred on 0.5 (PERLIN_NORM = 1.3 is the measured
//     99.9 percentile of |n| (0.756) rounded so that the practical range of one octave fills 0..1: automation/host/noise_generator).
//   - Tileable: the lattice repeats every round(Scale * Lacunarity^k) cells in each octave k (Scale is snapped to an
//     integer first), so the output has period 1 in x, y and z of the Vector input.

char *str_noise_generator_perlin = "\
float perlin_wrap(float i, float period) { \
	float m = 65536.0; \
	if (period >= 0.5) { m = min(period, 65536.0); } \
	return i - m * floor((i + 0.5) / m); \
} \
int perlin_hash(float ix, float iy, float iz, int seed) { \
	int h = int(ix) * 374761393 + int(iy) * 668265263 + int(iz) * 1274126177 + seed * 1103515245; \
	h = (h ^ ((h >> 13) & 524287)) * 1274126177; \
	h = h ^ ((h >> 16) & 65535); \
	h = h * 668265263; \
	h = h ^ ((h >> 15) & 131071); \
	return h; \
} \
float perlin_grad(int h, float x, float y, float z) { \
	int k = h & 15; \
	float u = y; \
	if (k < 8) { u = x; } \
	float v = z; \
	if (k < 4) { v = y; } \
	if (k == 12 || k == 14) { v = x; } \
	float su = u; \
	if ((k & 1) != 0) { su = -u; } \
	float sv = v; \
	if ((k & 2) != 0) { sv = -v; } \
	return su + sv; \
} \
float perlin_fade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); } \
float perlin_noise_3d(float3 p, float period, int seed) { \
	float3 fl = floor(p); \
	float fx = p.x - fl.x; \
	float fy = p.y - fl.y; \
	float fz = p.z - fl.z; \
	float x0 = perlin_wrap(fl.x, period); \
	float x1 = perlin_wrap(fl.x + 1.0, period); \
	float y0 = perlin_wrap(fl.y, period); \
	float y1 = perlin_wrap(fl.y + 1.0, period); \
	float z0 = perlin_wrap(fl.z, period); \
	float z1 = perlin_wrap(fl.z + 1.0, period); \
	float n000 = perlin_grad(perlin_hash(x0, y0, z0, seed), fx, fy, fz); \
	float n100 = perlin_grad(perlin_hash(x1, y0, z0, seed), fx - 1.0, fy, fz); \
	float n010 = perlin_grad(perlin_hash(x0, y1, z0, seed), fx, fy - 1.0, fz); \
	float n110 = perlin_grad(perlin_hash(x1, y1, z0, seed), fx - 1.0, fy - 1.0, fz); \
	float n001 = perlin_grad(perlin_hash(x0, y0, z1, seed), fx, fy, fz - 1.0); \
	float n101 = perlin_grad(perlin_hash(x1, y0, z1, seed), fx - 1.0, fy, fz - 1.0); \
	float n011 = perlin_grad(perlin_hash(x0, y1, z1, seed), fx, fy - 1.0, fz - 1.0); \
	float n111 = perlin_grad(perlin_hash(x1, y1, z1, seed), fx - 1.0, fy - 1.0, fz - 1.0); \
	float u = perlin_fade(fx); \
	float v = perlin_fade(fy); \
	float w = perlin_fade(fz); \
	return lerp(lerp(lerp(n000, n100, u), lerp(n010, n110, u), v), lerp(lerp(n001, n101, u), lerp(n011, n111, u), v), w); \
} \
float perlin_base_scale(float scale, float tileable) { \
	if (tileable > 0.5) { return max(1.0, floor(scale + 0.5)); } \
	return max(scale, 0.0001); \
} \
float perlin_octave(float3 p, float freq, float tileable, int seed) { \
	float f = freq; \
	float period = 0.0; \
	if (tileable > 0.5) { \
		f = max(1.0, floor(freq + 0.5)); \
		period = f; \
	} \
	return perlin_noise_3d(p * f, period, seed); \
} \
float perlin_signed(float3 p, float freq, float tileable, int seed) { \
	return clamp(perlin_octave(p, freq, tileable, seed) * 1.3, -1.0, 1.0); \
} \
float perlin_fbm(float3 p, float scale, float detail, float roughness, float lacunarity, float tileable, int seed) { \
	float freq = perlin_base_scale(scale, tileable); \
	float amp = 1.0; \
	float maxamp = 0.0; \
	float sum = 0.0; \
	int n = int(clamp(detail, 0.0, 8.0)); \
	for (int ii = 0; ii <= n; ii += 1) { \
		sum = sum + amp * perlin_octave(p, freq, tileable, seed + ii * 31); \
		maxamp = maxamp + amp; \
		amp = amp * roughness; \
		freq = freq * lacunarity; \
	} \
	float rmd = clamp(detail, 0.0, 8.0) - floor(clamp(detail, 0.0, 8.0)); \
	if (rmd > 0.0) { \
		float t = perlin_octave(p, freq, tileable, seed + (n + 1) * 31); \
		return lerp(sum / maxamp, (sum + t * amp) / (maxamp + amp), rmd); \
	} \
	return sum / maxamp; \
} \
float perlin_channel(float3 p, float scale, float detail, float roughness, float lacunarity, float distortion, float tileable, float seed, int channel) { \
	int sd = int(floor(seed)) + channel * 7919; \
	float3 q = p; \
	if (distortion != 0.0) { \
		float se = perlin_base_scale(scale, tileable); \
		q = p + distortion / se * float3(perlin_signed(p, se, tileable, sd + 101), perlin_signed(p, se, tileable, sd + 202), perlin_signed(p, se, tileable, sd + 303)); \
	} \
	float n = perlin_fbm(q, scale, detail, roughness, lacunarity, tileable, sd); \
	return 0.5 + 0.5 * clamp(n * 1.3, -1.0, 1.0); \
} \
float perlin_factor(float3 p, float scale, float detail, float roughness, float lacunarity, float distortion, float tileable, float seed) { \
	return perlin_channel(p, scale, detail, roughness, lacunarity, distortion, tileable, seed, 0); \
} \
float3 perlin_color(float3 p, float scale, float detail, float roughness, float lacunarity, float distortion, float tileable, float seed) { \
	return float3(perlin_channel(p, scale, detail, roughness, lacunarity, distortion, tileable, seed, 0), \
	              perlin_channel(p, scale, detail, roughness, lacunarity, distortion, tileable, seed, 1), \
	              perlin_channel(p, scale, detail, roughness, lacunarity, distortion, tileable, seed, 2)); \
} \
";

enum {
	NOISE_GENERATOR_INPUT_VECTOR,
	NOISE_GENERATOR_INPUT_SCALE,
	NOISE_GENERATOR_INPUT_DETAIL,
	NOISE_GENERATOR_INPUT_ROUGHNESS,
	NOISE_GENERATOR_INPUT_LACUNARITY,
	NOISE_GENERATOR_INPUT_DISTORTION,
	NOISE_GENERATOR_INPUT_SEED,
};

enum {
	NOISE_GENERATOR_OUTPUT_FACTOR,
	NOISE_GENERATOR_OUTPUT_COLOR,
};

enum {
	NOISE_GENERATOR_BUTTON_MODE,
	NOISE_GENERATOR_BUTTON_TILEABLE,
};

// One value per mode, in the order of the Mode list. The count is the last entry: a new mode goes before it.
enum {
	NOISE_GENERATOR_MODE_PERLIN,
	NOISE_GENERATOR_MODE_COUNT,
};

// A mode: its name in the Mode list, the rows it hides (bit n of a mask = input, output or setting n of the node) and
// the function that returns the shader expression of an output (Factor: float, Color: float3).
typedef struct noise_generator_mode {
	char *name;
	i32   hidden_inputs;
	i32   hidden_outputs;
	i32   hidden_buttons;
	char *(*emit)(ui_node_t *node, i32 output);
} noise_generator_mode_t;

static noise_generator_mode_t noise_generator_modes[NOISE_GENERATOR_MODE_COUNT];

static void noise_generator_mode_set(i32 mode, char *name, i32 hidden_inputs, i32 hidden_outputs, i32 hidden_buttons,
                                     char *(*emit)(ui_node_t *node, i32 output)) {
	noise_generator_modes[mode].name           = name;
	noise_generator_modes[mode].hidden_inputs  = hidden_inputs;
	noise_generator_modes[mode].hidden_outputs = hidden_outputs;
	noise_generator_modes[mode].hidden_buttons = hidden_buttons;
	noise_generator_modes[mode].emit           = emit;
}

static f32 noise_generator_node_button(ui_node_t *node, i32 index) {
	return node->buttons->buffer[index]->default_value->buffer[0];
}

static i32 noise_generator_node_mode(ui_node_t *node) {
	i32 mode = (i32)noise_generator_node_button(node, NOISE_GENERATOR_BUTTON_MODE);
	return mode >= 0 && mode < NOISE_GENERATOR_MODE_COUNT ? mode : 0;
}

// Perlin shows every row of the node.
static char *noise_generator_perlin_emit(ui_node_t *node, i32 output) {
	node_shader_add_function(parser_material_kong, str_noise_generator_perlin);
	char *co         = parser_material_get_coord(node);
	char *scale      = parser_material_parse_value_input(node->inputs->buffer[NOISE_GENERATOR_INPUT_SCALE], false);
	char *detail     = parser_material_parse_value_input(node->inputs->buffer[NOISE_GENERATOR_INPUT_DETAIL], false);
	char *roughness  = parser_material_parse_value_input(node->inputs->buffer[NOISE_GENERATOR_INPUT_ROUGHNESS], false);
	char *lacunarity = parser_material_parse_value_input(node->inputs->buffer[NOISE_GENERATOR_INPUT_LACUNARITY], false);
	char *distortion = parser_material_parse_value_input(node->inputs->buffer[NOISE_GENERATOR_INPUT_DISTORTION], false);
	char *seed       = parser_material_parse_value_input(node->inputs->buffer[NOISE_GENERATOR_INPUT_SEED], false);
	char *tileable   = noise_generator_node_button(node, NOISE_GENERATOR_BUTTON_TILEABLE) > 0.5 ? "1.0" : "0.0";
	return string_tmp("perlin_%s(%s, %s, %s, %s, %s, %s, %s, %s)", output == NOISE_GENERATOR_OUTPUT_FACTOR ? "factor" : "color", co, scale, detail,
	                  roughness, lacunarity, distortion, tileable, seed);
}

static void noise_generator_modes_init() {
	noise_generator_mode_set(NOISE_GENERATOR_MODE_PERLIN, _tr("Perlin"), 0, 0, 0, noise_generator_perlin_emit);
}

static bool noise_generator_node_item_hidden_for_mode(i32 mode, ui_node_item_t kind, i32 index) {
	if (mode < 0 || mode >= NOISE_GENERATOR_MODE_COUNT || index < 0 || index > 30) {
		return false;
	}
	i32 mask = kind == UI_NODE_ITEM_INPUT ? noise_generator_modes[mode].hidden_inputs : kind == UI_NODE_ITEM_OUTPUT ? noise_generator_modes[mode].hidden_outputs : noise_generator_modes[mode].hidden_buttons;
	return ((mask >> index) & 1) != 0;
}

static bool noise_generator_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	return noise_generator_node_item_hidden_for_mode(noise_generator_node_mode(node), kind, index);
}

char *noise_generator_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	i32 mode = noise_generator_node_mode(node);
	if (socket == node->outputs->buffer[NOISE_GENERATOR_OUTPUT_COLOR]) {
		return noise_generator_modes[mode].emit(node, NOISE_GENERATOR_OUTPUT_COLOR);
	}
	return string_tmp("(float3(1.0, 1.0, 1.0) * %s)", noise_generator_modes[mode].emit(node, NOISE_GENERATOR_OUTPUT_FACTOR));
}

char *noise_generator_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	i32 mode = noise_generator_node_mode(node);
	if (socket == node->outputs->buffer[NOISE_GENERATOR_OUTPUT_COLOR]) {
		return string_tmp("%s.x", noise_generator_modes[mode].emit(node, NOISE_GENERATOR_OUTPUT_COLOR));
	}
	return noise_generator_modes[mode].emit(node, NOISE_GENERATOR_OUTPUT_FACTOR);
}

static ui_node_socket_t *noise_generator_node_value_input(char *name, f32 value, f32 min, f32 max) {
	return ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                     .node_id       = 0,
	                                     .name          = name,
	                                     .type          = "VALUE",
	                                     .color         = 0xffa1a1a1,
	                                     .default_value = f32_array_create_x(value),
	                                     .min           = min,
	                                     .max           = max,
	                                     .precision     = 100,
	                                     .display       = 0});
}

void noise_generator_node_init() {
	noise_generator_modes_init();
	char *mode_items = "";
	for (i32 i = 0; i < NOISE_GENERATOR_MODE_COUNT; ++i) {
		mode_items = i == 0 ? string_tmp("%s", noise_generator_modes[i].name) : string_tmp("%s\n%s", mode_items, noise_generator_modes[i].name);
	}

	ui_node_t *noise_generator_node_def = ALLOC_INIT(
	    ui_node_t, {.id     = 0,
	                .name   = _tr("Noise Generator"),
	                .type   = "TEX_NOISE_GENERATOR",
	                .x      = 0,
	                .y      = 0,
	                .color  = 0xff4982a0,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                                      .node_id       = 0,
	                                                      .name          = _tr("Vector"),
	                                                      .type          = "VECTOR",
	                                                      .color         = 0xff6363c7,
	                                                      .default_value = f32_array_create_xyz(0.0, 0.0, 0.0),
	                                                      .min           = 0.0,
	                                                      .max           = 1.0,
	                                                      .precision     = 100,
	                                                      .display       = 0}),
	                        noise_generator_node_value_input(_tr("Scale"), 5.0, 0.0, 256.0),
	                        noise_generator_node_value_input(_tr("Detail"), 0.0, 0.0, 8.0),
	                        noise_generator_node_value_input(_tr("Roughness"), 0.5, 0.0, 1.0),
	                        noise_generator_node_value_input(_tr("Lacunarity"), 2.0, 0.0, 10.0),
	                        noise_generator_node_value_input(_tr("Distortion"), 0.0, -10.0, 10.0),
	                        noise_generator_node_value_input(_tr("Seed"), 0.0, 0.0, 1000.0),
	                    },
	                    7),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                                      .node_id       = 0,
	                                                      .name          = _tr("Factor"),
	                                                      .type          = "VALUE",
	                                                      .color         = 0xffa1a1a1,
	                                                      .default_value = f32_array_create_x(0.5),
	                                                      .min           = 0.0,
	                                                      .max           = 1.0,
	                                                      .precision     = 100,
	                                                      .display       = 0}),
	                        ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                                      .node_id       = 0,
	                                                      .name          = _tr("Color"),
	                                                      .type          = "RGBA",
	                                                      .color         = 0xffc7c729,
	                                                      .default_value = f32_array_create_xyzw(0.5, 0.5, 0.5, 1.0),
	                                                      .min           = 0.0,
	                                                      .max           = 1.0,
	                                                      .precision     = 100,
	                                                      .display       = 0}),
	                    },
	                    2),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        ALLOC_INIT(ui_node_button_t, {.name          = _tr("Mode"),
	                                                      .type          = "ENUM",
	                                                      .output        = -1,
	                                                      .default_value = f32_array_create_x(0),
	                                                      .data          = u8_array_create_from_string(mode_items),
	                                                      .min           = 0.0,
	                                                      .max           = (f32)(NOISE_GENERATOR_MODE_COUNT - 1),
	                                                      .precision     = 100,
	                                                      .height        = 0}),
	                        ALLOC_INIT(ui_node_button_t, {.name          = _tr("Tileable"),
	                                                      .type          = "BOOL",
	                                                      .output        = -1,
	                                                      .default_value = f32_array_create_x(0),
	                                                      .data          = NULL,
	                                                      .min           = 0.0,
	                                                      .max           = 1.0,
	                                                      .precision     = 100,
	                                                      .height        = 0}),
	                    },
	                    2),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_texture, noise_generator_node_def);
	any_map_set(parser_material_node_vectors, "TEX_NOISE_GENERATOR", noise_generator_node_vector);
	any_map_set(parser_material_node_values, "TEX_NOISE_GENERATOR", noise_generator_node_value);
	any_map_set(ui_nodes_hidden_rules, "TEX_NOISE_GENERATOR", noise_generator_node_item_hidden);
}
