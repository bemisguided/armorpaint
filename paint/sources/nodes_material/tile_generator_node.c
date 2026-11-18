#include "../global.h"

// Tile Generator (Image menu): a grid of tiles, each one a copy of a pattern with random scale, rotation, offset and
// brightness, composed into one image. An image node (render/make_image_node.c): every pattern input (Image, Image 2
// to Image 4, as many as Patterns says) is one capture of the node's image square, the tiles are drawn on the cpu
// into one RGBA64 texture per node:
// R = Image (the composed pattern grey), G = Position x, B = Position y (the uv inside the tile),
// A = ID (a random grey per tile, floored to 1/1024 so that Mask = A > 0).
// Tiles are drawn in row order, a later tile covers an earlier one where they overlap (Scale above 1, jitter), and
// nothing is clipped to the cell. The image wraps, so a tile leaving one side comes back on the other and the result
// repeats seamlessly. The texture is sampled like an Image Texture at tex_coord, repeating (Tile UV places it on the uv).

#define TILE_GENERATOR_ID_FLOOR  1024.0
#define TILE_GENERATOR_MAX_TILES 64
#define TILE_GENERATOR_MAX_PATTERNS 4

// Input socket indices
enum {
	TILE_GENERATOR_INPUT_IMAGE             = 0, // Image 2 to 4 follow
	TILE_GENERATOR_INPUT_TILES_X           = 4,
	TILE_GENERATOR_INPUT_TILES_Y           = 5,
	TILE_GENERATOR_INPUT_SCALE             = 6,
	TILE_GENERATOR_INPUT_SCALE_RANDOM      = 7,
	TILE_GENERATOR_INPUT_ROTATION          = 8,
	TILE_GENERATOR_INPUT_ROTATION_RANDOM   = 9,
	TILE_GENERATOR_INPUT_OFFSET_RANDOM     = 10,
	TILE_GENERATOR_INPUT_BRIGHTNESS_RANDOM = 11,
	TILE_GENERATOR_INPUT_SEED              = 12,
};

// Node button indices (settings)
enum {
	TILE_GENERATOR_BUTTON_SIZE         = 0,
	TILE_GENERATOR_BUTTON_PATTERNS     = 1,
	TILE_GENERATOR_BUTTON_PATTERN_MODE = 2,
};

// Output socket indices
enum {
	TILE_GENERATOR_OUTPUT_IMAGE    = 0,
	TILE_GENERATOR_OUTPUT_MASK     = 1,
	TILE_GENERATOR_OUTPUT_ID       = 2,
	TILE_GENERATOR_OUTPUT_POSITION = 3,
};

// Settings and scalar inputs of the processing, only 4 byte members (hashed as bytes)
typedef struct tile_generator_params {
	i32 patterns;
	i32 cycle; // Pattern Mode: 0 Random, 1 Cycle
	i32 tiles_x;
	i32 tiles_y;
	f32 scale;
	f32 scale_random;
	f32 rotation;
	f32 rotation_random;
	f32 offset_random;
	f32 brightness_random;
	u32 seed;
} tile_generator_params_t;

static f32 tile_generator_node_button(ui_node_t *node, i32 i) {
	return node->buttons->buffer[i]->default_value->buffer[0];
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 tile_generator_node_size(ui_node_t *node) {
	i32 i = (i32)tile_generator_node_button(node, TILE_GENERATOR_BUTTON_SIZE);
	return 512 << (i < 0 ? 0 : i > 3 ? 3 : i);
}

// Patterns 1 to 4
static i32 tile_generator_node_patterns(ui_node_t *node) {
	i32 i = (i32)tile_generator_node_button(node, TILE_GENERATOR_BUTTON_PATTERNS);
	return 1 + (i < 0 ? 0 : i > TILE_GENERATOR_MAX_PATTERNS - 1 ? TILE_GENERATOR_MAX_PATTERNS - 1 : i);
}

static i32 tile_generator_node_capture_count(ui_node_t *node) {
	return tile_generator_node_patterns(node);
}

// Captured per pixel: the grey of the pattern input of the capture being written, over the whole image square
static char *tile_generator_node_capture(ui_node_t *node) {
	char *v = parser_material_parse_value_input(node->inputs->buffer[TILE_GENERATOR_INPUT_IMAGE + make_image_node_capture_index()], false);
	return string_tmp("float4(%s, %s, %s, 1.0)", v, v, v);
}

static f32 tile_generator_node_clamp(f32 v, f32 lo, f32 hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

static void tile_generator_node_params(ui_node_t *node, void *params) {
	tile_generator_params_t *p = params;
	p->patterns                = tile_generator_node_patterns(node);
	p->cycle                   = tile_generator_node_button(node, TILE_GENERATOR_BUTTON_PATTERN_MODE) != 0.0;
	f32 tx                     = make_image_node_input_value(node, TILE_GENERATOR_INPUT_TILES_X);
	f32 ty                     = make_image_node_input_value(node, TILE_GENERATOR_INPUT_TILES_Y);
	p->tiles_x                 = (i32)tile_generator_node_clamp(math_floor(tx + 0.5), 1.0, TILE_GENERATOR_MAX_TILES);
	p->tiles_y                 = (i32)tile_generator_node_clamp(math_floor(ty + 0.5), 1.0, TILE_GENERATOR_MAX_TILES);
	p->scale                   = tile_generator_node_clamp(make_image_node_input_value(node, TILE_GENERATOR_INPUT_SCALE), 0.0, 2.0);
	p->scale_random            = tile_generator_node_clamp(make_image_node_input_value(node, TILE_GENERATOR_INPUT_SCALE_RANDOM), 0.0, 1.0);
	p->rotation                = make_image_node_input_value(node, TILE_GENERATOR_INPUT_ROTATION);
	p->rotation_random         = tile_generator_node_clamp(make_image_node_input_value(node, TILE_GENERATOR_INPUT_ROTATION_RANDOM), 0.0, 1.0);
	p->offset_random           = tile_generator_node_clamp(make_image_node_input_value(node, TILE_GENERATOR_INPUT_OFFSET_RANDOM), 0.0, 1.0);
	p->brightness_random       = tile_generator_node_clamp(make_image_node_input_value(node, TILE_GENERATOR_INPUT_BRIGHTNESS_RANDOM), 0.0, 1.0);
	f32 seed                   = make_image_node_input_value(node, TILE_GENERATOR_INPUT_SEED);
	p->seed                    = (u32)math_floor(seed > 0.0 ? seed : 0.0);
}

static u32 tile_generator_node_mix(u32 h, u32 v) {
	h ^= v;
	h *= 0x9E3779B1u;
	h ^= h >> 15;
	h *= 0x85EBCA77u;
	h ^= h >> 13;
	return h;
}

// Random number [0, 1) number i of the tile
static f32 tile_generator_node_random(u32 tile_hash, u32 i) {
	return (f32)(tile_generator_node_mix(tile_hash, i + 1) & 0xffffff) / 16777216.0;
}

// Draws one tile (pattern at pixels, size * size RGBA8 grey) over result, wrapping at the image edges
static void tile_generator_node_draw_tile(u8 *pattern, i32 size, f32 *result, f32 center_x, f32 center_y, f32 half_x, f32 half_y, f32 angle, f32 brightness,
                                          f32 id) {
	f32 c      = math_cos(angle);
	f32 s      = math_sin(angle);
	f32 radius = math_sqrt(half_x * half_x + half_y * half_y);
	i32 x0     = (i32)math_floor((center_x - radius) * size);
	i32 x1     = (i32)math_ceil((center_x + radius) * size);
	i32 y0     = (i32)math_floor((center_y - radius) * size);
	i32 y1     = (i32)math_ceil((center_y + radius) * size);
	if (x1 - x0 > size) {
		x1 = x0 + size; // A tile wider than the image covers each pixel once
	}
	if (y1 - y0 > size) {
		y1 = y0 + size;
	}
	for (i32 iy = y0; iy < y1; ++iy) {
		f32 dy = (iy + 0.5) / size - center_y;
		i32 wy = ((iy % size) + size) % size;
		for (i32 ix = x0; ix < x1; ++ix) {
			f32 dx = (ix + 0.5) / size - center_x;
			f32 lx = dx * c + dy * s;
			f32 ly = -dx * s + dy * c;
			if (lx < -half_x || lx > half_x || ly < -half_y || ly > half_y) {
				continue;
			}
			f32 u  = lx / half_x * 0.5 + 0.5;
			f32 v  = ly / half_y * 0.5 + 0.5;
			i32 pu = (i32)tile_generator_node_clamp(math_floor(u * size), 0.0, size - 1);
			i32 pv = (i32)tile_generator_node_clamp(math_floor(v * size), 0.0, size - 1);
			f32 g  = pattern[((size_t)pv * size + pu) * 4] / 255.0 * brightness;
			i32 wx = ((ix % size) + size) % size;
			f32 *r = &result[((size_t)wy * size + wx) * 4];
			r[0]   = tile_generator_node_clamp(g, 0.0, 1.0);
			r[1]   = u;
			r[2]   = v;
			r[3]   = id;
		}
	}
}

// Draws the tiles over the captured patterns; returns the number of tiles
static i32 tile_generator_node_process(u8 *pixels, i32 size, void *params, f32 *result) {
	tile_generator_params_t *p   = params;
	size_t                   one = (size_t)size * size * 4;
	for (i32 cy = 0; cy < p->tiles_y; ++cy) {
		for (i32 cx = 0; cx < p->tiles_x; ++cx) {
			u32 h = tile_generator_node_mix(tile_generator_node_mix(tile_generator_node_mix(p->seed + 1, (u32)cx), (u32)cy), 7);
			f32 r_offset_x    = tile_generator_node_random(h, 0);
			f32 r_offset_y    = tile_generator_node_random(h, 1);
			f32 r_scale       = tile_generator_node_random(h, 2);
			f32 r_rotation    = tile_generator_node_random(h, 3);
			f32 r_brightness  = tile_generator_node_random(h, 4);
			f32 r_pattern     = tile_generator_node_random(h, 5);
			f32 r_id          = tile_generator_node_random(h, 6);

			i32 k = p->cycle ? (cy * p->tiles_x + cx) % p->patterns : (i32)math_floor(r_pattern * p->patterns);
			k     = k < 0 ? 0 : k > p->patterns - 1 ? p->patterns - 1 : k;

			f32 center_x   = (cx + 0.5 + (r_offset_x - 0.5) * p->offset_random) / p->tiles_x;
			f32 center_y   = (cy + 0.5 + (r_offset_y - 0.5) * p->offset_random) / p->tiles_y;
			f32 scale      = p->scale * (1.0 - p->scale_random * r_scale);
			f32 half_x     = 0.5 * scale / p->tiles_x;
			f32 half_y     = 0.5 * scale / p->tiles_y;
			f32 angle      = (p->rotation + p->rotation_random * (r_rotation * 2.0 - 1.0) * 0.5) * 2.0 * math_pi();
			f32 brightness = 1.0 - p->brightness_random * r_brightness;
			f32 id         = 1.0 / TILE_GENERATOR_ID_FLOOR + r_id * (1.0 - 1.0 / TILE_GENERATOR_ID_FLOOR);
			if (half_x <= 0.0 || half_y <= 0.0) {
				continue;
			}
			tile_generator_node_draw_tile(pixels + one * k, size, result, center_x, center_y, half_x, half_y, angle, brightness, id);
		}
	}
	return p->tiles_x * p->tiles_y;
}

static image_node_type_t tile_generator_node_image_type = {
    .type          = "TEX_TILE_GENERATOR",
    .result_format = GPU_TEXTURE_FORMAT_RGBA64,
    .params_size   = sizeof(tile_generator_params_t),
    .size          = tile_generator_node_size,
    .capture       = tile_generator_node_capture,
    .capture_count = tile_generator_node_capture_count,
    .params        = tile_generator_node_params,
    .process       = tile_generator_node_process,
};

char *tile_generator_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	char *tg = make_image_node_sample(node);
	i32   i  = array_index_of(node->outputs, socket);
	if (i == TILE_GENERATOR_OUTPUT_MASK) {
		return string_tmp("clamp(%s.a * %s, 0.0, 1.0)", tg, parser_material_vec1(TILE_GENERATOR_ID_FLOOR));
	}
	if (i == TILE_GENERATOR_OUTPUT_ID) {
		return string_tmp("%s.a", tg);
	}
	if (i == TILE_GENERATOR_OUTPUT_POSITION) {
		return string_tmp("%s.g", tg);
	}
	return string_tmp("%s.r", tg);
}

char *tile_generator_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	if (array_index_of(node->outputs, socket) == TILE_GENERATOR_OUTPUT_POSITION) {
		char *tg = make_image_node_sample(node);
		return string_tmp("float3(%s.g, %s.b, 0.0)", tg, tg);
	}
	return parser_material_to_vec3(tile_generator_node_value(node, socket));
}

static ui_node_socket_t *tile_generator_node_socket(char *name, char *type, u32 color, f32_array_t *default_value, f32 max, f32 precision) {
	return ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                     .node_id       = 0,
	                                     .name          = name,
	                                     .type          = type,
	                                     .color         = color,
	                                     .default_value = default_value,
	                                     .min           = 0.0,
	                                     .max           = max,
	                                     .precision     = precision,
	                                     .display       = 0});
}

static ui_node_button_t *tile_generator_node_setting(char *name, char *type, f32 default_value, char *data) {
	return ALLOC_INIT(ui_node_button_t, {.name          = name,
	                                     .type          = type,
	                                     .output        = -1,
	                                     .default_value = f32_array_create_x(default_value),
	                                     .data          = data != NULL ? u8_array_create_from_string(data) : NULL,
	                                     .min           = 0.0,
	                                     .max           = 1.0,
	                                     .precision     = 100,
	                                     .height        = 0});
}

void tile_generator_node_init() {
	u32 value_color  = 0xffa1a1a1;
	u32 vector_color = 0xff6363c7;
	u32 image_color  = NODES_MATERIAL_IMAGE_COLOR;

	// The pattern inputs default to 1, a full square tile
	ui_node_t *tile_generator_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Tile Generator"),
	                .type   = "TEX_TILE_GENERATOR",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        tile_generator_node_socket(_tr("Image"), "VALUE", image_color, f32_array_create_x(1.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Image 2"), "VALUE", image_color, f32_array_create_x(1.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Image 3"), "VALUE", image_color, f32_array_create_x(1.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Image 4"), "VALUE", image_color, f32_array_create_x(1.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Tiles X"), "VALUE", value_color, f32_array_create_x(4.0), TILE_GENERATOR_MAX_TILES, 1),
	                        tile_generator_node_socket(_tr("Tiles Y"), "VALUE", value_color, f32_array_create_x(4.0), TILE_GENERATOR_MAX_TILES, 1),
	                        tile_generator_node_socket(_tr("Scale"), "VALUE", value_color, f32_array_create_x(0.9), 2.0, 100),
	                        tile_generator_node_socket(_tr("Scale Random"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Rotation"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Rotation Random"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Offset Random"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Brightness Random"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Seed"), "VALUE", value_color, f32_array_create_x(0.0), 1000.0, 1),
	                    },
	                    13),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        tile_generator_node_socket(_tr("Image"), "VALUE", image_color, f32_array_create_x(0.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Mask"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("ID"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Position"), "VECTOR", vector_color, f32_array_create_xyz(0.0, 0.0, 0.0), 1.0, 100),
	                    },
	                    4),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        tile_generator_node_setting(_tr("Size"), "ENUM", 1, "512\n1024\n2048\n4096"),
	                        tile_generator_node_setting(_tr("Patterns"), "ENUM", 0, "1\n2\n3\n4"),
	                        tile_generator_node_setting(_tr("Pattern Mode"), "ENUM", 0, "Random\nCycle"),
	                        make_image_node_state_button(),
	                    },
	                    4),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_image, tile_generator_node_def);
	any_map_set(parser_material_node_values, "TEX_TILE_GENERATOR", tile_generator_node_value);
	any_map_set(parser_material_node_vectors, "TEX_TILE_GENERATOR", tile_generator_node_vector);
	make_image_node_register(&tile_generator_node_image_type);
}
