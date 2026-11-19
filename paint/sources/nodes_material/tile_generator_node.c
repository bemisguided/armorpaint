#include "../global.h"

// Tile Generator (Pattern menu): a grid of tiles, each one a copy of a pattern with random scale, rotation, offset and
// brightness, composed into one image. An image node (render/make_image_node.c): every pattern input (Image, Image 2
// to Image 4, as many as Patterns says) is one colour capture (32 bit float) of the node's image square, read through the
// shared cpu sampler with the Filter setting (Linear, Cubic, Nearest) and the tiles are drawn on the cpu into two RGBA64
// textures per node (second_result), because a colour image leaves no channel for Position:
// first texture:  R, G, B = Image (the composed pattern colour), A = ID (a random grey per tile, floored to 1/1024 so that
//                 Mask = A > 0),
// second texture: R = Position x, G = Position y (the uv inside the tile).
// Per tile maps (Mask, Scale Map, Rotation Map, Color Map, Pattern Map): grey images in image space read once per tile at
// the centre of its grid cell, so they are captured at a small fixed size (TILE_GENERATOR_MAP_SIZE, two captures: the maps
// are packed R = B = first, G = second, A = third) and read bilinear. Color Map multiplies the colour of the tile.
// Tiles are drawn in row order, a later tile covers an earlier one where they overlap (Scale above 1, jitter), and
// nothing is clipped to the cell. With Wrap Edges (default on) the image wraps, so a tile leaving one side comes back on the
// other and the result repeats seamlessly; with it off the tiles are clipped at the image borders (Mask, ID and Position too).
// The texture is sampled like an Image Texture at tex_coord, repeating (Tile UV places it on the uv).
// Memory at Size 4096: the first texture 134 MB and the second 134 MB on the gpu (8 bytes per pixel each), the cpu result
// 2 * 268 MB while it is drawn, and each pattern capture 268 MB (16 bytes per pixel).

#define TILE_GENERATOR_ID_FLOOR  1024.0
#define TILE_GENERATOR_MAX_TILES 64
#define TILE_GENERATOR_MAX_PATTERNS 4
#define TILE_GENERATOR_MAP_SIZE     64

// Input socket indices
enum {
	TILE_GENERATOR_INPUT_IMAGE             = 0, // Image 2 to 4 follow
	TILE_GENERATOR_INPUT_MASK              = 4,
	TILE_GENERATOR_INPUT_SCALE_MAP         = 5,
	TILE_GENERATOR_INPUT_ROTATION_MAP      = 6,
	TILE_GENERATOR_INPUT_COLOR_MAP         = 7,
	TILE_GENERATOR_INPUT_PATTERN_MAP       = 8,
	TILE_GENERATOR_INPUT_TILES_X           = 9,
	TILE_GENERATOR_INPUT_TILES_Y           = 10,
	TILE_GENERATOR_INPUT_SCALE             = 11,
	TILE_GENERATOR_INPUT_SCALE_RANDOM      = 12,
	TILE_GENERATOR_INPUT_ROTATION          = 13,
	TILE_GENERATOR_INPUT_ROTATION_RANDOM   = 14,
	TILE_GENERATOR_INPUT_OFFSET_RANDOM     = 15,
	TILE_GENERATOR_INPUT_BRIGHTNESS_RANDOM = 16,
	TILE_GENERATOR_INPUT_SEED              = 17,
};

// Node button indices (settings)
enum {
	TILE_GENERATOR_BUTTON_SIZE         = 0, // Button 1 is the state row
	TILE_GENERATOR_BUTTON_PATTERNS     = 2,
	TILE_GENERATOR_BUTTON_PATTERN_MODE = 3,
	TILE_GENERATOR_BUTTON_WRAP_EDGES   = 4,
	TILE_GENERATOR_BUTTON_FILTER       = 5,
};

// Filter setting values (the order of the combo); the sampler takes IMAGE_FILTER_*, mapped in tile_generator_node_params
enum {
	TILE_GENERATOR_FILTER_LINEAR,
	TILE_GENERATOR_FILTER_CUBIC,
	TILE_GENERATOR_FILTER_NEAREST,
};

// Output socket indices
enum {
	TILE_GENERATOR_OUTPUT_IMAGE    = 0,
	TILE_GENERATOR_OUTPUT_MASK     = 1,
	TILE_GENERATOR_OUTPUT_ID       = 2,
	TILE_GENERATOR_OUTPUT_POSITION = 3,
};

// Pattern Mode
enum {
	TILE_GENERATOR_MODE_RANDOM = 0,
	TILE_GENERATOR_MODE_CYCLE  = 1,
	TILE_GENERATOR_MODE_BY_MAP = 2,
};

// Settings and scalar inputs of the processing, only 4 byte members (hashed as bytes)
typedef struct tile_generator_params {
	i32 patterns;
	i32 mode; // Pattern Mode
	i32 wrap; // Wrap Edges, 1 or 0
	i32 filter; // IMAGE_FILTER_*
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

// The pattern captures, then two small captures of the per tile maps
static i32 tile_generator_node_capture_count(ui_node_t *node) {
	return tile_generator_node_patterns(node) + 2;
}

static i32 tile_generator_node_capture_size(ui_node_t *node, i32 k) {
	return k < tile_generator_node_patterns(node) ? tile_generator_node_size(node) : TILE_GENERATOR_MAP_SIZE;
}

static char *tile_generator_node_input(ui_node_t *node, i32 input) {
	return parser_material_parse_value_input(node->inputs->buffer[input], false);
}

// Captured per pixel: the colour of the pattern input of the capture being written, over the whole image square.
// The map captures pack three maps (R = B = a, G = b, A = c), grey images read as float.
static char *tile_generator_node_capture(ui_node_t *node) {
	i32 k        = make_image_node_capture_index();
	i32 patterns = tile_generator_node_patterns(node);
	if (k == patterns) {
		char *mask     = tile_generator_node_input(node, TILE_GENERATOR_INPUT_MASK);
		char *scale    = tile_generator_node_input(node, TILE_GENERATOR_INPUT_SCALE_MAP);
		char *rotation = tile_generator_node_input(node, TILE_GENERATOR_INPUT_ROTATION_MAP);
		return string_tmp("float4(%s, %s, %s, %s)", mask, scale, mask, rotation);
	}
	if (k == patterns + 1) {
		char *color   = tile_generator_node_input(node, TILE_GENERATOR_INPUT_COLOR_MAP);
		char *pattern = tile_generator_node_input(node, TILE_GENERATOR_INPUT_PATTERN_MAP);
		return string_tmp("float4(%s, %s, %s, 1.0)", color, pattern, color);
	}
	char *image = parser_material_parse_vector_input(node->inputs->buffer[TILE_GENERATOR_INPUT_IMAGE + k]);
	return string_tmp("float4(%s, 1.0)", image);
}

static f32 tile_generator_node_clamp(f32 v, f32 lo, f32 hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

static void tile_generator_node_params(ui_node_t *node, void *params) {
	tile_generator_params_t *p = params;
	p->patterns                = tile_generator_node_patterns(node);
	i32 mode                   = (i32)tile_generator_node_button(node, TILE_GENERATOR_BUTTON_PATTERN_MODE);
	p->mode                    = mode < 0 ? 0 : mode > TILE_GENERATOR_MODE_BY_MAP ? TILE_GENERATOR_MODE_BY_MAP : mode;
	p->wrap                    = tile_generator_node_button(node, TILE_GENERATOR_BUTTON_WRAP_EDGES) != 0.0 ? 1 : 0;
	i32 filter                 = (i32)tile_generator_node_button(node, TILE_GENERATOR_BUTTON_FILTER);
	p->filter                  = filter == TILE_GENERATOR_FILTER_CUBIC ? IMAGE_FILTER_CUBIC : filter == TILE_GENERATOR_FILTER_NEAREST ? IMAGE_FILTER_NEAREST : IMAGE_FILTER_LINEAR;
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

// Draws one tile (pattern at pixels, size * size RGBA float) over result (colour and ID) and position (the uv in the tile),
// wrapping at the image edges or clipped at them
static void tile_generator_node_draw_tile(f32 *pattern, i32 size, f32 *result, f32 *position, f32 center_x, f32 center_y, f32 half_x, f32 half_y, f32 angle,
                                          f32 brightness, f32 id, i32 filter, bool wrap) {
	f32 c      = math_cos(angle);
	f32 s      = math_sin(angle);
	f32 radius = math_sqrt(half_x * half_x + half_y * half_y);
	i32 x0     = (i32)math_floor((center_x - radius) * size);
	i32 x1     = (i32)math_ceil((center_x + radius) * size);
	i32 y0     = (i32)math_floor((center_y - radius) * size);
	i32 y1     = (i32)math_ceil((center_y + radius) * size);
	if (!wrap) {
		x0 = x0 < 0 ? 0 : x0; // The part outside the image is dropped
		y0 = y0 < 0 ? 0 : y0;
		x1 = x1 > size ? size : x1;
		y1 = y1 > size ? size : y1;
	}
	// With wrap the whole window around the tile is walked and every index wrapped, also a tile larger than the image
	// (Scale is at most 2, so the window is at most about three images wide)
	for (i32 iy = y0; iy < y1; ++iy) {
		f32 dy = (iy + 0.5) / size - center_y;
		i32 wy = wrap ? ((iy % size) + size) % size : iy;
		for (i32 ix = x0; ix < x1; ++ix) {
			f32 dx = (ix + 0.5) / size - center_x;
			f32 lx = dx * c + dy * s;
			f32 ly = -dx * s + dy * c;
			if (lx < -half_x || lx > half_x || ly < -half_y || ly > half_y) {
				continue;
			}
			f32 u  = lx / half_x * 0.5 + 0.5;
			f32 v  = ly / half_y * 0.5 + 0.5;
			f32 c[4];
			make_image_node_sample_rgba(pattern, size, u, v, filter, IMAGE_TILING_CLAMP, c);
			i32 wx   = wrap ? ((ix % size) + size) % size : ix;
			size_t o = ((size_t)wy * size + wx) * 4;
			f32 *r   = &result[o];
			r[0]     = tile_generator_node_clamp(c[0] * brightness, 0.0, 1.0);
			r[1]     = tile_generator_node_clamp(c[1] * brightness, 0.0, 1.0);
			r[2]     = tile_generator_node_clamp(c[2] * brightness, 0.0, 1.0);
			r[3]     = id;
			f32 *q   = &position[o];
			q[0]     = u;
			q[1]     = v;
			q[2]     = 0.0;
			q[3]     = 1.0;
		}
	}
}

// Channel c (0 R, 1 G, 3 A) of a per tile map capture at the uv, bilinear, clamped at the border
static f32 tile_generator_node_map(f32 *map, f32 u, f32 v, i32 c) {
	i32 n  = TILE_GENERATOR_MAP_SIZE;
	f32 x  = tile_generator_node_clamp(u * n - 0.5, 0.0, n - 1.0);
	f32 y  = tile_generator_node_clamp(v * n - 0.5, 0.0, n - 1.0);
	i32 x0 = (i32)math_floor(x);
	i32 y0 = (i32)math_floor(y);
	i32 x1 = x0 + 1 < n ? x0 + 1 : x0;
	i32 y1 = y0 + 1 < n ? y0 + 1 : y0;
	f32 fx = x - x0;
	f32 fy = y - y0;
	f32 a  = map[((size_t)y0 * n + x0) * 4 + c];
	f32 b  = map[((size_t)y0 * n + x1) * 4 + c];
	f32 d  = map[((size_t)y1 * n + x0) * 4 + c];
	f32 e  = map[((size_t)y1 * n + x1) * 4 + c];
	return (a * (1.0 - fx) + b * fx) * (1.0 - fy) + (d * (1.0 - fx) + e * fx) * fy;
}

// Draws the tiles over the captured patterns; returns the number of tiles
static i32 tile_generator_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	tile_generator_params_t *p        = params;
	f32                     *pixels   = (f32 *)bytes;                       // Float captures
	size_t                   one      = (size_t)size * size * 4;
	f32                     *position = result + one;                       // The second result
	f32                     *maps_a   = pixels + one * p->patterns;         // R = B Mask, G Scale Map, A Rotation Map
	f32                     *maps_b   = maps_a + (size_t)TILE_GENERATOR_MAP_SIZE * TILE_GENERATOR_MAP_SIZE * 4; // R = B Color Map, G Pattern Map
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

			// The per tile maps at the centre of the grid cell
			f32 mu         = (cx + 0.5) / p->tiles_x;
			f32 mv         = (cy + 0.5) / p->tiles_y;
			f32 mask       = tile_generator_node_map(maps_a, mu, mv, 0);
			f32 scale_map  = tile_generator_node_map(maps_a, mu, mv, 1);
			f32 rotate_map = tile_generator_node_map(maps_a, mu, mv, 3);
			f32 color_map  = tile_generator_node_map(maps_b, mu, mv, 0);
			f32 pattern_map = tile_generator_node_map(maps_b, mu, mv, 1);
			if (mask < 0.5) {
				continue; // A hole
			}

			i32 k = p->mode == TILE_GENERATOR_MODE_CYCLE ? (cy * p->tiles_x + cx) % p->patterns :
			        p->mode == TILE_GENERATOR_MODE_BY_MAP ? (i32)math_floor(pattern_map * p->patterns) : (i32)math_floor(r_pattern * p->patterns);
			k     = k < 0 ? 0 : k > p->patterns - 1 ? p->patterns - 1 : k;

			f32 center_x   = (cx + 0.5 + (r_offset_x - 0.5) * p->offset_random) / p->tiles_x;
			f32 center_y   = (cy + 0.5 + (r_offset_y - 0.5) * p->offset_random) / p->tiles_y;
			f32 scale      = p->scale * (1.0 - p->scale_random * r_scale) * scale_map;
			f32 half_x     = 0.5 * scale / p->tiles_x;
			f32 half_y     = 0.5 * scale / p->tiles_y;
			f32 angle      = (p->rotation + rotate_map + p->rotation_random * (r_rotation * 2.0 - 1.0) * 0.5) * 2.0 * math_pi();
			f32 brightness = (1.0 - p->brightness_random * r_brightness) * color_map;
			f32 id         = 1.0 / TILE_GENERATOR_ID_FLOOR + r_id * (1.0 - 1.0 / TILE_GENERATOR_ID_FLOOR);
			if (half_x <= 0.0 || half_y <= 0.0) {
				continue;
			}
			tile_generator_node_draw_tile(pixels + one * k, size, result, position, center_x, center_y, half_x, half_y, angle, brightness, id, p->filter,
			                              p->wrap != 0);
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
    .capture_float = true,
    .second_result = true,
    .capture_count = tile_generator_node_capture_count,
    .capture_size  = tile_generator_node_capture_size,
    .params        = tile_generator_node_params,
    .process       = tile_generator_node_process,
};

char *tile_generator_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	i32 i = array_index_of(node->outputs, socket);
	if (i == TILE_GENERATOR_OUTPUT_POSITION) {
		return string_tmp("%s.r", make_image_node_sample_second(node));
	}
	char *tg = make_image_node_sample(node);
	if (i == TILE_GENERATOR_OUTPUT_MASK) {
		return string_tmp("clamp(%s.a * %s, 0.0, 1.0)", tg, parser_material_vec1(TILE_GENERATOR_ID_FLOOR));
	}
	if (i == TILE_GENERATOR_OUTPUT_ID) {
		return string_tmp("%s.a", tg);
	}
	return string_tmp("%s.r", tg);
}

char *tile_generator_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	i32 i = array_index_of(node->outputs, socket);
	if (i == TILE_GENERATOR_OUTPUT_POSITION) {
		char *pos = make_image_node_sample_second(node);
		return string_tmp("float3(%s.r, %s.g, 0.0)", pos, pos);
	}
	if (i == TILE_GENERATOR_OUTPUT_IMAGE) {
		return string_tmp("%s.rgb", make_image_node_sample(node));
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
	                        tile_generator_node_socket(_tr("Image"), "RGBA", image_color, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Image 2"), "RGBA", image_color, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Image 3"), "RGBA", image_color, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Image 4"), "RGBA", image_color, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Mask"), "VALUE", image_color, f32_array_create_x(1.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Scale Map"), "VALUE", image_color, f32_array_create_x(1.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Rotation Map"), "VALUE", image_color, f32_array_create_x(0.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Color Map"), "VALUE", image_color, f32_array_create_x(1.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Pattern Map"), "VALUE", image_color, f32_array_create_x(0.0), 1.0, 100),
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
	                    18),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        tile_generator_node_socket(_tr("Image"), "RGBA", image_color, f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Mask"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("ID"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                        tile_generator_node_socket(_tr("Position"), "VECTOR", vector_color, f32_array_create_xyz(0.0, 0.0, 0.0), 1.0, 100),
	                    },
	                    4),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        tile_generator_node_setting(_tr("Size"), "ENUM", 1, "512\n1024\n2048\n4096"),
	                        make_image_node_state_button(),
	                        tile_generator_node_setting(_tr("Patterns"), "ENUM", 0, "1\n2\n3\n4"),
	                        tile_generator_node_setting(_tr("Pattern Mode"), "ENUM", 0, "Random\nCycle\nBy Map"),
	                        tile_generator_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                        tile_generator_node_setting(_tr("Filter"), "ENUM", TILE_GENERATOR_FILTER_LINEAR, "Linear\nCubic\nNearest"),
	                    },
	                    6),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, tile_generator_node_def);
	any_map_set(parser_material_node_values, "TEX_TILE_GENERATOR", tile_generator_node_value);
	any_map_set(parser_material_node_vectors, "TEX_TILE_GENERATOR", tile_generator_node_vector);
	make_image_node_register(&tile_generator_node_image_type);
}
