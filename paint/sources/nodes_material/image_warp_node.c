#include "../global.h"

// Warp (Filter menu): moves the pixels of the Image input along a direction, optionally smearing them along the way. A pass-through node
// with capture (render/make_image_node.c, image_node_type_t.pass_through_capture): it bakes nothing. The Image input is evaluated in image
// space and kept as a render target (32 bit float, one channel) of Capture Size, and in Slope mode the Map input too (a second target,
// image_node_type_t.image_input2, only while Direction is Slope and Map is linked); the material shader reads the Image at p + d, repeating,
// so the warp, the smear and every number are in the shader and a slider never captures again. In Direction Angle the Map is a shader input
// evaluated at the current pixel (no capture of it). An Image or Map linked straight to the Image output of a baked node reads that node's
// result instead (no copy). With Wrap Edges on (the default) the image repeats, so the warp wraps around the edges; off, samples outside
// the image (the warp tap and the Map reads) clamp to the nearest edge pixel. This is Directional Warp and Slope Blur in one node.
//
// Every output pixel reads the image at p + d, like the Warp node of the Color menu (tex_coord + direction * mask): the content
// moves AGAINST d, so with a positive Strength it moves against the Angle (a negative Strength moves it along the Angle):
//   Direction Angle:        d = (cos Angle, sin Angle) * Strength * Map(p), Map clamped to 0..1.
//   Direction Slope of Map: g = central difference of Map at p, in Map units per image width (Map one capture pixel to each side, times
//                           half the Capture Size); d = g / |g| * min(|g|, 1) * Strength. So a Map that rises by 1 across the image moves the
//                           content by exactly Strength of the image, steeper slopes are clamped to that, flat areas do not move.
// d is in uv, Strength is a fraction of the image (the whole image is 1), Angle is in degrees (0 along x, y down like the uv).
// Angle follows the rotation convention of all image nodes (armorpaint-admin specs/image-nodes/ROTATION_CONVENTION.md):
// the direction is (cos Angle, sin Angle) on the y down image, 90 points down the image (the same direction as Advanced
// Blur's Angle); the lookup moves along it, the content moves against it (Angle 0 moves the content left, 90 up).
// In Slope mode d points uphill, so the content flows downhill (towards lower Map values).
// Blur smears along the path: 0 is a pure warp (one tap at p + d); above 0 the output averages taps on the segment from
// p + d * (1 - Blur) to p + d, so Blur 1 smears over the whole displacement (Slope Blur, Directional Blur along a map).
// Taps are one capture pixel apart or closer, at most IMAGE_WARP_MAX_TAPS (32: the cost is per mesh pixel and frame, decided D8 of the
// bake audit; the baked node took up to 129): a smear longer than 32 pixels aliases (spacing above one pixel).
// Strength, Angle, Blur and Map are evaluated per pixel (a linked number varies over the image).

// Input socket indices
enum {
	IMAGE_WARP_INPUT_IMAGE    = 0,
	IMAGE_WARP_INPUT_MAP      = 1,
	IMAGE_WARP_INPUT_STRENGTH = 2,
	IMAGE_WARP_INPUT_ANGLE    = 3,
	IMAGE_WARP_INPUT_BLUR     = 4,
};

// Node button indices (settings)
enum {
	IMAGE_WARP_BUTTON_CAPTURE_SIZE = 0,
	IMAGE_WARP_BUTTON_DIRECTION    = 1,
	IMAGE_WARP_BUTTON_WRAP         = 2, // Wrap Edges
};

enum {
	IMAGE_WARP_DIRECTION_ANGLE = 0,
	IMAGE_WARP_DIRECTION_SLOPE = 1,
};

#define IMAGE_WARP_MAX_TAPS 32

// Angle only sets the direction of Direction Angle; Slope of Map takes it from the Map. A linked Angle still shows
// (ui_node_item_hidden).
static bool image_warp_node_input_hidden(i32 direction, i32 input) {
	return input == IMAGE_WARP_INPUT_ANGLE && direction == IMAGE_WARP_DIRECTION_SLOPE;
}

static bool image_warp_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	return kind == UI_NODE_ITEM_INPUT && image_warp_node_input_hidden((i32)make_image_node_button(node, IMAGE_WARP_BUTTON_DIRECTION), index);
}

// Width and height of the captures: Capture Size 512, 1024, 2048 or 4096
static i32 image_warp_node_size(ui_node_t *node) {
	return make_image_node_size(node, IMAGE_WARP_BUTTON_CAPTURE_SIZE);
}

// The Map is captured (second capture) only in Slope mode, where it is read at the neighbours of a pixel, and only when it is linked
// (an unlinked Map is flat: no slope)
static i32 image_warp_node_second_input(ui_node_t *node) {
	if ((i32)make_image_node_button(node, IMAGE_WARP_BUTTON_DIRECTION) != IMAGE_WARP_DIRECTION_SLOPE) {
		return -1;
	}
	return parser_material_get_input_link(node->inputs->buffer[IMAGE_WARP_INPUT_MAP]) != NULL ? IMAGE_WARP_INPUT_MAP : -1;
}

// Captured per pixel: capture 0 is the Image, capture 1 the Map (one channel each, in R)
static char *image_warp_node_capture(ui_node_t *node) {
	i32   input = make_image_node_capture_index() == 0 ? IMAGE_WARP_INPUT_IMAGE : IMAGE_WARP_INPUT_MAP;
	char *value = parser_material_parse_value_input(node->inputs->buffer[input], false);
	char *name  = parser_material_store_var_name(node);
	parser_material_write(parser_material_kong, string_tmp("float %s_capture = %s;", name, value));
	return string_tmp("float4(%s_capture, %s_capture, %s_capture, 1.0)", name, name, name);
}

static image_node_type_t image_warp_node_image_type = {
    .type                 = "TEX_IMAGE_WARP",
    .size                 = image_warp_node_size,
    .capture              = image_warp_node_capture,
    .capture_format       = IMAGE_CAPTURE_R32F, // A float plane per capture: the slope of the Map is a difference times the Capture Size, half floats would be noise
    .pass_through_capture = true,
    .image_input          = IMAGE_WARP_INPUT_IMAGE,
    .image_input2         = image_warp_node_second_input,
};

// The warped value (a float variable of the material shader), written once per node: the displacement d in uv, then the Image read at
// p + d, or the average of up to IMAGE_WARP_MAX_TAPS taps along the smear
static char *image_warp_node_warp(ui_node_t *node) {
	char *name = parser_material_store_var_name(node);
	char *out  = string_tmp("%s_warp", name);
	if (string_array_index_of(parser_material_parsed, out) >= 0) {
		return out;
	}
	any_array_push(parser_material_parsed, out);
	if (!parser_material_image_space) {
		node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
	}
	bool  slope    = (i32)make_image_node_button(node, IMAGE_WARP_BUTTON_DIRECTION) == IMAGE_WARP_DIRECTION_SLOPE;
	bool  wrap     = make_image_node_button(node, IMAGE_WARP_BUTTON_WRAP) != 0.0f;
	char *strength = parser_material_parse_value_input(node->inputs->buffer[IMAGE_WARP_INPUT_STRENGTH], false);
	parser_material_write(parser_material_kong, string_tmp("float %s_size = %d.0;", name, image_warp_node_size(node))); // Pixels of a capture per image width
	parser_material_write(parser_material_kong, string_tmp("float2 %s_p = tex_coord;", name));
	parser_material_write(parser_material_kong, string_tmp("float %s_strength = %s;", name, strength));
	if (slope) {
		if (image_warp_node_second_input(node) >= 0) {
			// Central differences of the Map one capture pixel to each side, 0..1 clamped, in Map units per image width
			static const char *sides[4]   = {"r", "l", "u", "d"};
			static const char *offsets[4] = {"float2(1.0 / %s_size, 0.0)", "float2(-1.0 / %s_size, 0.0)", "float2(0.0, 1.0 / %s_size)", "float2(0.0, -1.0 / %s_size)"};
			for (i32 i = 0; i < 4; ++i) {
				char *var = string_tmp("%s_m%s", name, sides[i]);
				char *uv  = string_tmp("%s_p + %s", name, string_tmp((char *)offsets[i], name));
				make_image_node_fetch_input(node, 1, var, uv, IMAGE_FILTER_LINEAR, wrap);
			}
			parser_material_write(parser_material_kong,
			                      string_tmp("float %s_gx = (clamp(%s_mr.r, 0.0, 1.0) - clamp(%s_ml.r, 0.0, 1.0)) * 0.5 * %s_size;", name, name, name, name));
			parser_material_write(parser_material_kong,
			                      string_tmp("float %s_gy = (clamp(%s_mu.r, 0.0, 1.0) - clamp(%s_md.r, 0.0, 1.0)) * 0.5 * %s_size;", name, name, name, name));
			parser_material_write(parser_material_kong, string_tmp("float %s_g = sqrt(%s_gx * %s_gx + %s_gy * %s_gy);", name, name, name, name, name));
			parser_material_write(parser_material_kong, string_tmp("float2 %s_d = float2(%s_gx, %s_gy) * (min(%s_g, 1.0) / max(%s_g, 0.000001) * %s_strength);", name, name,
			                                                       name, name, name, name));
		}
		else {
			parser_material_write(parser_material_kong, string_tmp("float2 %s_d = float2(0.0, 0.0);", name)); // A flat Map does not move anything
		}
	}
	else {
		char *map   = parser_material_parse_value_input(node->inputs->buffer[IMAGE_WARP_INPUT_MAP], false);
		char *angle = parser_material_parse_value_input(node->inputs->buffer[IMAGE_WARP_INPUT_ANGLE], false);
		parser_material_write(parser_material_kong, string_tmp("float %s_angle = (%s) * (3.14159265 / 180.0);", name, angle));
		parser_material_write(parser_material_kong, string_tmp("float2 %s_d = float2(cos(%s_angle), sin(%s_angle)) * (%s_strength * clamp(%s, 0.0, 1.0));", name,
		                                                       name, name, name, map));
	}
	ui_node_socket_t *blur_socket = node->inputs->buffer[IMAGE_WARP_INPUT_BLUR];
	if (parser_material_get_input_link(blur_socket) == NULL && blur_socket->default_value->buffer[0] <= 0.0f) {
		// No smear: one tap at p + d
		make_image_node_fetch_input(node, 0, string_tmp("%s_tap", name), string_tmp("%s_p + %s_d", name, name), IMAGE_FILTER_LINEAR, wrap);
		parser_material_write(parser_material_kong, string_tmp("float %s_warp = clamp(%s_tap.r, 0.0, 1.0);", name, name));
		return out;
	}
	char *blur = parser_material_parse_value_input(blur_socket, false);
	parser_material_write(parser_material_kong, string_tmp("float %s_blur = clamp(%s, 0.0, 1.0);", name, blur));
	parser_material_write(parser_material_kong,
	                      string_tmp("float %s_len = %s_blur * sqrt(%s_d.x * %s_d.x + %s_d.y * %s_d.y) * %s_size;", name, name, name, name, name, name, name));
	parser_material_write(parser_material_kong, string_tmp("float %s_taps = 1.0;", name));
	parser_material_write(parser_material_kong, string_tmp("if (%s_len >= 0.5) {", name));
	parser_material_write(parser_material_kong, string_tmp("%s_taps = min(ceil(%s_len) + 1.0, %d.0);", name, name, IMAGE_WARP_MAX_TAPS));
	parser_material_write(parser_material_kong, "}");
	parser_material_write(parser_material_kong, string_tmp("float %s_sum = 0.0;", name));
	parser_material_write(parser_material_kong, string_tmp("for (int i = 0; i < int(%s_taps); i += 1) {", name));
	// 0 to 1 along the segment from p + d * (1 - blur) to p + d
	parser_material_write(parser_material_kong, string_tmp("float %s_k = 1.0 - %s_blur + %s_blur * (float(i) / max(%s_taps - 1.0, 1.0));", name, name, name, name));
	parser_material_write(parser_material_kong, string_tmp("if (%s_taps < 1.5) {", name));
	parser_material_write(parser_material_kong, string_tmp("%s_k = 1.0;", name));
	parser_material_write(parser_material_kong, "}");
	make_image_node_fetch_input(node, 0, string_tmp("%s_tap", name), string_tmp("%s_p + %s_d * %s_k", name, name, name), IMAGE_FILTER_LINEAR, wrap);
	parser_material_write(parser_material_kong, string_tmp("%s_sum += %s_tap.r;", name, name));
	parser_material_write(parser_material_kong, "}");
	parser_material_write(parser_material_kong, string_tmp("float %s_warp = clamp(%s_sum / %s_taps, 0.0, 1.0);", name, name, name));
	return out;
}

char *image_warp_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return image_warp_node_warp(node);
}

char *image_warp_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return parser_material_to_vec3(image_warp_node_value(node, socket));
}

void image_warp_node_init() {
	u32 value_color = NODES_MATERIAL_VALUE_COLOR;

	ui_node_t *image_warp_node_def =
	    ALLOC_INIT(ui_node_t, {.id     = 0,
	                           .name   = _tr("Warp"),
	                           .type   = "TEX_IMAGE_WARP",
	                           .x      = 0,
	                           .y      = 0,
	                           .color  = NODES_MATERIAL_PASS_THROUGH_COLOR, // Bakes nothing: a shader on the mesh over captures of Image (and Map in Slope mode)
	                           .inputs = any_array_create_from_raw(
	                               (void *[]){
	                                   make_image_node_socket(_tr("Image"), "VALUE", value_color, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                                   make_image_node_socket(_tr("Map"), "VALUE", value_color, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                                   make_image_node_socket(_tr("Strength"), "VALUE", value_color, f32_array_create_x(0.05), -0.5, 0.5, 1000),
	                                   make_image_node_socket(_tr("Angle"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 360.0, 10),
	                                   make_image_node_socket(_tr("Blur"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                               },
	                               5),
	                           .outputs = any_array_create_from_raw(
	                               (void *[]){
	                                   make_image_node_socket(_tr("Image"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                               },
	                               1),
	                           .buttons = any_array_create_from_raw(
	                               (void *[]){
	                                   make_image_node_capture_size_setting(),
	                                   make_image_node_setting(_tr("Direction"), "ENUM", 0, "Angle\nSlope of Map"),
	                                   make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                               },
	                               3),
	                           .width = 0,
	                           .flags = 0});

	any_array_push(nodes_material_filter, image_warp_node_def);
	any_map_set(parser_material_node_values, "TEX_IMAGE_WARP", image_warp_node_value);
	any_map_set(parser_material_node_vectors, "TEX_IMAGE_WARP", image_warp_node_vector);
	any_map_set(ui_nodes_hidden_rules, "TEX_IMAGE_WARP", image_warp_node_item_hidden);
	make_image_node_sections_capture(image_warp_node_def, NULL, 0, 1); // Texture: Capture Size; General: Direction, Wrap Edges and Image; Parameters: the rest
	make_image_node_register(&image_warp_node_image_type);
}
