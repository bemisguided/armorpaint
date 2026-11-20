#include "../global.h"

// Height to Normal (Filter menu): turns a height image into a tangent space normal map, optionally on top of an
// incoming normal. An image node (render/make_image_node.c): the Height input (the height, grey, 0 black to 1 white) and the
// Normal input are evaluated in image space (32 bit float so a smooth height does not band) and the normal map is computed
// on the cpu into one RGBA texture per node, RGB = normal * 0.5 + 0.5, sampled like an Image Texture at tex_coord,
// repeating (Tile UV places it on the uv).
// It replaces a Bump node for image node heights: Bump reads ddx / ddy of the height, derivatives over a 2 x 2 pixel
// quad, so the slope is constant inside the quad and a thin diagonal ridge gets blocky normals. Here the slope of
// every texel is a Sobel gradient of the baked height plane, a per pixel value without block structure.
//
// Maths, per texel with the 3 x 3 neighbourhood h (rows y down, columns x right, wrap or clamp at the border):
//   gx = (h[x+1][y-1] + 2 h[x+1][y] + h[x+1][y+1]) - (h[x-1][y-1] + 2 h[x-1][y] + h[x-1][y+1])   (Sobel)
//   gy = (h[x-1][y+1] + 2 h[x][y+1] + h[x+1][y+1]) - (h[x-1][y-1] + 2 h[x][y-1] + h[x+1][y-1])
//   sx = Strength * gx / 8 * Size / 1024, sy likewise    height change per texel at Size 1024 (per image width / 1024 at any Size), a ramp of 1 per texel at Size 1024 is 1
//   d  = normalize(-sx, sy, 1)                        the detail normal of the height; green up on the image (up is -y rows)
// The slope does not depend on Size: the 1/8 is the Sobel weight sum (4) times the two texel central difference, Size / 1024 turns
// the per texel difference into one per image width (a larger Size has smaller differences per texel for the same picture), so
// Strength 1 gives the same look at 512 and at 4096 (at 1024 a texel is the unit; a ridge of Edge Width w is w * Size texels wide).
// OpenGL convention only (+Y up, green channel up): the one the engine's Normal Map handling expects (a DirectX map is
// converted to it on import, image_texture_node.c). At Size 1024 a ramp of 1 per texel at Strength 1 tilts 45
// degrees, a height of 1 over the whole image at Strength 1 only 0.056 degrees. Strength scales the slope of the height linearly and nothing else.
// Normal input (like the Bump node's Normal input; unlinked it is the flat normal (0.5, 0.5, 1), meaning no incoming normal):
// an encoded tangent space normal, decoded (2 v - 1) and renormalised per texel. The height detail is combined with it by
// Reoriented Normal Mapping (base = incoming normal b, detail d):
//   t = b + (0, 0, 1), u = d * (-1, -1, 1), n = normalize(t * dot(t, u) / t.z - u)
// which turns the detail by the rotation that takes the flat normal to b. Without an incoming normal the result is d itself
// (exactly: the flat base is taken as a shortcut), a flat height gives b back. The Bump node adds the raw vector to its
// (ddx, ddy, 1), which is only meaningful for a decoded unit normal that does not tilt much; RNM keeps the detail's slope
// on a tilted base, so it is used here instead.
// The result is stored encoded in half floats (precision 2^-11 near 0.5..1, about 0.06 degrees), the sampling filter is bilinear:
// a filtered normal is not of unit length any more, the Normal Map node renormalises it, and a node that does not has to.
// Invert flips the sign of the height detail (Bump's Invert): the height direction is reversed, the incoming normal is not.
// The inputs and their order mirror the Bump node (Strength, Height, Normal) so the node can replace it; this is the one image
// node whose first input is not Image. Bump's Distance input has no effect in its shader and is not copied.
// Strength is scalar for the whole image (evaluated at the image centre when linked): the slope scale has to be known per bake.
// The capture packs both inputs in one RGBA float: R = height, GBA = the incoming normal (encoded 0..1).

// Input socket indices
// Same order as the Bump node (Strength, Height, Normal; Bump's Distance does nothing in its shader and is left out), so it can replace it
enum {
	HEIGHT_TO_NORMAL_INPUT_STRENGTH = 0,
	HEIGHT_TO_NORMAL_INPUT_HEIGHT   = 1,
	HEIGHT_TO_NORMAL_INPUT_NORMAL   = 2,
};

// Node button indices (settings)
enum {
	HEIGHT_TO_NORMAL_BUTTON_SIZE   = 0, // Button 1 is the state row
	HEIGHT_TO_NORMAL_BUTTON_INVERT = 2,
	HEIGHT_TO_NORMAL_BUTTON_WRAP   = 3,
};

typedef struct height_to_normal_params {
	i32 invert;
	i32 wrap;
	f32 strength;
} height_to_normal_params_t;

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 height_to_normal_node_size(ui_node_t *node) {
	return make_image_node_size(node, HEIGHT_TO_NORMAL_BUTTON_SIZE);
}

// Captured per pixel: the Height input as grey in R, the Normal input in GBA
static char *height_to_normal_node_capture(ui_node_t *node) {
	char *image  = parser_material_parse_value_input(node->inputs->buffer[HEIGHT_TO_NORMAL_INPUT_HEIGHT], false);
	char *normal = parser_material_parse_vector_input(node->inputs->buffer[HEIGHT_TO_NORMAL_INPUT_NORMAL]);
	char *grey   = string_tmp("%s_grey", parser_material_store_var_name(node));
	char *nor    = string_tmp("%s_nor", parser_material_store_var_name(node));
	parser_material_write(parser_material_kong, string_tmp("float %s = clamp(%s, 0.0, 1.0);", grey, image));
	parser_material_write(parser_material_kong, string_tmp("float3 %s = %s;", nor, normal));
	return string_tmp("float4(%s, %s.x, %s.y, %s.z)", grey, nor, nor, nor);
}

static void height_to_normal_node_params(ui_node_t *node, void *params) {
	height_to_normal_params_t *p = params;
	p->invert                    = make_image_node_button(node, HEIGHT_TO_NORMAL_BUTTON_INVERT) != 0.0;
	p->wrap                      = make_image_node_button(node, HEIGHT_TO_NORMAL_BUTTON_WRAP) != 0.0;
	p->strength                  = make_image_node_input_value(node, HEIGHT_TO_NORMAL_INPUT_STRENGTH);
}

static i32 height_to_normal_node_wrap_index(i32 i, i32 size, bool wrap) {
	if (wrap) {
		return ((i % size) + size) % size;
	}
	return i < 0 ? 0 : i >= size ? size - 1 : i;
}

// Detail normal d (unit, tilt from the slope) on the base normal b (unit): Reoriented Normal Mapping
static void height_to_normal_node_rnm(const f32 *b, const f32 *d, f32 *n) {
	f32 tx = b[0], ty = b[1], tz = b[2] + 1.0f;
	f32 ux = -d[0], uy = -d[1], uz = d[2];
	f32 k  = (tx * ux + ty * uy + tz * uz) / tz;
	f32 rx = tx * k - ux, ry = ty * k - uy, rz = tz * k - uz;
	f32 l  = sqrtf(rx * rx + ry * ry + rz * rz);
	l      = l > 1e-12f ? 1.0f / l : 0.0f;
	n[0]   = rx * l;
	n[1]   = ry * l;
	n[2]   = l > 0.0f ? rz * l : 1.0f;
}

typedef struct height_to_normal_ctx {
	f32 *pixels; // Captured RGBA: the height in R, the incoming normal encoded in GBA
	f32 *result;
	f32  scale; // Strength / 8 * Size / 1024
	i32  size;
	bool wrap;
} height_to_normal_ctx_t;

// Rows [begin, end) of the normal map
static void height_to_normal_node_rows(void *param, i32 begin, i32 end) {
	height_to_normal_ctx_t *c = param;
	f32 *pixels = c->pixels;
	i32  size   = c->size;
	for (i32 y = begin; y < end; ++y) {
		i32 ym = height_to_normal_node_wrap_index(y - 1, size, c->wrap);
		i32 yp = height_to_normal_node_wrap_index(y + 1, size, c->wrap);
		for (i32 x = 0; x < size; ++x) {
			i32 xm = height_to_normal_node_wrap_index(x - 1, size, c->wrap);
			i32 xp = height_to_normal_node_wrap_index(x + 1, size, c->wrap);
#define H2N_AT(px_, py_) pixels[((size_t)(py_) * size + (px_)) * 4]
			f32 gx = (H2N_AT(xp, ym) + 2.0f * H2N_AT(xp, y) + H2N_AT(xp, yp)) - (H2N_AT(xm, ym) + 2.0f * H2N_AT(xm, y) + H2N_AT(xm, yp));
			f32 gy = (H2N_AT(xm, yp) + 2.0f * H2N_AT(x, yp) + H2N_AT(xp, yp)) - (H2N_AT(xm, ym) + 2.0f * H2N_AT(x, ym) + H2N_AT(xp, ym));
#undef H2N_AT
			f32 nx    = -gx * c->scale;
			f32 ny    = gy * c->scale;
			f32 inv   = 1.0f / sqrtf(nx * nx + ny * ny + 1.0f);
			f32 d[3]  = {nx * inv, ny * inv, inv};
			f32 *in   = pixels + ((size_t)y * size + x) * 4;
			f32 bx    = in[1] * 2.0f - 1.0f, by = in[2] * 2.0f - 1.0f, bz = in[3] * 2.0f - 1.0f;
			f32 *o    = c->result + ((size_t)y * size + x) * 4;
			f32 n[3]  = {d[0], d[1], d[2]};
			if (!(bx == 0.0f && by == 0.0f && bz == 1.0f)) { // Not the flat normal: reorient the detail onto the incoming one
				f32 bl = sqrtf(bx * bx + by * by + bz * bz);
				f32 b[3];
				if (bl > 1e-6f) {
					b[0] = bx / bl;
					b[1] = by / bl;
					b[2] = bz / bl;
				}
				else {
					b[0] = 0.0f;
					b[1] = 0.0f;
					b[2] = 1.0f;
				}
				if (b[2] < 1e-3f) { // Pointing away from the surface: keep t.z away from 0
					b[2]     = 1e-3f;
					f32 l    = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
					b[0]    /= l;
					b[1]    /= l;
					b[2]    /= l;
				}
				height_to_normal_node_rnm(b, d, n);
			}
			o[0] = n[0] * 0.5f + 0.5f;
			o[1] = n[1] * 0.5f + 0.5f;
			o[2] = n[2] * 0.5f + 0.5f;
			o[3] = 1.0f;
		}
	}
}

// Normal map from the captured height and normal; returns 0
static i32 height_to_normal_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	height_to_normal_params_t p = *(height_to_normal_params_t *)params;
	f32 strength = p.strength;
	strength     = !(strength > 0.0f) ? 0.0f : strength > 1000.0f ? 1000.0f : strength; // Also keeps NaN out
	strength     = p.invert ? -strength : strength;
	height_to_normal_ctx_t c = {.pixels = (f32 *)bytes, .result = result, .scale = strength / 8.0f * (f32)size / 1024.0f, .size = size, .wrap = p.wrap != 0};
	make_image_node_parallel(size, height_to_normal_node_rows, &c);
	return 0;
}

static image_node_type_t height_to_normal_node_image_type = {
    .type          = "TEX_HEIGHT_TO_NORMAL",
    .result_format = GPU_TEXTURE_FORMAT_RGBA64, // A normal encoded in 0..1: half floats resolve 0.06 degrees, no second plane
    .params_size   = sizeof(height_to_normal_params_t),
    .size          = height_to_normal_node_size,
    .capture       = height_to_normal_node_capture,
    .capture_float = true,
    .smooth_sample = true,
    .params        = height_to_normal_node_params,
    .process       = height_to_normal_node_process,
};

char *height_to_normal_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.rgb", make_image_node_sample(node));
}

char *height_to_normal_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

void height_to_normal_node_init() {
	u32 value_color = 0xffa1a1a1;

	ui_node_t *height_to_normal_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Height to Normal"),
	                .type   = "TEX_HEIGHT_TO_NORMAL",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Strength"), "VALUE", value_color, f32_array_create_x(4.0), 0.0, 100.0, 100),
	                        make_image_node_socket(_tr("Height"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Normal"), "VECTOR", 0xff6363c7, f32_array_create_xyz(0.5, 0.5, 1.0), 0.0, 1.0, 100),
	                    },
	                    3),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Normal"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(0.5, 0.5, 1.0, 1.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Size"), "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS),
	                        make_image_node_state_button(),
	                        make_image_node_setting(_tr("Invert"), "BOOL", 0, NULL),
	                        make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                    },
	                    4),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, height_to_normal_node_def);
	any_map_set(parser_material_node_values, "TEX_HEIGHT_TO_NORMAL", height_to_normal_node_value);
	any_map_set(parser_material_node_vectors, "TEX_HEIGHT_TO_NORMAL", height_to_normal_node_vector);
	make_image_node_sections(height_to_normal_node_def, NULL, 0, 3); // General: Invert, Wrap Edges, Strength, Height and Normal
	make_image_node_register(&height_to_normal_node_image_type);
}
