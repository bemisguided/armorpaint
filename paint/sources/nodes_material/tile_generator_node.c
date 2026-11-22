#include "../global.h"

// Tile Generator (Pattern menu): a grid of tiles, each one a copy of a pattern with random scale, rotation, offset and
// brightness, composed into one image. A GPU baked image node (render/make_image_node.c, result_by_reference + gpu_bake, the section GPU baked
// nodes of armorpaint-admin specs/image-nodes/IMAGE_NODES.md): every pattern input (Pattern 1, Pattern 2 to Pattern 4, as many as Patterns says)
// is one colour capture (32 bit float) of the node's image square, drawn into a render target and never read back; one pass over two render
// targets (RGBA64, MRT) composes the tiles, read with the Filter setting (Linear, Nearest), see the GPU pass below. The bake is the render target
// itself: no readback of the patterns (268 MB each at Size 4096), no cpu composition, no upload. Two RGBA64 textures per node (second_result), because
// a colour image leaves no channel for Position:
// first texture:  R, G, B = Image (the composed pattern colour), A = ID (a random grey per tile, floored to 1/1024 so that
//                 Mask = A > 0),
// second texture: R = Position x, G = Position y (the uv inside the tile).
// Per tile maps (Mask, Scale Map, Rotation Map, Color Map, Pattern Map): grey images in image space read once per tile at
// the centre of its grid cell, so they are captured at a small fixed size (TILE_GENERATOR_MAP_SIZE, two captures: the maps
// are packed R = B = first, G = second, A = third) and read bilinear. Color Map multiplies the colour of the tile.
// Tiles are drawn in row order and nothing is clipped to the cell. Where tiles overlap (Scale above 1, Offset Random,
// Rotation) the Blend setting decides the Image: Max (default) keeps the larger value of every colour channel, so the black
// background of a pattern's square does not cut a tile drawn earlier; Replace lets the later tile cover the earlier one, its
// whole square included. Mask, ID and Position always keep the footprint of the tile (the whole square of the later tile, by
// design: they describe the tile, not the pattern's shape). With Wrap Edges (default on) the image wraps, so a tile leaving one side comes back on the
// other and the result repeats seamlessly; with it off the tiles are clipped at the image borders (Mask, ID and Position too).
// The texture is sampled like an Image Texture at tex_coord, repeating (Tile UV places it on the uv).
// Rotation follows the rotation convention of all image nodes (armorpaint-admin specs/image-nodes/ROTATION_CONVENTION.md):
// degrees, a positive angle turns a tile clockwise as seen in the editor view (y down), about the centre of the tile.
// A tile's angle is Rotation + Rotation Map * 360 + Rotation Random * (-180..180) degrees: Rotation Map is a grey value
// where 1 stands for one full turn (the whole Rotation slider range), Rotation Random is a fraction of the full circle
// (0 none, 1 anywhere in -180..180), like the other Random amounts.
// The result is the same as the former cpu composition (a tile drawn over the canvas in row order) up to float rounding: the reference copy of that
// code is in armorpaint-admin automation/host/tile_generator_gpu with the measured differences.
// Memory at Size 4096: the two results 134 MB each on the gpu (8 bytes per pixel), and each pattern capture 268 MB (16 bytes per pixel) while the pass runs.

#define TILE_GENERATOR_ID_FLOOR  1024.0
#define TILE_GENERATOR_MAX_TILES 64
#define TILE_GENERATOR_MAX_PATTERNS 4
#define TILE_GENERATOR_MAP_SIZE     64

// Input socket indices
enum {
	TILE_GENERATOR_INPUT_PATTERN           = 0, // Pattern 2 to 4 follow
	TILE_GENERATOR_INPUT_MASK              = 4,
	TILE_GENERATOR_INPUT_SCALE_MAP         = 5,
	TILE_GENERATOR_INPUT_ROTATION_MAP      = 6,
	TILE_GENERATOR_INPUT_COLOR_MAP         = 7,
	TILE_GENERATOR_INPUT_PATTERN_MAP       = 8,
	TILE_GENERATOR_INPUT_TILES_X           = 9,
	TILE_GENERATOR_INPUT_TILES_Y           = 10,
	TILE_GENERATOR_INPUT_SCALE             = 11,
	TILE_GENERATOR_INPUT_ROTATION          = 12,
	TILE_GENERATOR_INPUT_SCALE_RANDOM      = 13,
	TILE_GENERATOR_INPUT_ROTATION_RANDOM   = 14,
	TILE_GENERATOR_INPUT_OFFSET_RANDOM     = 15,
	TILE_GENERATOR_INPUT_BRIGHTNESS_RANDOM = 16,
	TILE_GENERATOR_INPUT_SEED              = 17,
};

// Own sections of the node: a contiguous range of inputs each (Pattern 1 to 4 belong to the General section), see make_image_node_sections.
// The input order puts every group in one run: Maps 4 to 8, Layout 9 to 12, Randomness 13 to 17.
enum {
	TILE_GENERATOR_SECTION_MAPS_FIRST        = TILE_GENERATOR_INPUT_MASK,
	TILE_GENERATOR_SECTION_MAPS_COUNT        = TILE_GENERATOR_INPUT_PATTERN_MAP - TILE_GENERATOR_INPUT_MASK + 1,
	TILE_GENERATOR_SECTION_LAYOUT_FIRST      = TILE_GENERATOR_INPUT_TILES_X,
	TILE_GENERATOR_SECTION_LAYOUT_COUNT      = TILE_GENERATOR_INPUT_ROTATION - TILE_GENERATOR_INPUT_TILES_X + 1,
	TILE_GENERATOR_SECTION_RANDOMNESS_FIRST  = TILE_GENERATOR_INPUT_SCALE_RANDOM,
	TILE_GENERATOR_SECTION_RANDOMNESS_COUNT  = TILE_GENERATOR_INPUT_SEED - TILE_GENERATOR_INPUT_SCALE_RANDOM + 1,
};

// Node button indices (settings)
enum {
	TILE_GENERATOR_BUTTON_SIZE         = 0, // Button 1 is the state row
	TILE_GENERATOR_BUTTON_PATTERNS     = 2,
	TILE_GENERATOR_BUTTON_PATTERN_MODE = 3,
	TILE_GENERATOR_BUTTON_WRAP_EDGES   = 4,
	TILE_GENERATOR_BUTTON_FILTER       = 5,
	TILE_GENERATOR_BUTTON_BLEND        = 6, // Appended last: scripts and saves address the buttons by index
};

// Filter setting values (the order of the combo); the sampler takes IMAGE_FILTER_*, mapped in tile_generator_node_params. Cubic was
// removed (2026-11-21: indistinguishable from Linear at 3.2x the cost, it overshoots 0..1), so Nearest moved from 2 to 1; a node saved
// with the old Nearest (2) is out of range and reads as Linear (the combo shows no text until it is set again)
enum {
	TILE_GENERATOR_FILTER_LINEAR,
	TILE_GENERATOR_FILTER_NEAREST,
};

// Blend setting values (the order of the combo): how a tile is drawn over the earlier ones
enum {
	TILE_GENERATOR_BLEND_MAX,
	TILE_GENERATOR_BLEND_REPLACE,
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
	i32 blend;  // TILE_GENERATOR_BLEND_*
	i32 tiles_x;
	i32 tiles_y;
	f32 scale;
	f32 scale_random;
	f32 rotation;        // Degrees, positive turns a tile clockwise on screen
	f32 rotation_random; // Fraction of the full circle, 0 to 1
	f32 offset_random;
	f32 brightness_random;
	u32 seed;
} tile_generator_params_t;

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 tile_generator_node_size(ui_node_t *node) {
	return make_image_node_size(node, TILE_GENERATOR_BUTTON_SIZE);
}

// Patterns 1 to 4
static i32 tile_generator_node_patterns(ui_node_t *node) {
	i32 i = (i32)make_image_node_button(node, TILE_GENERATOR_BUTTON_PATTERNS);
	return 1 + (i < 0 ? 0 : i > TILE_GENERATOR_MAX_PATTERNS - 1 ? TILE_GENERATOR_MAX_PATTERNS - 1 : i);
}

// Pattern k + 1 above the Patterns setting is not used, and Pattern Map only matters when Pattern Mode is By Map: hidden
// in the node (a linked socket still shows, see ui_node_item_hidden). Settings and indices are untouched.
static bool tile_generator_node_input_hidden(i32 patterns, i32 mode, i32 input) {
	if (input > TILE_GENERATOR_INPUT_PATTERN && input < TILE_GENERATOR_INPUT_PATTERN + TILE_GENERATOR_MAX_PATTERNS) {
		return input - TILE_GENERATOR_INPUT_PATTERN >= patterns;
	}
	return input == TILE_GENERATOR_INPUT_PATTERN_MAP && mode != TILE_GENERATOR_MODE_BY_MAP;
}

static bool tile_generator_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	if (kind != UI_NODE_ITEM_INPUT) {
		return false;
	}
	i32 mode = (i32)make_image_node_button(node, TILE_GENERATOR_BUTTON_PATTERN_MODE);
	return tile_generator_node_input_hidden(tile_generator_node_patterns(node), mode, index);
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
	char *image = parser_material_parse_vector_input(node->inputs->buffer[TILE_GENERATOR_INPUT_PATTERN + k]);
	return string_tmp("float4(%s, 1.0)", image);
}

static f32 tile_generator_node_clamp(f32 v, f32 lo, f32 hi) {
	return !(v > lo) ? lo : v > hi ? hi : v; // Not a number gives lo
}

static void tile_generator_node_params(ui_node_t *node, void *params) {
	tile_generator_params_t *p = params;
	p->patterns                = tile_generator_node_patterns(node);
	i32 mode                   = (i32)make_image_node_button(node, TILE_GENERATOR_BUTTON_PATTERN_MODE);
	p->mode                    = mode < 0 ? 0 : mode > TILE_GENERATOR_MODE_BY_MAP ? TILE_GENERATOR_MODE_BY_MAP : mode;
	p->wrap                    = make_image_node_button(node, TILE_GENERATOR_BUTTON_WRAP_EDGES) != 0.0 ? 1 : 0;
	i32 filter                 = (i32)make_image_node_button(node, TILE_GENERATOR_BUTTON_FILTER);
	p->filter                  = filter == TILE_GENERATOR_FILTER_NEAREST ? IMAGE_FILTER_NEAREST : IMAGE_FILTER_LINEAR;
	i32 blend                  = (i32)make_image_node_button(node, TILE_GENERATOR_BUTTON_BLEND);
	p->blend                   = blend == TILE_GENERATOR_BLEND_REPLACE ? TILE_GENERATOR_BLEND_REPLACE : TILE_GENERATOR_BLEND_MAX;
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
	p->seed                    = (u32)math_floor(tile_generator_node_clamp(seed, 0.0, 4000000000.0));
}

// ---- The GPU pass ----
// One full screen pass over two render targets (MRT): every output pixel is a gather of the tiles that cover it. The candidates are the grid cells
// around the pixel (the reach of a tile is bounded: Scale at most 2, Offset Random at most half a cell, any rotation, so the half diagonal of a tile
// is a known radius; a thin anisotropic grid reaches further along its long axis, the loop bounds come from that radius). Per candidate the tile
// is rebuilt from its hash (the same integer hash as always), the per tile maps are read bilinear at the cell centre, and the pixel is tested against
// the rotated rectangle. The order of the drawing decides the result where tiles overlap, so the tile that is drawn last wins Replace, ID and
// Position: its key is the tile index in row order, then (a tile larger than the image reaches the pixel through several copies when Wrap Edges is on)
// the copy that the walk of the window visits last. Blend Max takes the largest value of every channel over all covering tiles.

#define TILE_GENERATOR_REACH_MARGIN 0.001

// Integer hash of a tile and the random numbers of it (bit identical to the former cpu code: uint arithmetic wraps the same way), kong source
static char *tile_generator_node_gpu_functions =
    "uint tile_mix(uint h, uint v) {\n"
    "	h = h ^ v;\n"
    "	h = h * (uint(40503) * uint(65536) + uint(31153));\n" // 0x9E3779B1
    "	h = h ^ (h >> uint(15));\n"
    "	h = h * (uint(34283) * uint(65536) + uint(51831));\n" // 0x85EBCA77
    "	h = h ^ (h >> uint(13));\n"
    "	return h;\n"
    "}\n"
    "float tile_random(uint h, uint i) {\n"
    "	return float(tile_mix(h, i + uint(1)) & uint(16777215)) / 16777216.0;\n"
    "}\n";

// The kong text of one pattern read: every @ is replaced by the number of the pattern (names and blocks of their own per pattern)
static char *tile_generator_node_gpu_number(char *text, i32 k) {
	buffer_t sb;
	string_buffer_init(&sb);
	char digit[2] = {(char)('0' + k), 0};
	char one[2]   = {0, 0};
	for (i32 i = 0; text[i] != 0; ++i) {
		if (text[i] == '@') {
			string_buffer_append(&sb, digit);
		}
		else {
			one[0] = text[i];
			string_buffer_append(&sb, one);
		}
	}
	char *r = string_copy(string_buffer_get(&sb));
	string_buffer_free(&sb);
	return r;
}

// Kong text that reads pattern k at (su, sv) (fractions of the pattern, clamped at the border) into pc, with the filter of the setting: Nearest is one
// texel, Linear four loads at texel centres weighted by hand (a 32 bit float texture is not guaranteed filterable on every backend)
static char *tile_generator_node_gpu_read(i32 k, bool nearest) {
	char *text = nearest ?
	    "if (sk > @.5 - 1.0 && sk < @.5) {\n"
	    "	float nx@ = clamp(floor(su * size), 0.0, size - 1.0);\n"
	    "	float ny@ = clamp(floor(sv * size), 0.0, size - 1.0);\n"
	    "	pc = sample_lod(tile_p@, sampler_linear, float2((nx@ + 0.5) / size, (ny@ + 0.5) / size), 0.0);\n"
	    "}\n" :
	    "if (sk > @.5 - 1.0 && sk < @.5) {\n"
	    "	float fx@ = su * size - 0.5;\n"
	    "	float fy@ = sv * size - 0.5;\n"
	    "	float bx@ = floor(fx@);\n"
	    "	float by@ = floor(fy@);\n"
	    "	float wx@ = fx@ - bx@;\n"
	    "	float wy@ = fy@ - by@;\n"
	    "	if (wx@ < 0.0001) { wx@ = 0.0; }\n"
	    "	if (wx@ > 0.9999) { wx@ = 1.0; }\n"
	    "	if (wy@ < 0.0001) { wy@ = 0.0; }\n"
	    "	if (wy@ > 0.9999) { wy@ = 1.0; }\n"
	    "	float x0@ = clamp(bx@, 0.0, size - 1.0);\n"
	    "	float x1@ = clamp(bx@ + 1.0, 0.0, size - 1.0);\n"
	    "	float y0@ = clamp(by@, 0.0, size - 1.0);\n"
	    "	float y1@ = clamp(by@ + 1.0, 0.0, size - 1.0);\n"
	    "	float4 a@ = sample_lod(tile_p@, sampler_linear, float2((x0@ + 0.5) / size, (y0@ + 0.5) / size), 0.0);\n"
	    "	float4 b@ = sample_lod(tile_p@, sampler_linear, float2((x1@ + 0.5) / size, (y0@ + 0.5) / size), 0.0);\n"
	    "	float4 d@ = sample_lod(tile_p@, sampler_linear, float2((x0@ + 0.5) / size, (y1@ + 0.5) / size), 0.0);\n"
	    "	float4 e@ = sample_lod(tile_p@, sampler_linear, float2((x1@ + 0.5) / size, (y1@ + 0.5) / size), 0.0);\n"
	    "	pc = (a@ * (1.0 - wx@) + b@ * wx@) * (1.0 - wy@) + (d@ * (1.0 - wx@) + e@ * wx@) * wy@;\n"
	    "}\n";
	return tile_generator_node_gpu_number(text, k);
}

// The fragment code of the pass for these settings. Wrap Edges, Filter, Blend and Patterns are written into the text (they are in the params, so the
// signature of the bake carries them); the numbers of the layout and the randomness are uniforms k0 to k3 (see tile_generator_node_gpu_bake)
static char *tile_generator_node_gpu_body(i32 patterns, bool wrap, bool nearest, bool blend_max) {
	buffer_t sb;
	string_buffer_init(&sb);
	string_buffer_append(&sb, "float tile_x = constants.k0.x;\n"
	                          "float tile_y = constants.k0.y;\n"
	                          "float size = constants.k0.z;\n"
	                          "float patterns = constants.k0.w;\n"
	                          "float scale_p = constants.k1.x;\n"
	                          "float scale_r = constants.k1.y;\n"
	                          "float rotation = constants.k1.z;\n"
	                          "float rotation_r = constants.k1.w;\n"
	                          "float offset_r = constants.k2.x;\n"
	                          "float bright_r = constants.k2.y;\n"
	                          "float reach_x = constants.k2.z;\n"
	                          "float reach_y = constants.k2.w;\n"
	                          "uint seed = uint(constants.k3.x) * uint(65536) + uint(constants.k3.y);\n"
	                          "float mode = constants.k3.z;\n"
	                          "float radius_max = constants.k3.w;\n"
	                          "int ix = int(floor(tex_coord.x * size));\n"
	                          "int iy = int(floor(tex_coord.y * size));\n"
	                          "float pcx = (float(ix) + 0.5) / size * tile_x;\n"
	                          "float pcy = (float(iy) + 0.5) / size * tile_y;\n"
	                          "int xlo = int(ceil(pcx - reach_x - 0.5));\n"
	                          "int xhi = int(floor(pcx + reach_x - 0.5));\n"
	                          "int ylo = int(ceil(pcy - reach_y - 0.5));\n"
	                          "int yhi = int(floor(pcy + reach_y - 0.5));\n");
	if (!wrap) {
		string_buffer_append(&sb, "xlo = max(xlo, 0);\n"
		                          "ylo = max(ylo, 0);\n"
		                          "xhi = min(xhi, int(tile_x) - 1);\n"
		                          "yhi = min(yhi, int(tile_y) - 1);\n");
	}
	string_buffer_append(&sb, "float best_key = -1.0;\n"
	                          "float best_u = 0.0;\n"
	                          "float best_v = 0.0;\n"
	                          "float best_id = 0.0;\n"
	                          "float best_b = 1.0;\n"
	                          "float best_k = 0.0;\n"
	                          "float3 col = float3(0.0, 0.0, 0.0);\n"
	                          "float4 pc = float4(0.0, 0.0, 0.0, 0.0);\n"
	                          "float su = 0.0;\n"
	                          "float sv = 0.0;\n"
	                          "float sk = 0.0;\n"
	                          "float sb = 1.0;\n"
	                          "for (int vy = ylo; vy <= yhi; vy += 1) {\n"
	                          "	float fy = float(vy);\n");
	string_buffer_append(&sb, wrap ? "	float shy = floor(fy / tile_y);\n" : "	float shy = 0.0;\n");
	string_buffer_append(&sb, "	float cy = fy - shy * tile_y;\n"
	                          "	for (int vx = xlo; vx <= xhi; vx += 1) {\n"
	                          "		float fx = float(vx);\n");
	string_buffer_append(&sb, wrap ? "		float shx = floor(fx / tile_x);\n" : "		float shx = 0.0;\n");
	string_buffer_append(&sb, "		float cx = fx - shx * tile_x;\n"
	                          "		uint h = tile_mix(tile_mix(tile_mix(seed + uint(1), uint(cx)), uint(cy)), uint(7));\n"
	                          "		float r_offset_x = tile_random(h, uint(0));\n"
	                          "		float r_offset_y = tile_random(h, uint(1));\n"
	                          "		float center_x = (cx + 0.5 + (r_offset_x - 0.5) * offset_r) / tile_x;\n"
	                          "		float center_y = (cy + 0.5 + (r_offset_y - 0.5) * offset_r) / tile_y;\n"
	                          "		float dx = (float(ix) - shx * size + 0.5) / size - center_x;\n"
	                          "		float dy = (float(iy) - shy * size + 0.5) / size - center_y;\n"
	                          "		if (abs(dx) <= radius_max && abs(dy) <= radius_max) {\n"
	                          // The per tile maps at the centre of the grid cell, bilinear, clamped at the border
	                          "			float mu = (cx + 0.5) / tile_x;\n"
	                          "			float mv = (cy + 0.5) / tile_y;\n"
	                          "			float mx = clamp(mu * 64.0 - 0.5, 0.0, 63.0);\n"
	                          "			float my = clamp(mv * 64.0 - 0.5, 0.0, 63.0);\n"
	                          "			float mx0 = floor(mx);\n"
	                          "			float my0 = floor(my);\n"
	                          "			float mx1 = min(mx0 + 1.0, 63.0);\n"
	                          "			float my1 = min(my0 + 1.0, 63.0);\n"
	                          "			float mfx = mx - mx0;\n"
	                          "			float mfy = my - my0;\n"
	                          "			float4 ma0 = sample_lod(tile_ma, sampler_linear, float2((mx0 + 0.5) / 64.0, (my0 + 0.5) / 64.0), 0.0);\n"
	                          "			float4 ma1 = sample_lod(tile_ma, sampler_linear, float2((mx1 + 0.5) / 64.0, (my0 + 0.5) / 64.0), 0.0);\n"
	                          "			float4 ma2 = sample_lod(tile_ma, sampler_linear, float2((mx0 + 0.5) / 64.0, (my1 + 0.5) / 64.0), 0.0);\n"
	                          "			float4 ma3 = sample_lod(tile_ma, sampler_linear, float2((mx1 + 0.5) / 64.0, (my1 + 0.5) / 64.0), 0.0);\n"
	                          "			float4 mb0 = sample_lod(tile_mb, sampler_linear, float2((mx0 + 0.5) / 64.0, (my0 + 0.5) / 64.0), 0.0);\n"
	                          "			float4 mb1 = sample_lod(tile_mb, sampler_linear, float2((mx1 + 0.5) / 64.0, (my0 + 0.5) / 64.0), 0.0);\n"
	                          "			float4 mb2 = sample_lod(tile_mb, sampler_linear, float2((mx0 + 0.5) / 64.0, (my1 + 0.5) / 64.0), 0.0);\n"
	                          "			float4 mb3 = sample_lod(tile_mb, sampler_linear, float2((mx1 + 0.5) / 64.0, (my1 + 0.5) / 64.0), 0.0);\n"
	                          "			float4 map_a = (ma0 * (1.0 - mfx) + ma1 * mfx) * (1.0 - mfy) + (ma2 * (1.0 - mfx) + ma3 * mfx) * mfy;\n"
	                          "			float4 map_b = (mb0 * (1.0 - mfx) + mb1 * mfx) * (1.0 - mfy) + (mb2 * (1.0 - mfx) + mb3 * mfx) * mfy;\n"
	                          "			float scale_map = clamp(map_a.y, 0.0, 1.0);\n"
	                          "			float rotate_map = map_a.w;\n"
	                          "			float color_map = map_b.x;\n"
	                          "			float pattern_map = map_b.y;\n"
	                          "			if (map_a.x >= 0.5) {\n"
	                          "				float r_scale = tile_random(h, uint(2));\n"
	                          "				float r_rotation = tile_random(h, uint(3));\n"
	                          "				float r_brightness = tile_random(h, uint(4));\n"
	                          "				float r_pattern = tile_random(h, uint(5));\n"
	                          "				float r_id = tile_random(h, uint(6));\n"
	                          "				float tindex = cy * tile_x + cx;\n"
	                          "				float pick = floor(r_pattern * patterns);\n"
	                          "				if (mode > 1.5) { pick = floor(pattern_map * patterns); }\n"
	                          "				if (mode > 0.5 && mode < 1.5) { pick = tindex - floor(tindex / patterns) * patterns; }\n"
	                          "				pick = clamp(pick, 0.0, patterns - 1.0);\n"
	                          "				float scale = scale_p * (1.0 - scale_r * r_scale) * scale_map;\n"
	                          "				float half_x = 0.5 * scale / tile_x;\n"
	                          "				float half_y = 0.5 * scale / tile_y;\n"
	                          "				float angle = (rotation + rotate_map * 360.0 + rotation_r * (r_rotation * 2.0 - 1.0) * 180.0) * (3.14159265358979 / 180.0);\n"
	                          "				float cs = cos(angle);\n"
	                          "				float sn = sin(angle);\n"
	                          "				float lx = dx * cs + dy * sn;\n"
	                          "				float ly = 0.0 - dx * sn + dy * cs;\n"
	                          "				if (half_x > 0.0 && half_y > 0.0 && lx >= 0.0 - half_x && lx <= half_x && ly >= 0.0 - half_y && ly <= half_y) {\n"
	                          "					float u = lx / half_x * 0.5 + 0.5;\n"
	                          "					float v = ly / half_y * 0.5 + 0.5;\n"
	                          "					float bright = (1.0 - bright_r * r_brightness) * color_map;\n"
	                          // Row order of the tiles, then the copy that the walk of the window reaches last
	                          "					float key = ((cy * tile_x + cx) * 16.0 + (8.0 - shy)) * 16.0 + (8.0 - shx);\n");
	if (blend_max) {
		string_buffer_append(&sb, "					su = u;\n"
		                          "					sv = v;\n"
		                          "					sk = pick;\n");
		for (i32 k = 0; k < patterns; ++k) {
			string_buffer_append(&sb, tile_generator_node_gpu_read(k, nearest));
		}
		string_buffer_append(&sb, "					col = max(col, clamp(pc.rgb * bright, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)));\n");
	}
	string_buffer_append(&sb, "					if (key > best_key) {\n"
	                          "						best_key = key;\n"
	                          "						best_u = u;\n"
	                          "						best_v = v;\n"
	                          "						best_b = bright;\n"
	                          "						best_k = pick;\n"
	                          "						best_id = 0.0009765625 + r_id * (1.0 - 0.0009765625);\n"
	                          "					}\n"
	                          "				}\n"
	                          "			}\n"
	                          "		}\n"
	                          "	}\n"
	                          "}\n");
	if (!blend_max) {
		// Replace: only the tile that won is read
		string_buffer_append(&sb, "if (best_key >= 0.0) {\n"
		                          "	su = best_u;\n"
		                          "	sv = best_v;\n"
		                          "	sk = best_k;\n");
		for (i32 k = 0; k < patterns; ++k) {
			string_buffer_append(&sb, tile_generator_node_gpu_read(k, nearest));
		}
		string_buffer_append(&sb, "	col = clamp(pc.rgb * best_b, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));\n"
		                          "}\n");
	}
	string_buffer_append(&sb, "output[0] = float4(col, best_id);\n"
	                          "if (best_key >= 0.0) {\n"
	                          "	output[1] = float4(best_u, best_v, 0.0, 1.0);\n"
	                          "}\n"
	                          "else {\n"
	                          "	output[1] = float4(0.0, 0.0, 0.0, 0.0);\n"
	                          "}\n");
	char *body = string_copy(string_buffer_get(&sb));
	string_buffer_free(&sb);
	return body;
}

// The uniforms k0 to k3 of the pass (16 floats). Reach of a tile in cells: the half diagonal of the largest tile (Scale; the Scale Random, Scale Map and the
// clamp only make it smaller) plus the largest offset of its centre
static void tile_generator_node_gpu_uniforms(tile_generator_params_t *p, i32 size, f32 *u) {
	f32 radius = 0.5f * p->scale * (f32)math_sqrt(1.0 / ((f64)p->tiles_x * p->tiles_x) + 1.0 / ((f64)p->tiles_y * p->tiles_y));
	radius     = radius * 1.0001f + 0.00001f;
	f32 reach_x = 0.5f * p->offset_random + radius * p->tiles_x + TILE_GENERATOR_REACH_MARGIN;
	f32 reach_y = 0.5f * p->offset_random + radius * p->tiles_y + TILE_GENERATOR_REACH_MARGIN;
	f32 v[16]   = {(f32)p->tiles_x, (f32)p->tiles_y, (f32)size, (f32)p->patterns, // k0
	               p->scale, p->scale_random, p->rotation, p->rotation_random,      // k1
	               p->offset_random, p->brightness_random, reach_x, reach_y,        // k2
	               (f32)(p->seed >> 16), (f32)(p->seed & 0xffff), (f32)p->mode, radius}; // k3, the seed in two halves (a float holds 24 bits)
	for (i32 i = 0; i < 16; ++i) {
		u[i] = v[i];
	}
}

#ifdef ARMORPAINT_TEST_NATIVES
// Test build only: with ARMORPAINT_TILEGEN_DUMP=<directory> in the environment every bake writes what the GPU pass read and wrote, for the host comparison with
// the cpu reference (automation/host/tile_generator_gpu): bake_<n>.params (size, then tile_generator_params_t), bake_<n>_pat<k>.f32 (the pattern captures, RGBA
// float), bake_<n>_maps.f32 (the two map captures, 64 x 64 RGBA float each), bake_<n>_r0.u16 and bake_<n>_r1.u16 (the two results, RGBA half)
static void tile_generator_node_test_write(char *path, gpu_texture_t *t) {
	buffer_t *b = gpu_get_texture_pixels(t);
	if (b == NULL) {
		return;
	}
	FILE *f = fopen(path, "wb");
	if (f != NULL) {
		fwrite(b->buffer, 1, (size_t)b->length, f);
		fclose(f);
	}
}

static void tile_generator_node_test_dump(i32 size, tile_generator_params_t *p, gpu_texture_t **captures, i32 count, gpu_texture_t **targets) {
	static i32 dumps = 0;
	char      *dir   = getenv("ARMORPAINT_TILEGEN_DUMP");
	if (dir == NULL) {
		return;
	}
	char *base = string_tmp("%s/bake_%d", dir, dumps++);
	FILE *f    = fopen(string_tmp("%s.params", base), "wb");
	if (f != NULL) {
		fwrite(&size, sizeof(size), 1, f);
		fwrite(p, sizeof(*p), 1, f);
		fclose(f);
	}
	for (i32 k = 0; k < p->patterns; ++k) {
		tile_generator_node_test_write(string_tmp("%s_pat%d.f32", base, k), captures[k]);
	}
	// The two maps one after the other
	f = fopen(string_tmp("%s_maps.f32", base), "wb");
	for (i32 k = p->patterns; k < count && f != NULL; ++k) {
		buffer_t *b = gpu_get_texture_pixels(captures[k]);
		if (b != NULL) {
			fwrite(b->buffer, 1, (size_t)b->length, f);
		}
	}
	if (f != NULL) {
		fclose(f);
	}
	tile_generator_node_test_write(string_tmp("%s_r0.u16", base), targets[0]);
	tile_generator_node_test_write(string_tmp("%s_r1.u16", base), targets[1]);
}
#endif

// The pass over the captures: the patterns, then the two small map captures (the render targets of capture k, never read back). Returns the target of
// the first result (Image rgb, ID in alpha) and hands over the second (Position) with make_image_node_gpu_set_second; NULL when a target or the shader is missing
static gpu_texture_t *tile_generator_node_gpu_bake(ui_node_t *node, i32 size, void *params, gpu_texture_t **captures, i32 count) {
	tile_generator_params_t *p        = params;
	char                    *names[TILE_GENERATOR_MAX_PATTERNS + 2] = {"tile_p0", "tile_p1", "tile_p2", "tile_p3", "tile_ma", "tile_mb"};
	char                    *inputs[TILE_GENERATOR_MAX_PATTERNS + 2];
	for (i32 k = 0; k < p->patterns; ++k) {
		inputs[k] = names[k];
	}
	inputs[p->patterns]     = names[TILE_GENERATOR_MAX_PATTERNS];
	inputs[p->patterns + 1] = names[TILE_GENERATOR_MAX_PATTERNS + 1];
	char *uniforms[]        = {"float4 k0", "float4 k1", "float4 k2", "float4 k3"};
	char *formats[]         = {"RGBA64", "RGBA64"};
	char *body              = tile_generator_node_gpu_body(p->patterns, p->wrap != 0, p->filter == IMAGE_FILTER_NEAREST, p->blend == TILE_GENERATOR_BLEND_MAX);
	shader_context_t *shader = make_image_node_gpu_shader_with_functions(tile_generator_node_gpu_functions, body, inputs, p->patterns + 2, uniforms, 4, formats, 2);
	free(body);
	if (shader == NULL) {
		return NULL;
	}
	gpu_texture_t *first  = gpu_create_render_target(size, size, GPU_TEXTURE_FORMAT_RGBA64);
	gpu_texture_t *second = gpu_create_render_target(size, size, GPU_TEXTURE_FORMAT_RGBA64);
	if (first == NULL || second == NULL) {
		if (first != NULL) {
			gpu_delete_texture(first);
		}
		if (second != NULL) {
			gpu_delete_texture(second);
		}
		make_image_node_gpu_shader_free(shader);
		return NULL;
	}
	f32 u[16];
	tile_generator_node_gpu_uniforms(p, size, u);
	gpu_texture_t *targets[2] = {first, second};
	make_image_node_gpu_pass(shader, targets, 2, captures, count, u);
	make_image_node_gpu_shader_free(shader);
#ifdef ARMORPAINT_TEST_NATIVES
	tile_generator_node_test_dump(size, p, captures, count, targets);
#endif
	make_image_node_gpu_set_second(second);
	return first;
}

static image_node_type_t tile_generator_node_image_type = {
    .type          = "TEX_TILE_GENERATOR",
    .result_format = GPU_TEXTURE_FORMAT_RGBA64,
    .params_size   = sizeof(tile_generator_params_t),
    .size          = tile_generator_node_size,
    .capture       = tile_generator_node_capture,
    .capture_format = IMAGE_CAPTURE_RGBA32F,
    .second_result = true,
    .result_by_reference = true,
    .gpu_bake      = tile_generator_node_gpu_bake,
    .capture_count = tile_generator_node_capture_count,
    .capture_size  = tile_generator_node_capture_size,
    .params        = tile_generator_node_params,
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

void tile_generator_node_init() {

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
	                        make_image_node_socket(_tr("Pattern 1"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Pattern 2"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Pattern 3"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Pattern 4"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Mask"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Scale Map"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Rotation Map"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Color Map"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Pattern Map"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Tiles X"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(4.0), 0.0, TILE_GENERATOR_MAX_TILES, 1),
	                        make_image_node_socket(_tr("Tiles Y"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(4.0), 0.0, TILE_GENERATOR_MAX_TILES, 1),
	                        make_image_node_socket(_tr("Scale"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.9), 0.0, 2.0, 100),
	                        make_image_node_socket(_tr("Rotation"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.0), 0.0, 360.0, 10), // Degrees
	                        make_image_node_socket(_tr("Scale Random"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Rotation Random"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Offset Random"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Brightness Random"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Seed"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.0), 0.0, 1000.0, 1),
	                    },
	                    18),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Mask"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("ID"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Position"), "VECTOR", NODES_MATERIAL_VECTOR_COLOR, f32_array_create_xyz(0.0, 0.0, 0.0), 0.0, 1.0, 100),
	                    },
	                    4),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Size"), "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS),
	                        make_image_node_state_button(),
	                        make_image_node_setting(_tr("Patterns"), "ENUM", 0, "1 Pattern\n2 Patterns\n3 Patterns\n4 Patterns"),
	                        make_image_node_setting(_tr("Pattern Mode"), "ENUM", 0, "Random\nCycle\nBy Map"),
	                        make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                        make_image_node_setting(_tr("Filter"), "ENUM", TILE_GENERATOR_FILTER_LINEAR, "Linear\nNearest"),
	                        make_image_node_setting(_tr("Blend"), "ENUM", TILE_GENERATOR_BLEND_MAX, "Max\nReplace"),
	                    },
	                    7),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, tile_generator_node_def);
	any_map_set(parser_material_node_values, "TEX_TILE_GENERATOR", tile_generator_node_value);
	any_map_set(parser_material_node_vectors, "TEX_TILE_GENERATOR", tile_generator_node_vector);
	any_map_set(ui_nodes_hidden_rules, "TEX_TILE_GENERATOR", tile_generator_node_item_hidden);
	ui_node_section_t sections[] = {
	    {.title = _tr("Maps"), .kind = UI_NODE_ITEM_INPUT, .first = TILE_GENERATOR_SECTION_MAPS_FIRST, .count = TILE_GENERATOR_SECTION_MAPS_COUNT, .closed_by_default = true},
	    {.title = _tr("Layout"), .kind = UI_NODE_ITEM_INPUT, .first = TILE_GENERATOR_SECTION_LAYOUT_FIRST, .count = TILE_GENERATOR_SECTION_LAYOUT_COUNT, .closed_by_default = false},
	    {.title = _tr("Randomness"), .kind = UI_NODE_ITEM_INPUT, .first = TILE_GENERATOR_SECTION_RANDOMNESS_FIRST, .count = TILE_GENERATOR_SECTION_RANDOMNESS_COUNT, .closed_by_default = true},
	};
	make_image_node_sections(tile_generator_node_def, sections, 3, TILE_GENERATOR_SECTION_MAPS_FIRST); // General: the settings and Pattern 1 to 4
	make_image_node_register(&tile_generator_node_image_type);
}
