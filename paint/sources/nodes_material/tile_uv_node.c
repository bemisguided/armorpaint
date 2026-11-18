#include "../global.h"

// Tile UV (Image menu): places an image on the uv, repeating. Image nodes work in image space and have no Vector
// input; this node goes at the end of the chain and decides where the final image lands on the mesh: Vector (unlinked:
// the uv) is scaled, rotated and offset like a Mapping node and the Image is read there, wrapped, so Scale 4 repeats it
// 4 x 4 times. It is an image node itself (render/make_image_node.c): its Image input is evaluated in image space,
// baked at Size (32 bit float so colour and smooth gradients keep their precision) and sampled at the placed uv.
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
	TILE_UV_BUTTON_SIZE   = 0,
	TILE_UV_BUTTON_FILTER = 2, // The state row is button 1
};

static f32 tile_uv_node_button(ui_node_t *node, i32 i) {
	return node->buttons->buffer[i]->default_value->buffer[0];
}

// Width and height of the baked image: Size 512, 1024, 2048 or 4096
static i32 tile_uv_node_size(ui_node_t *node) {
	i32 i = (i32)tile_uv_node_button(node, TILE_UV_BUTTON_SIZE);
	return 512 << (i < 0 ? 0 : i > 3 ? 3 : i);
}

// Captured per pixel: the Image input
static char *tile_uv_node_capture(ui_node_t *node) {
	char *image = parser_material_parse_vector_input(node->inputs->buffer[TILE_UV_INPUT_IMAGE]);
	return string_tmp("float4(%s, 1.0)", image);
}

static void tile_uv_node_params(ui_node_t *node, void *params) {
}

// The captured image is the result
static i32 tile_uv_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	memcpy(result, bytes, (size_t)size * size * 4 * sizeof(f32));
	return 0;
}

static image_node_type_t tile_uv_node_image_type = {
    .type          = "TEX_TILE_UV",
    .result_format = GPU_TEXTURE_FORMAT_RGBA128,
    .params_size   = 0,
    .size          = tile_uv_node_size,
    .capture       = tile_uv_node_capture,
    .capture_float = true,
    .params        = tile_uv_node_params,
    .process       = tile_uv_node_process,
};

// The image placed on the uv: Vector (unlinked: tex_coord) rotated around its centre, scaled, then offset
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
		parser_material_write(parser_material_kong, string_tmp("float2 %s = %s - 0.5;", p, uv));
		parser_material_write(parser_material_kong, string_tmp("float %s = %s * (3.14159265 / 180.0);", a, rot));
		parser_material_write(parser_material_kong,
		                      string_tmp("%s = float2(%s.x * cos(%s) - %s.y * sin(%s), %s.x * sin(%s) + %s.y * cos(%s));", p, p, a, p, a, p, a, p, a));
		parser_material_write(parser_material_kong, string_tmp("%s = %s * float2(%s, %s) + 0.5 + float2(%s, %s);", p, p, sx, sy, ox, oy));
	}
	i32 setting = (i32)tile_uv_node_button(node, TILE_UV_BUTTON_FILTER); // 0 Linear, 1 Cubic, 2 Nearest
	i32 filter  = setting == 0 ? IMAGE_FILTER_LINEAR : setting == 1 ? IMAGE_FILTER_CUBIC : IMAGE_FILTER_NEAREST;
	return make_image_node_sample_at(node, p, filter);
}

char *tile_uv_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.rgb", tile_uv_node_sample(node));
}

char *tile_uv_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", tile_uv_node_sample(node));
}

static ui_node_socket_t *tile_uv_node_socket(char *name, char *type, u32 color, f32_array_t *default_value, f32 min, f32 max, f32 precision) {
	return ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                     .node_id       = 0,
	                                     .name          = name,
	                                     .type          = type,
	                                     .color         = color,
	                                     .default_value = default_value,
	                                     .min           = min,
	                                     .max           = max,
	                                     .precision     = precision,
	                                     .display       = 0});
}

static ui_node_button_t *tile_uv_node_setting(char *name, f32 default_value, char *data) {
	return ALLOC_INIT(ui_node_button_t, {.name          = name,
	                                     .type          = "ENUM",
	                                     .output        = -1,
	                                     .default_value = f32_array_create_x(default_value),
	                                     .data          = u8_array_create_from_string(data),
	                                     .min           = 0.0,
	                                     .max           = 1.0,
	                                     .precision     = 100,
	                                     .height        = 0});
}

void tile_uv_node_init() {
	u32 value_color  = 0xffa1a1a1;
	u32 vector_color = 0xff6363c7;

	ui_node_t *tile_uv_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Tile UV"),
	                .type   = "TEX_TILE_UV",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        tile_uv_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 0.0, 1.0, 100),
	                        tile_uv_node_socket(_tr("Scale X"), "VALUE", value_color, f32_array_create_x(1.0), 0.0, 32.0, 100),
	                        tile_uv_node_socket(_tr("Scale Y"), "VALUE", value_color, f32_array_create_x(1.0), 0.0, 32.0, 100),
	                        tile_uv_node_socket(_tr("Rotation"), "VALUE", value_color, f32_array_create_x(0.0), -180.0, 180.0, 10),
	                        tile_uv_node_socket(_tr("Offset X"), "VALUE", value_color, f32_array_create_x(0.0), -1.0, 1.0, 100),
	                        tile_uv_node_socket(_tr("Offset Y"), "VALUE", value_color, f32_array_create_x(0.0), -1.0, 1.0, 100),
	                        tile_uv_node_socket(_tr("Vector"), "VECTOR", vector_color, f32_array_create_xyz(0.0, 0.0, 0.0), 0.0, 1.0, 100),
	                    },
	                    7),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        tile_uv_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        tile_uv_node_setting(_tr("Size"), 1, "512\n1024\n2048\n4096"),
	                        make_image_node_state_button(),
	                        tile_uv_node_setting(_tr("Filter"), 1, "Linear\nCubic\nNearest"),
	                    },
	                    3),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, tile_uv_node_def);
	any_map_set(parser_material_node_values, "TEX_TILE_UV", tile_uv_node_value);
	any_map_set(parser_material_node_vectors, "TEX_TILE_UV", tile_uv_node_vector);
	make_image_node_register(&tile_uv_node_image_type);
}
