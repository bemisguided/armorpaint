#include "../global.h"

// Height to Normal (Filter menu): a height image to a tangent space normal map, optionally on top of an incoming normal. A pass-through
// node with capture (render/make_image_node.c, image_node_type_t.pass_through_capture): it bakes nothing. The Height input is evaluated
// in image space and kept as a render target (32 bit float, one channel: only the height is captured) of Capture Size; the material
// shader reads the 3 x 3 neighbourhood of the pixel from it with exact texel loads, computes the normal and combines it with the Normal
// input, which is evaluated live at the pixel (it is only combined per pixel, no neighbourhood needed). The slope, Strength, Invert and
// the combination are in the shader, so a slider never captures again. A Height linked straight to the Image output of a baked node
// reads that node's result instead (no copy).
// The Bump node stays as it is (it reads the height in the material shader with ddx / ddy of a 2 x 2 pixel quad, so the slope is constant
// inside the quad and a thin diagonal ridge gets blocky normals). Here the slope of every texel is a Sobel gradient of the captured height
// plane, a per pixel value without block structure. Normal to Height (the baked FFT node) is the inverse.
//
// Maths, per texel with the 3 x 3 neighbourhood h (rows y down, columns x right, wrap or clamp at the border):
//   gx = (h[x+1][y-1] + 2 h[x+1][y] + h[x+1][y+1]) - (h[x-1][y-1] + 2 h[x-1][y] + h[x-1][y+1])   (Sobel)
//   gy = (h[x-1][y+1] + 2 h[x][y+1] + h[x+1][y+1]) - (h[x-1][y-1] + 2 h[x][y-1] + h[x+1][y-1])
//   sx = Strength * gx / 8 * Capture Size / 1024, sy likewise    height change per texel at Capture Size 1024 (per image width / 1024 at any
//                                                                size), a ramp of 1 per texel at Capture Size 1024 is 1
//   d  = normalize(-sx, sy, 1)                        the detail normal of the height; green up on the image (up is -y rows)
// The slope does not depend on Capture Size: the 1/8 is the Sobel weight sum (4) times the two texel central difference, Capture Size / 1024
// turns the per texel difference into one per image width (a larger Capture Size has smaller differences per texel for the same picture),
// so Strength 1 gives the same look at 512 and at 4096 (at 1024 a texel is the unit; a ridge of Edge Width w is w * Capture Size texels wide).
// OpenGL convention only (+Y up, green channel up): the one the engine's Normal Map handling expects (a DirectX map is converted to it on
// import, image_texture_node.c). At Capture Size 1024 a ramp of 1 per texel at Strength 1 tilts 45 degrees, a height of 1 over the whole
// image at Strength 1 only 0.056 degrees. Strength scales the slope of the height linearly and nothing else.
// Normal input (like the Bump node's Normal input; unlinked it is the flat normal (0.5, 0.5, 1), meaning no incoming normal): an encoded
// tangent space normal, decoded (2 v - 1) and renormalised. The height detail is combined with it by Reoriented Normal Mapping
// (base = incoming normal b, detail d):
//   t = b + (0, 0, 1), u = d * (-1, -1, 1), n = normalize(t * dot(t, u) / t.z - u)
// which turns the detail by the rotation that takes the flat normal to b. Without an incoming normal the result is d itself (exactly: the
// flat base is taken as a shortcut), a flat height gives b back. The Bump node adds the raw vector to its (ddx, ddy, 1), which is only
// meaningful for a decoded unit normal that does not tilt much; RNM keeps the detail's slope on a tilted base, so it is used here instead.
// The result is stored encoded in the material shader (n * 0.5 + 0.5) in 32 bit float: there is no half float storage any more (the old
// baked node stored half floats, 0.06 degrees), the sampling is whatever the reader does: a filtered normal is not of unit length, the
// Normal Map node renormalises it, and a node that does not has to.
// Invert flips the sign of the height detail (Bump's Invert): the height direction is reversed, the incoming normal is not.
// Wrap Edges (default on): the Sobel neighbourhood wraps at the border of the image (periodic, tileable height); off it clamps to the edge texel.
// The inputs and their order mirror the Bump node (Strength, Height, Normal; Bump's Distance input has no effect in its shader and is not copied).
// Strength is a per pixel input like the other pass-through nodes (a linked value may vary over the image; the baked node read it once at the
// image centre). The neighbourhood is read in units of Capture Size texels: a Height linked straight to a baked node of another size reads
// that node's result at the spacing of Capture Size (nearest texel), so the slope unit stays Capture Size.

// Input socket indices
// Same order as the Bump node (Strength, Height, Normal; Bump's Distance does nothing in its shader and is left out)
enum {
	HEIGHT_TO_NORMAL_INPUT_STRENGTH = 0,
	HEIGHT_TO_NORMAL_INPUT_HEIGHT   = 1,
	HEIGHT_TO_NORMAL_INPUT_NORMAL   = 2,
};

// Node button indices (settings)
enum {
	HEIGHT_TO_NORMAL_BUTTON_CAPTURE_SIZE = 0,
	HEIGHT_TO_NORMAL_BUTTON_INVERT       = 1,
	HEIGHT_TO_NORMAL_BUTTON_WRAP         = 2, // Wrap Edges
};

// Width and height of the capture of the Height input: Capture Size 512, 1024, 2048 or 4096
static i32 height_to_normal_node_size(ui_node_t *node) {
	return make_image_node_size(node, HEIGHT_TO_NORMAL_BUTTON_CAPTURE_SIZE);
}

// Captured per pixel: the Height input, grey in R (the capture target of the shared pass; only the height is captured, the Normal is evaluated live)
static char *height_to_normal_node_capture(ui_node_t *node) {
	char *height = parser_material_parse_value_input(node->inputs->buffer[HEIGHT_TO_NORMAL_INPUT_HEIGHT], false);
	char *name   = parser_material_store_var_name(node);
	parser_material_write(parser_material_kong, string_tmp("float %s_capture = clamp(%s, 0.0, 1.0);", name, height));
	return string_tmp("float4(%s_capture, %s_capture, %s_capture, 1.0)", name, name, name);
}

static image_node_type_t height_to_normal_node_image_type = {
    .type                 = "HEIGHT_TO_NORMAL",
    .size                 = height_to_normal_node_size,
    .capture              = height_to_normal_node_capture,
    .capture_format       = IMAGE_CAPTURE_R32F, // A float plane: the slope is a difference of neighbours times the Capture Size, half floats would band
    .pass_through_capture = true,
    .image_input          = HEIGHT_TO_NORMAL_INPUT_HEIGHT,
};

// The normal (a float3 variable of the material shader, encoded 0..1), written once per node: the Sobel gradient of the 3 x 3 texels around the
// pixel, the detail normal d, then Reoriented Normal Mapping on the incoming normal. Written with scalars and plain if blocks (the shader
// language has no component assignment); the order of the float operations is the one of the old cpu code of the baked node.
static char *height_to_normal_node_normal(ui_node_t *node) {
	char *name = parser_material_store_var_name(node);
	char *out  = string_tmp("%s_normal", name);
	if (string_array_index_of(parser_material_parsed, out) >= 0) {
		return out;
	}
	any_array_push(parser_material_parsed, out);
	if (!parser_material_image_space) {
		node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
	}
	bool  wrap     = make_image_node_button(node, HEIGHT_TO_NORMAL_BUTTON_WRAP) != 0.0f;
	bool  invert   = make_image_node_button(node, HEIGHT_TO_NORMAL_BUTTON_INVERT) != 0.0f;
	char *strength = parser_material_parse_value_input(node->inputs->buffer[HEIGHT_TO_NORMAL_INPUT_STRENGTH], false);
	char *normal   = parser_material_parse_vector_input(node->inputs->buffer[HEIGHT_TO_NORMAL_INPUT_NORMAL]);
	char *n        = name;
	parser_material_write(parser_material_kong, string_tmp("float %s_size = %d.0;", n, height_to_normal_node_size(node))); // Texels of the capture per image width
	// The texel the pixel is in (its centre): the taps are exact loads at the centres of the 3 x 3 texels around it
	parser_material_write(parser_material_kong, string_tmp("float2 %s_c = (floor(tex_coord * %s_size) + float2(0.5, 0.5)) / %s_size;", n, n, n));
	static const char *axis[3] = {"m", "z", "p"}; // -1, 0, +1
	for (i32 j = 0; j < 3; ++j) {
		for (i32 i = 0; i < 3; ++i) {
			if (i == 1 && j == 1) {
				continue; // The centre texel has no weight in the Sobel operator
			}
			char *var = string_tmp("%s_h%s%s", n, axis[i], axis[j]);
			char *uv  = string_tmp("%s_c + float2(%d.0, %d.0) / %s_size", n, i - 1, j - 1, n);
			make_image_node_fetch_input(node, 0, var, uv, IMAGE_FILTER_NEAREST, wrap);
		}
	}
#define H2N_TAP(x_, y_) string_tmp("%s_h%s%s.r", n, x_, y_)
	parser_material_write(parser_material_kong, string_tmp("float %s_gx = (%s + 2.0 * %s + %s) - (%s + 2.0 * %s + %s);", n, H2N_TAP("p", "m"), H2N_TAP("p", "z"),
	                                                       H2N_TAP("p", "p"), H2N_TAP("m", "m"), H2N_TAP("m", "z"), H2N_TAP("m", "p")));
	parser_material_write(parser_material_kong, string_tmp("float %s_gy = (%s + 2.0 * %s + %s) - (%s + 2.0 * %s + %s);", n, H2N_TAP("m", "p"), H2N_TAP("z", "p"),
	                                                       H2N_TAP("p", "p"), H2N_TAP("m", "m"), H2N_TAP("z", "m"), H2N_TAP("p", "m")));
#undef H2N_TAP
	// Strength 0 to 1000 (negative is 0), the sign flipped by Invert, per image width: Strength / 8 * Capture Size / 1024
	parser_material_write(parser_material_kong, string_tmp("float %s_str = clamp(%s, 0.0, 1000.0);", n, strength));
	parser_material_write(parser_material_kong, string_tmp("float %s_k = %s%s_str / 8.0 * %s_size / 1024.0;", n, invert ? "-" : "", n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_sx = -%s_gx * %s_k;", n, n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_sy = %s_gy * %s_k;", n, n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_inv = 1.0 / sqrt(%s_sx * %s_sx + %s_sy * %s_sy + 1.0);", n, n, n, n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_dx = %s_sx * %s_inv;", n, n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_dy = %s_sy * %s_inv;", n, n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_dz = %s_inv;", n, n));
	// The incoming normal, decoded
	parser_material_write(parser_material_kong, string_tmp("float3 %s_in = %s;", n, normal));
	parser_material_write(parser_material_kong, string_tmp("float %s_bx = %s_in.x * 2.0 - 1.0;", n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_by = %s_in.y * 2.0 - 1.0;", n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_bz = %s_in.z * 2.0 - 1.0;", n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_rx = %s_dx;", n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_ry = %s_dy;", n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_rz = %s_dz;", n, n));
	parser_material_write(parser_material_kong, string_tmp("if (%s_bx != 0.0 || %s_by != 0.0 || %s_bz != 1.0) {", n, n, n)); // Not the flat normal: reorient the detail onto the incoming one
	parser_material_write(parser_material_kong, string_tmp("float %s_bl = sqrt(%s_bx * %s_bx + %s_by * %s_by + %s_bz * %s_bz);", n, n, n, n, n, n, n));
	parser_material_write(parser_material_kong, string_tmp("if (%s_bl > 0.000001) {", n));
	parser_material_write(parser_material_kong, string_tmp("%s_bx = %s_bx / %s_bl;", n, n, n));
	parser_material_write(parser_material_kong, string_tmp("%s_by = %s_by / %s_bl;", n, n, n));
	parser_material_write(parser_material_kong, string_tmp("%s_bz = %s_bz / %s_bl;", n, n, n));
	parser_material_write(parser_material_kong, "}");
	parser_material_write(parser_material_kong, string_tmp("if (%s_bl <= 0.000001) {", n));
	parser_material_write(parser_material_kong, string_tmp("%s_bx = 0.0;", n));
	parser_material_write(parser_material_kong, string_tmp("%s_by = 0.0;", n));
	parser_material_write(parser_material_kong, string_tmp("%s_bz = 1.0;", n));
	parser_material_write(parser_material_kong, "}");
	parser_material_write(parser_material_kong, string_tmp("if (%s_bz < 0.001) {", n)); // Pointing away from the surface: keep t.z away from 0
	parser_material_write(parser_material_kong, string_tmp("%s_bz = 0.001;", n));
	parser_material_write(parser_material_kong, string_tmp("float %s_bn = sqrt(%s_bx * %s_bx + %s_by * %s_by + %s_bz * %s_bz);", n, n, n, n, n, n, n));
	parser_material_write(parser_material_kong, string_tmp("%s_bx = %s_bx / %s_bn;", n, n, n));
	parser_material_write(parser_material_kong, string_tmp("%s_by = %s_by / %s_bn;", n, n, n));
	parser_material_write(parser_material_kong, string_tmp("%s_bz = %s_bz / %s_bn;", n, n, n));
	parser_material_write(parser_material_kong, "}");
	// Reoriented Normal Mapping: t = b + (0, 0, 1), u = d * (-1, -1, 1), n = normalize(t * dot(t, u) / t.z - u)
	parser_material_write(parser_material_kong, string_tmp("float %s_tz = %s_bz + 1.0;", n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_ux = -%s_dx;", n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_uy = -%s_dy;", n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_uz = %s_dz;", n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_kk = (%s_bx * %s_ux + %s_by * %s_uy + %s_tz * %s_uz) / %s_tz;", n, n, n, n, n, n, n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_qx = %s_bx * %s_kk - %s_ux;", n, n, n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_qy = %s_by * %s_kk - %s_uy;", n, n, n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_qz = %s_tz * %s_kk - %s_uz;", n, n, n, n));
	parser_material_write(parser_material_kong, string_tmp("float %s_ql = sqrt(%s_qx * %s_qx + %s_qy * %s_qy + %s_qz * %s_qz);", n, n, n, n, n, n, n));
	parser_material_write(parser_material_kong, string_tmp("if (%s_ql > 0.000000000001) {", n));
	parser_material_write(parser_material_kong, string_tmp("%s_rx = %s_qx * (1.0 / %s_ql);", n, n, n));
	parser_material_write(parser_material_kong, string_tmp("%s_ry = %s_qy * (1.0 / %s_ql);", n, n, n));
	parser_material_write(parser_material_kong, string_tmp("%s_rz = %s_qz * (1.0 / %s_ql);", n, n, n));
	parser_material_write(parser_material_kong, "}");
	parser_material_write(parser_material_kong, string_tmp("if (%s_ql <= 0.000000000001) {", n));
	parser_material_write(parser_material_kong, string_tmp("%s_rx = 0.0;", n));
	parser_material_write(parser_material_kong, string_tmp("%s_ry = 0.0;", n));
	parser_material_write(parser_material_kong, string_tmp("%s_rz = 1.0;", n));
	parser_material_write(parser_material_kong, "}");
	parser_material_write(parser_material_kong, "}");
	parser_material_write(parser_material_kong, string_tmp("float3 %s = float3(%s_rx * 0.5 + 0.5, %s_ry * 0.5 + 0.5, %s_rz * 0.5 + 0.5);", out, n, n, n));
	return out;
}

char *height_to_normal_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return height_to_normal_node_normal(node);
}

char *height_to_normal_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.x", height_to_normal_node_normal(node));
}

void height_to_normal_node_init() {
	u32 value_color = NODES_MATERIAL_VALUE_COLOR;

	ui_node_t *height_to_normal_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Height to Normal"),
	                .type   = "HEIGHT_TO_NORMAL",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_PASS_THROUGH_COLOR, // Bakes nothing: a shader on the mesh over a capture of the Height
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Strength"), "VALUE", value_color, f32_array_create_x(4.0), 0.0, 100.0, 100),
	                        make_image_node_socket(_tr("Height"), "VALUE", value_color, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Normal"), "VECTOR", NODES_MATERIAL_VECTOR_COLOR, f32_array_create_xyz(0.5, 0.5, 1.0), 0.0, 1.0, 100),
	                    },
	                    3),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Normal"), "VECTOR", NODES_MATERIAL_VECTOR_COLOR, f32_array_create_xyz(0.5, 0.5, 1.0), 0.0, 1.0, 100), // A direction: purple like the Normal input
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_capture_size_setting(),
	                        make_image_node_setting(_tr("Invert"), "BOOL", 0, NULL),
	                        make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                    },
	                    3),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, height_to_normal_node_def);
	any_map_set(parser_material_node_values, "HEIGHT_TO_NORMAL", height_to_normal_node_value);
	any_map_set(parser_material_node_vectors, "HEIGHT_TO_NORMAL", height_to_normal_node_vector);
	make_image_node_sections_capture(height_to_normal_node_def, NULL, 0, 3); // Texture: Capture Size; General: Invert, Wrap Edges, Strength, Height and Normal
	make_image_node_register(&height_to_normal_node_image_type);
}
