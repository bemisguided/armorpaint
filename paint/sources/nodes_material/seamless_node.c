#include "../global.h"

// Seamless (Filter menu): makes the Image input repeat without a visible seam. A pass-through node with capture
// (render/make_image_node.c, image_node_type_t.pass_through_capture): it bakes nothing. The Image input is evaluated in image space and
// kept as a render target (32 bit float, colour kept) of Capture Size; the material shader reads that capture four times and blends
// the taps (an Image linked straight to the Image output of a baked node reads that node's result instead, no copy). The node's
// result is a function of the uv written in the shader, so Blend Width is a plain slider that never captures again.
//
// The usual make-it-tile, separable: the image is blended with copies of itself shifted by half the image in x, in y
// and in both. The weight of the unshifted image along an axis is 0 on the borders and ramps (smoothstep) to 1 over
// Blend Width, a fraction of the image measured from each border. The four copies are weighted by the products of the
// x and y weights, so the shifted copies' own wrap seams (at the centre lines) always carry weight 0 and the borders
// are continuous across the repeat: no seam on the borders and none through the middle. The price is a cross fade inside the
// Blend Width: detail there is mixed from two places (ghosting on strong features), a smaller Blend Width keeps more
// of the original and shows the join more.
// The taps are bilinear and wrap (make_image_node_sample_input_tap): on the pixel grid of the capture a shift by half the image is
// exactly Capture Size / 2 texels, a texel centre, so the taps are the capture's own pixels and the result is what the old baked node
// computed on the cpu (the same weights from the same formula), read at any other resolution it is smooth. Blend Width is a
// per pixel value: a slider is one number, a linked value may vary over the image (the baked node read it once at the image
// centre).

// Input socket indices
enum {
	SEAMLESS_INPUT_IMAGE       = 0,
	SEAMLESS_INPUT_BLEND_WIDTH = 1,
};

// Node button indices (settings)
enum {
	SEAMLESS_BUTTON_CAPTURE_SIZE = 0,
};

// Width and height of the capture of the Image input: Capture Size 512, 1024, 2048 or 4096
static i32 seamless_node_size(ui_node_t *node) {
	return make_image_node_size(node, SEAMLESS_BUTTON_CAPTURE_SIZE);
}

// Captured per pixel: the Image input (the capture target of the shared pass)
static char *seamless_node_capture(ui_node_t *node) {
	char *image = parser_material_parse_vector_input(node->inputs->buffer[SEAMLESS_INPUT_IMAGE]);
	return string_tmp("float4(%s, 1.0)", image);
}

static image_node_type_t seamless_node_image_type = {
    .type                 = "TEX_SEAMLESS",
    .size                 = seamless_node_size,
    .capture              = seamless_node_capture,
    .capture_format       = IMAGE_CAPTURE_RGBA32F,
    .pass_through_capture = true,
    .image_input          = SEAMLESS_INPUT_IMAGE,
};

// The blend, once per node and material: with f the uv wrapped to 0..1, d = distance to the nearest border per axis, the weight of
// the unshifted image per axis w = smoothstep(d / Blend Width) (Blend Width clamped to 0 .. 0.5; a width below 0.0001 acts as 0.0001:
// weight 1 except in the outermost 0.0001 of the image), and the four taps of the capture at uv, uv + (0.5, 0), uv + (0, 0.5) and
// uv + (0.5, 0.5) weighted by w.x w.y, (1 - w.x) w.y, w.x (1 - w.y) and (1 - w.x) (1 - w.y). The taps are bilinear (exact on the texel
// centres of the capture) and in the same order as the sum of the old cpu code
static char *seamless_node_blend(ui_node_t *node) {
	char *name = parser_material_store_var_name(node);
	char *v    = string_tmp("%s_seam", name);
	char *res  = string_tmp("%s_blend", v);
	if (string_array_index_of(parser_material_parsed, res) >= 0) {
		return res;
	}
	if (!parser_material_image_space) {
		node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
	}
	char *bw = parser_material_parse_value_input(node->inputs->buffer[SEAMLESS_INPUT_BLEND_WIDTH], false);
	char *uv = "tex_coord";
	parser_material_write(parser_material_kong, string_tmp("float2 %s_f = %s - floor(%s);", v, uv, uv));
	parser_material_write(parser_material_kong, string_tmp("float2 %s_dist = min(%s_f, 1.0 - %s_f);", v, v, v));
	parser_material_write(parser_material_kong, string_tmp("float %s_bw = max(min(%s, 0.5), 0.0001);", v, bw));
	parser_material_write(parser_material_kong, string_tmp("float2 %s_t = clamp(%s_dist / %s_bw, 0.0, 1.0);", v, v, v));
	parser_material_write(parser_material_kong, string_tmp("float2 %s_w = %s_t * %s_t * (3.0 - 2.0 * %s_t);", v, v, v, v));
	char *a = make_image_node_sample_input_tap(node, "seam_a", string_tmp("%s_f", v), IMAGE_FILTER_LINEAR);
	char *b = make_image_node_sample_input_tap(node, "seam_b", string_tmp("%s_f + float2(0.5, 0.0)", v), IMAGE_FILTER_LINEAR);
	char *c = make_image_node_sample_input_tap(node, "seam_c", string_tmp("%s_f + float2(0.0, 0.5)", v), IMAGE_FILTER_LINEAR);
	char *d = make_image_node_sample_input_tap(node, "seam_d", string_tmp("%s_f + float2(0.5, 0.5)", v), IMAGE_FILTER_LINEAR);
	any_array_push(parser_material_parsed, res);
	parser_material_write(parser_material_kong, string_tmp("float %s_w00 = %s_w.x * %s_w.y;", v, v, v));
	parser_material_write(parser_material_kong, string_tmp("float %s_w10 = (1.0 - %s_w.x) * %s_w.y;", v, v, v));
	parser_material_write(parser_material_kong, string_tmp("float %s_w01 = %s_w.x * (1.0 - %s_w.y);", v, v, v));
	parser_material_write(parser_material_kong, string_tmp("float %s_w11 = (1.0 - %s_w.x) * (1.0 - %s_w.y);", v, v, v));
	parser_material_write(parser_material_kong, string_tmp("float4 %s = float4(%s.rgb * %s_w00 + %s.rgb * %s_w10 + %s.rgb * %s_w01 + %s.rgb * %s_w11, 1.0);", res, a, v, b, v, c, v, d, v));
	return res;
}

char *seamless_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.rgb", seamless_node_blend(node));
}

char *seamless_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", seamless_node_blend(node));
}

void seamless_node_init() {
	u32 value_color = NODES_MATERIAL_VALUE_COLOR;

	ui_node_t *seamless_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Seamless"),
	                .type   = "TEX_SEAMLESS",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_PASS_THROUGH_COLOR, // Bakes nothing: a shader on the mesh over a capture of the Image
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_RGBA_COLOR, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Blend Width"), "VALUE", value_color, f32_array_create_x(0.25), 0.0, 0.5, 100),
	                    },
	                    2),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_RGBA_COLOR, f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_capture_size_setting(),
	                    },
	                    1),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, seamless_node_def);
	any_map_set(parser_material_node_values, "TEX_SEAMLESS", seamless_node_value);
	any_map_set(parser_material_node_vectors, "TEX_SEAMLESS", seamless_node_vector);
	make_image_node_sections_capture(seamless_node_def, NULL, 0, -1); // Texture only (Capture Size): Image and Blend Width stay plain rows
	make_image_node_register(&seamless_node_image_type);
}
