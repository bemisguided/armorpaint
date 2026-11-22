#include "../global.h"

// Tile UV (Pattern menu): places an image on the uv, repeating. Image nodes work in image space and have no Vector
// input; this node goes at the end of the chain and decides where the final image lands on the mesh: Vector (unlinked:
// the uv) is rotated and scaled about the uv centre, then offset, and the Image is read there, wrapped, so Scale 4 repeats it
// 4 x 4 times. Rotation follows the rotation convention of all image nodes (armorpaint-admin
// specs/image-nodes/ROTATION_CONVENTION.md): degrees, a positive angle turns the placed image clockwise as seen on the
// uv (y down), about the uv centre; the Offset is not turned with it (for a uniform Scale it moves the image along the
// uv axes whatever the Rotation, like Transform's Offset).
// A pass-through node with capture (render/make_image_node.c, image_node_type_t.pass_through_capture): it bakes nothing. Its Image
// input is evaluated in image space and kept as a render target (32 bit float, so colour and smooth gradients keep their precision)
// of Capture Size, and the material shader samples that texture at the placed uv; the placement, the Filter and every number are in
// the shader, so a slider never captures again. An Image linked straight to the Image output of a baked node reads that node's
// result instead (no copy).
// Chosen over evaluating the Image graph at the placed uv in the material shader: whatever sits between the image
// nodes and Tile UV (Math, Mix, ...) then stays a plain image space graph, nothing is evaluated twice at a
// different coordinate, and chained image nodes keep reading each other 1:1.
// Filter: Linear reads bilinear, Cubic (default) a cubic B-spline whose slope is continuous (a Bump node on the result
// shows no texel dashes), Nearest the nearest texel (ids, islands, anything that must not blend).

// Input socket indices
enum {
	TILE_UV_INPUT_IMAGE    = 0,
	TILE_UV_INPUT_SCALE_X  = 1,
	TILE_UV_INPUT_SCALE_Y  = 2,
	TILE_UV_INPUT_ROTATION = 3,
	TILE_UV_INPUT_OFFSET_X = 4,
	TILE_UV_INPUT_OFFSET_Y = 5,
	TILE_UV_INPUT_VECTOR   = 6,
};

// Node button indices (settings)
enum {
	TILE_UV_BUTTON_CAPTURE_SIZE = 0,
	TILE_UV_BUTTON_FILTER       = 1,
};

// Width and height of the capture of the Image input: Capture Size 512, 1024, 2048 or 4096
static i32 tile_uv_node_size(ui_node_t *node) {
	return make_image_node_size(node, TILE_UV_BUTTON_CAPTURE_SIZE);
}

// Captured per pixel: the Image input (the capture target of the shared pass)
static char *tile_uv_node_capture(ui_node_t *node) {
	char *image = parser_material_parse_vector_input(node->inputs->buffer[TILE_UV_INPUT_IMAGE]);
	return string_tmp("float4(%s, 1.0)", image);
}

static image_node_type_t tile_uv_node_image_type = {
    .type                 = "TEX_TILE_UV",
    .size                 = tile_uv_node_size,
    .capture              = tile_uv_node_capture,
    .capture_format       = IMAGE_CAPTURE_RGBA32F,
    .pass_through_capture = true,
    .image_input          = TILE_UV_INPUT_IMAGE,
};

// The image placed on the uv: p = R^-1 * (Vector (unlinked: tex_coord) - 0.5), times (Scale X, Scale Y), plus 0.5 plus
// R^-1 * Offset, R = [cos a, -sin a; sin a, cos a] the clockwise turn on the y down uv, so the image is read where it
// came from. Rotation and scale pivot on the uv centre; Scale is a repeat count (Scale 2 repeats the image twice); the
// Offset is turned by R^-1 as well, which keeps its effect on screen along the uv axes whatever the Rotation (exact for a
// uniform Scale)
static char *tile_uv_node_sample(ui_node_t *node) {
	char             *name   = parser_material_store_var_name(node);
	char             *uv     = NULL;
	ui_node_socket_t *vector = node->inputs->buffer[TILE_UV_INPUT_VECTOR];
	if (parser_material_get_input_link(vector) != NULL) {
		uv = string_tmp("%s.xy", parser_material_parse_vector_input(vector));
	}
	else {
		if (!parser_material_image_space) {
			node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
		}
		uv = "tex_coord";
	}
	char *sx  = parser_material_parse_value_input(node->inputs->buffer[TILE_UV_INPUT_SCALE_X], false);
	char *sy  = parser_material_parse_value_input(node->inputs->buffer[TILE_UV_INPUT_SCALE_Y], false);
	char *rot = parser_material_parse_value_input(node->inputs->buffer[TILE_UV_INPUT_ROTATION], false);
	char *ox  = parser_material_parse_value_input(node->inputs->buffer[TILE_UV_INPUT_OFFSET_X], false);
	char *oy  = parser_material_parse_value_input(node->inputs->buffer[TILE_UV_INPUT_OFFSET_Y], false);
	char *p   = string_tmp("%s_place", name);
	char *a   = string_tmp("%s_angle", name);
	if (string_array_index_of(parser_material_parsed, p) == -1) {
		any_array_push(parser_material_parsed, p);
		char *o = string_tmp("%s_shift", name);
		parser_material_write(parser_material_kong, string_tmp("float %s = %s * (3.14159265 / 180.0);", a, rot));
		parser_material_write(parser_material_kong, string_tmp("float2 %s = %s - float2(0.5, 0.5);", p, uv));
		parser_material_write(parser_material_kong,
		                      string_tmp("%s = float2(%s.x * cos(%s) + %s.y * sin(%s), -%s.x * sin(%s) + %s.y * cos(%s));", p, p, a, p, a, p, a, p, a));
		parser_material_write(parser_material_kong, string_tmp("float2 %s = float2(%s, %s);", o, ox, oy));
		parser_material_write(parser_material_kong,
		                      string_tmp("%s = float2(%s.x * cos(%s) + %s.y * sin(%s), -%s.x * sin(%s) + %s.y * cos(%s));", o, o, a, o, a, o, a, o, a));
		parser_material_write(parser_material_kong, string_tmp("%s = %s * float2(%s, %s) + float2(0.5, 0.5) + %s;", p, p, sx, sy, o));
	}
	i32 setting = (i32)make_image_node_button(node, TILE_UV_BUTTON_FILTER); // 0 Linear, 1 Cubic, 2 Nearest
	i32 filter  = setting == 0 ? IMAGE_FILTER_LINEAR : setting == 1 ? IMAGE_FILTER_CUBIC : IMAGE_FILTER_NEAREST;
	return make_image_node_sample_input_at(node, p, filter);
}

char *tile_uv_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.rgb", tile_uv_node_sample(node));
}

char *tile_uv_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", tile_uv_node_sample(node));
}

void tile_uv_node_init() {
	u32 value_color = NODES_MATERIAL_VALUE_COLOR;

	ui_node_t *tile_uv_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Tile UV"),
	                .type   = "TEX_TILE_UV",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_PASS_THROUGH_COLOR, // Bakes nothing: a shader on the mesh over a capture of the Image
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 0.0, 1.0, 100), // Expects a tileable image (the one orange socket of a grey node)
	                        make_image_node_socket(_tr("Scale X"), "VALUE", value_color, f32_array_create_x(1.0), 0.0, 32.0, 100),
	                        make_image_node_socket(_tr("Scale Y"), "VALUE", value_color, f32_array_create_x(1.0), 0.0, 32.0, 100),
	                        make_image_node_socket(_tr("Rotation"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 360.0, 10),
	                        make_image_node_socket(_tr("Offset X"), "VALUE", value_color, f32_array_create_x(0.0), -1.0, 1.0, 100),
	                        make_image_node_socket(_tr("Offset Y"), "VALUE", value_color, f32_array_create_x(0.0), -1.0, 1.0, 100),
	                        make_image_node_vector_socket(),
	                    },
	                    7),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_RGBA_COLOR, f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_capture_size_setting(),
	                        make_image_node_setting(_tr("Filter"), "ENUM", 1, "Linear\nCubic\nNearest"),
	                    },
	                    2),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, tile_uv_node_def);
	any_map_set(parser_material_node_values, "TEX_TILE_UV", tile_uv_node_value);
	any_map_set(parser_material_node_vectors, "TEX_TILE_UV", tile_uv_node_vector);
	make_image_node_sections_capture(tile_uv_node_def, NULL, 0, 1); // Texture: Capture Size; General: Filter and Image; Parameters: the rest
	make_image_node_register(&tile_uv_node_image_type);
}
