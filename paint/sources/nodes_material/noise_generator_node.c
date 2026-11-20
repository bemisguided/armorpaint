#include "../global.h"

// Noise Generator (Pattern menu): a tileable noise image. An image node (render/make_image_node.c) that generates its image
// itself: the capture shader writes the noise per pixel in image space (uv across the image, float, grey), the result is the
// baked texture at the Size setting, sampled by Tile UV like every other pattern node. No Vector input (image space rule).
// Next to Noise Texture (Blender style 3D procedural noise on the mesh, no resolution limit) it is the Substance style
// generator: modes, tileable by construction, composes with the other image nodes (specs/noise/NOISE_GENERATOR.md).
// The Mode setting decides the maths and which inputs and settings are shown (ui_nodes_hidden_rules, a linked socket is
// always shown). Modes are rows of a table (noise_generator_modes_init); the spec describes how to add one.
//
// The noise point is p = (uv, z): uv in 0..1 across the image, z = 0.3 + 0.4 * frac(Seed) a constant slice (the integer part of
// Seed picks the hash seed, the fraction moves the slice). Every octave multiplies x and y of p by its frequency (Scale = lattice
// cells across the image for the first octave), z stays: the slice is inside the first z cell of every octave, never on a lattice
// plane (z * frequency must not be an integer: a slice exactly on a z lattice plane is a 2D noise of the 12 gradients projected, which
// shows axis aligned streaks and triangular crests, seen with z = 0 and with z = 0.5 at Scale 5).
//
// Mode Perlin: gradient noise (improved Perlin, the 12 edge midpoint gradients), fade 6t^5 - 15t^4 + 10t^3.
//   - Lattice coordinates wrap at 65536 cells (a bound for the int conversion), or at the period when Tileable (at most 65536).
//   - Octave k >= 1 turns the lattice by the angle perlin_turn(k) (a table of fractions of a quarter turn, the first octave stays
//     axis aligned so Detail 0 is the plain noise): every octave of a gradient noise has its power on the lattice axes, equal
//     octaves stack that into a grid bias (faint horizontal and vertical streaks after an edge detect). The turned lookup is the
//     point (a x - b y, b x + a y) with (a, b) = f (cos, sin); Tileable rounds a and b to integers: the matrix [[a, -b], [b, a]]
//     maps the integer lattice onto the sublattice it spans, and the lattice cells are reduced modulo that sublattice
//     (perlin_wrap_i, perlin_wrap_j) so uv + (1, 0) and uv + (0, 1) land on the same cell: the image still repeats exactly
//     with no shorter repeat (the sublattice has index a^2 + b^2 = the number of cells of the image). The octave frequency becomes
//     sqrt(a^2 + b^2) (within a few percent of f at f >= 8). Not Tileable uses the exact cos and sin.
//   - Integer hash (no sin) of the lattice corner and the seed, so the CPU host test and the GPU agree.
//   - fBM like Noise Texture: Detail 0 is one octave, Detail d has floor(d) + 1 octaves and a fractional part blends
//     the next octave in, Roughness is the gain, Lacunarity the frequency step; the sum is divided by the sum of the
//     gains. Each octave has its own seed.
//   - Distortion displaces the point by a noise vector (in lattice units of Scale) before the fBM.
//   - Output 0.5 + 0.5 * clamp(PERLIN_NORM * n, -1, 1): about 0..1 centred on 0.5 (PERLIN_NORM = 1.3 is the measured
//     99.9 percentile of |n| (0.756) rounded so that the practical range of one octave fills 0..1: automation/host/noise_generator).
//   - Tileable (default on, the generator's Wrap Edges): the lattice repeats every round(Scale * Lacunarity^k) cells in
//     each octave k (Scale is snapped to an integer first), so the image repeats exactly: the value at uv 0 equals the value at uv 1.

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
	if ((k & 1) != 0) { su = 0.0 - u; } \
	float sv = v; \
	if ((k & 2) != 0) { sv = 0.0 - v; } \
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
float perlin_wrap_i(float i, float j, float a, float b, float n2) { \
	if (n2 < 0.5) { return perlin_wrap(i, 0.0); } \
	float u = floor((a * i + b * j) / n2); \
	float w = floor((0.0 - b * i + a * j) / n2); \
	return perlin_wrap(i - (a * u - b * w), 0.0); \
} \
float perlin_wrap_j(float i, float j, float a, float b, float n2) { \
	if (n2 < 0.5) { return perlin_wrap(j, 0.0); } \
	float u = floor((a * i + b * j) / n2); \
	float w = floor((0.0 - b * i + a * j) / n2); \
	return perlin_wrap(j - (b * u + a * w), 0.0); \
} \
float perlin_corner(float cx, float cy, float z, float dx, float dy, float dz, float a, float b, float n2, int seed) { \
	return perlin_grad(perlin_hash(perlin_wrap_i(cx, cy, a, b, n2), perlin_wrap_j(cx, cy, a, b, n2), z, seed), dx, dy, dz); \
} \
float perlin_noise_rot(float3 p, float a, float b, float n2, int seed) { \
	float3 fl = floor(p); \
	float fx = p.x - fl.x; \
	float fy = p.y - fl.y; \
	float fz = p.z - fl.z; \
	float z0 = perlin_wrap(fl.z, 0.0); \
	float z1 = perlin_wrap(fl.z + 1.0, 0.0); \
	float n000 = perlin_corner(fl.x, fl.y, z0, fx, fy, fz, a, b, n2, seed); \
	float n100 = perlin_corner(fl.x + 1.0, fl.y, z0, fx - 1.0, fy, fz, a, b, n2, seed); \
	float n010 = perlin_corner(fl.x, fl.y + 1.0, z0, fx, fy - 1.0, fz, a, b, n2, seed); \
	float n110 = perlin_corner(fl.x + 1.0, fl.y + 1.0, z0, fx - 1.0, fy - 1.0, fz, a, b, n2, seed); \
	float n001 = perlin_corner(fl.x, fl.y, z1, fx, fy, fz - 1.0, a, b, n2, seed); \
	float n101 = perlin_corner(fl.x + 1.0, fl.y, z1, fx - 1.0, fy, fz - 1.0, a, b, n2, seed); \
	float n011 = perlin_corner(fl.x, fl.y + 1.0, z1, fx, fy - 1.0, fz - 1.0, a, b, n2, seed); \
	float n111 = perlin_corner(fl.x + 1.0, fl.y + 1.0, z1, fx - 1.0, fy - 1.0, fz - 1.0, a, b, n2, seed); \
	float u = perlin_fade(fx); \
	float v = perlin_fade(fy); \
	float w = perlin_fade(fz); \
	return lerp(lerp(lerp(n000, n100, u), lerp(n010, n110, u), v), lerp(lerp(n001, n101, u), lerp(n011, n111, u), v), w); \
} \
float perlin_turn(int k) { \
	if (k == 1) { return 0.5; } \
	if (k == 2) { return 0.4; } \
	if (k == 3) { return 0.15; } \
	if (k == 4) { return 0.65; } \
	if (k == 5) { return 0.3; } \
	if (k == 6) { return 0.8; } \
	if (k == 7) { return 0.05; } \
	if (k == 8) { return 0.55; } \
	if (k == 9) { return 0.9; } \
	return 0.0; \
} \
float perlin_octave(float3 p, float freq, float tileable, int seed, int k) { \
	float f = freq; \
	float period = 0.0; \
	if (tileable > 0.5) { \
		f = max(1.0, floor(freq + 0.5)); \
		period = f; \
	} \
	if (k == 0) { return perlin_noise_3d(float3(p.x * f, p.y * f, p.z), period, seed); } \
	float ang = perlin_turn(k) * 1.5707963; \
	float a = f * cos(ang); \
	float b = f * sin(ang); \
	float n2 = 0.0; \
	if (tileable > 0.5) { \
		a = floor(a + 0.5); \
		b = floor(b + 0.5); \
		n2 = a * a + b * b; \
	} \
	return perlin_noise_rot(float3(a * p.x - b * p.y, b * p.x + a * p.y, p.z), a, b, n2, seed); \
} \
float perlin_signed(float3 p, float freq, float tileable, int seed) { \
	return clamp(perlin_octave(p, freq, tileable, seed, 0) * 1.3, -1.0, 1.0); \
} \
float perlin_fbm(float3 p, float scale, float detail, float roughness, float lacunarity, float tileable, int seed) { \
	float freq = perlin_base_scale(scale, tileable); \
	float amp = 1.0; \
	float maxamp = 0.0; \
	float sum = 0.0; \
	int n = int(clamp(detail, 0.0, 8.0)); \
	for (int ii = 0; ii <= n; ii += 1) { \
		sum = sum + amp * perlin_octave(p, freq, tileable, seed + ii * 31, ii); \
		maxamp = maxamp + amp; \
		amp = amp * roughness; \
		freq = freq * lacunarity; \
	} \
	float rmd = clamp(detail, 0.0, 8.0) - floor(clamp(detail, 0.0, 8.0)); \
	if (rmd > 0.0) { \
		float t = perlin_octave(p, freq, tileable, seed + (n + 1) * 31, n + 1); \
		return lerp(sum / maxamp, (sum + t * amp) / (maxamp + amp), rmd); \
	} \
	return sum / maxamp; \
} \
float perlin_factor(float3 p, float scale, float detail, float roughness, float lacunarity, float distortion, float tileable, float seed) { \
	int sd = int(floor(seed)); \
	float3 q = p; \
	if (distortion != 0.0) { \
		float se = perlin_base_scale(scale, tileable); \
		q = p + distortion / se * float3(perlin_signed(p, se, tileable, sd + 101), perlin_signed(p, se, tileable, sd + 202), perlin_signed(p, se, tileable, sd + 303)); \
	} \
	float n = perlin_fbm(q, scale, detail, roughness, lacunarity, tileable, sd); \
	return 0.5 + 0.5 * clamp(n * 1.3, -1.0, 1.0); \
} \
float perlin_image(float u, float v, float scale, float detail, float roughness, float lacunarity, float distortion, float tileable, float seed) { \
	float z = 0.3 + 0.4 * (seed - floor(seed)); \
	return perlin_factor(float3(u, v, z), scale, detail, roughness, lacunarity, distortion, tileable, seed); \
} \
";

// Input socket indices (all in the Parameters section)
enum {
	NOISE_GENERATOR_INPUT_SCALE,
	NOISE_GENERATOR_INPUT_DETAIL,
	NOISE_GENERATOR_INPUT_ROUGHNESS,
	NOISE_GENERATOR_INPUT_LACUNARITY,
	NOISE_GENERATOR_INPUT_DISTORTION,
	NOISE_GENERATOR_INPUT_SEED,
};

// Node button indices (settings): Size, the state row, then the General section
enum {
	NOISE_GENERATOR_BUTTON_SIZE = 0, // The state row is button 1
	NOISE_GENERATOR_BUTTON_MODE = 2,
	NOISE_GENERATOR_BUTTON_TILEABLE = 3,
};

// One value per mode, in the order of the Mode list. The count is the last entry: a new mode goes before it.
enum {
	NOISE_GENERATOR_MODE_PERLIN,
	NOISE_GENERATOR_MODE_COUNT,
};

// A mode: its name in the Mode list, the rows it hides (bit n of a mask = input or setting n of the node) and the function
// that returns the shader expression (float, 0..1) of the pixel; it is written in the capture shader, tex_coord is the image uv.
typedef struct noise_generator_mode {
	char *name;
	i32   hidden_inputs;
	i32   hidden_buttons;
	char *(*emit)(ui_node_t *node);
} noise_generator_mode_t;

static noise_generator_mode_t noise_generator_modes[NOISE_GENERATOR_MODE_COUNT];

static void noise_generator_mode_set(i32 mode, char *name, i32 hidden_inputs, i32 hidden_buttons, char *(*emit)(ui_node_t *node)) {
	noise_generator_modes[mode].name           = name;
	noise_generator_modes[mode].hidden_inputs  = hidden_inputs;
	noise_generator_modes[mode].hidden_buttons = hidden_buttons;
	noise_generator_modes[mode].emit           = emit;
}

static i32 noise_generator_node_mode(ui_node_t *node) {
	i32 mode = (i32)make_image_node_button(node, NOISE_GENERATOR_BUTTON_MODE);
	return mode >= 0 && mode < NOISE_GENERATOR_MODE_COUNT ? mode : 0;
}

// The input as a shader value: a literal, or per pixel when linked
static char *noise_generator_node_input(ui_node_t *node, i32 input) {
	if (input >= node->inputs->length) {
		return "0.0"; // A node saved with fewer inputs (make_image_node_reconcile completes it, this is the safety net)
	}
	return parser_material_parse_value_input(node->inputs->buffer[input], false);
}

// Perlin shows every row of the node.
static char *noise_generator_perlin_emit(ui_node_t *node) {
	node_shader_add_function(parser_material_kong, str_noise_generator_perlin);
	char *scale      = noise_generator_node_input(node, NOISE_GENERATOR_INPUT_SCALE);
	char *detail     = noise_generator_node_input(node, NOISE_GENERATOR_INPUT_DETAIL);
	char *roughness  = noise_generator_node_input(node, NOISE_GENERATOR_INPUT_ROUGHNESS);
	char *lacunarity = noise_generator_node_input(node, NOISE_GENERATOR_INPUT_LACUNARITY);
	char *distortion = noise_generator_node_input(node, NOISE_GENERATOR_INPUT_DISTORTION);
	char *seed       = noise_generator_node_input(node, NOISE_GENERATOR_INPUT_SEED);
	char *tileable   = make_image_node_button(node, NOISE_GENERATOR_BUTTON_TILEABLE) > 0.5 ? "1.0" : "0.0";
	return string_tmp("perlin_image(tex_coord.x, tex_coord.y, %s, %s, %s, %s, %s, %s, %s)", scale, detail, roughness, lacunarity, distortion, tileable, seed);
}

static void noise_generator_modes_init() {
	noise_generator_mode_set(NOISE_GENERATOR_MODE_PERLIN, _tr("Perlin"), 0, 0, noise_generator_perlin_emit);
}

static bool noise_generator_node_item_hidden_for_mode(i32 mode, ui_node_item_t kind, i32 index) {
	if (mode < 0 || mode >= NOISE_GENERATOR_MODE_COUNT || index < 0 || index > 30) {
		return false;
	}
	if (kind == UI_NODE_ITEM_OUTPUT) {
		return false; // One output
	}
	i32 mask = kind == UI_NODE_ITEM_INPUT ? noise_generator_modes[mode].hidden_inputs : noise_generator_modes[mode].hidden_buttons;
	return ((mask >> index) & 1) != 0;
}

static bool noise_generator_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	return noise_generator_node_item_hidden_for_mode(noise_generator_node_mode(node), kind, index);
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 noise_generator_node_size(ui_node_t *node) {
	return make_image_node_size(node, NOISE_GENERATOR_BUTTON_SIZE);
}

// The capture is the generator: the noise of the mode per pixel, float, the same value in R, G and B
static char *noise_generator_node_capture(ui_node_t *node) {
	char *value = noise_generator_modes[noise_generator_node_mode(node)].emit(node);
	char *var   = string_tmp("%s_noise", parser_material_store_var_name(node));
	parser_material_write(parser_material_kong, string_tmp("float %s = %s;", var, value));
	return string_tmp("float4(%s, %s, %s, 1.0)", var, var, var);
}

// Everything is in the shader (and so in the signature of the captured pixels): no processing parameters
static void noise_generator_node_params(ui_node_t *node, void *params) {
}

// The result is the capture: copied into the RGBA result
static i32 noise_generator_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	memcpy(result, bytes, (size_t)size * size * 4 * sizeof(f32));
	return 0;
}

static image_node_type_t noise_generator_node_image_type = {
    .type          = "TEX_NOISE_GENERATOR",
    .result_format = GPU_TEXTURE_FORMAT_RGBA128, // Float: a Bump node or a blur reads the slopes of the noise, half floats would band
    .params_size   = 0,
    .size          = noise_generator_node_size,
    .capture       = noise_generator_node_capture,
    .capture_float = true,
    .smooth_sample = true,
    .params        = noise_generator_node_params,
    .process       = noise_generator_node_process,
};

char *noise_generator_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	char *v = string_tmp("%s.r", make_image_node_sample(node));
	return string_tmp("float3(%s, %s, %s)", v, v, v);
}

char *noise_generator_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

void noise_generator_node_init() {
	noise_generator_modes_init();
	char *mode_items = "";
	for (i32 i = 0; i < NOISE_GENERATOR_MODE_COUNT; ++i) {
		mode_items = i == 0 ? string_tmp("%s", noise_generator_modes[i].name) : string_tmp("%s\n%s", mode_items, noise_generator_modes[i].name);
	}
	u32 value_color = 0xffa1a1a1;

	ui_node_t *noise_generator_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Noise Generator"),
	                .type   = "TEX_NOISE_GENERATOR",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Scale"), "VALUE", value_color, f32_array_create_x(5.0), 0.0, 256.0, 100),
	                        make_image_node_socket(_tr("Detail"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 8.0, 100),
	                        make_image_node_socket(_tr("Roughness"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Lacunarity"), "VALUE", value_color, f32_array_create_x(2.0), 0.0, 10.0, 100),
	                        make_image_node_socket(_tr("Distortion"), "VALUE", value_color, f32_array_create_x(0.0), -10.0, 10.0, 100),
	                        make_image_node_socket(_tr("Seed"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1000.0, 100),
	                    },
	                    6),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Size"), "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS),
	                        make_image_node_state_button(),
	                        make_image_node_setting(_tr("Mode"), "ENUM", 0, mode_items),
	                        make_image_node_setting(_tr("Tileable"), "BOOL", 1, NULL),
	                    },
	                    4),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, noise_generator_node_def);
	any_map_set(parser_material_node_vectors, "TEX_NOISE_GENERATOR", noise_generator_node_vector);
	any_map_set(parser_material_node_values, "TEX_NOISE_GENERATOR", noise_generator_node_value);
	any_map_set(ui_nodes_hidden_rules, "TEX_NOISE_GENERATOR", noise_generator_node_item_hidden);
	make_image_node_sections(noise_generator_node_def, NULL, 0, 0); // General: Mode and Tileable; Parameters: all inputs
	make_image_node_register(&noise_generator_node_image_type);
}
