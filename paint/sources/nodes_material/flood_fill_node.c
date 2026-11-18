#include "../global.h"
#include "../util/flood_fill_label.h"

// Flood Fill (Image menu): connected islands of a mask with a random grey, a bounding box gradient and the size per island.
// An image node (render/make_image_node.c): the Image input is evaluated in image space, compared with Threshold per
// pixel, and the inside pixels are labelled on the cpu (util/flood_fill_label.c) into one RGBA64 texture per node:
// R = Image (the island grey), G = Position x, B = Position y, A = Area (floored to 1/1024 inside kept islands,
// so Mask = A > 0). The texture is sampled like an Image Texture at tex_coord, repeating (Tile UV places it on the uv).

#define FLOOD_FILL_AREA_FLOOR 1024.0

// Input socket indices
enum {
	FLOOD_FILL_INPUT_IMAGE     = 0,
	FLOOD_FILL_INPUT_THRESHOLD = 1,
	FLOOD_FILL_INPUT_MIN_SIZE  = 2,
	FLOOD_FILL_INPUT_SEED      = 3,
};

// Node button indices (settings)
enum {
	FLOOD_FILL_BUTTON_SIZE         = 0, // Button 1 is the state row
	FLOOD_FILL_BUTTON_CONNECTIVITY = 2,
	FLOOD_FILL_BUTTON_INVERT       = 3,
	FLOOD_FILL_BUTTON_WRAP         = 4,
};

// Output socket indices
enum {
	FLOOD_FILL_OUTPUT_IMAGE    = 0,
	FLOOD_FILL_OUTPUT_POSITION = 1,
	FLOOD_FILL_OUTPUT_AREA     = 2,
	FLOOD_FILL_OUTPUT_MASK     = 3,
};

static f32 flood_fill_node_button(ui_node_t *node, i32 i) {
	return node->buttons->buffer[i]->default_value->buffer[0];
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 flood_fill_node_size(ui_node_t *node) {
	i32 i = (i32)flood_fill_node_button(node, FLOOD_FILL_BUTTON_SIZE);
	return 512 << (i < 0 ? 0 : i > 3 ? 3 : i);
}

// Captured per pixel: 1 inside the mask (Image above Threshold, at or below with Invert), else 0
static char *flood_fill_node_capture(ui_node_t *node) {
	char *image     = parser_material_parse_value_input(node->inputs->buffer[FLOOD_FILL_INPUT_IMAGE], false);
	char *threshold = parser_material_parse_value_input(node->inputs->buffer[FLOOD_FILL_INPUT_THRESHOLD], false);
	char *inside    = string_tmp("%s_inside", parser_material_store_var_name(node));
	char *compare   = flood_fill_node_button(node, FLOOD_FILL_BUTTON_INVERT) != 0.0 ? "<=" : ">";
	parser_material_write(parser_material_kong, string_tmp("float %s = 0.0;", inside));
	parser_material_write(parser_material_kong, string_tmp("if (%s %s %s) { %s = 1.0; }", image, compare, threshold, inside));
	return string_tmp("float4(%s, %s, %s, 1.0)", inside, inside, inside);
}

// Labelling parameters, width and height are set by the bake. Min Size and Seed hold for the whole image.
static void flood_fill_node_params(ui_node_t *node, void *params) {
	flood_fill_params_t *p        = params;
	f32                  min_size = make_image_node_input_value(node, FLOOD_FILL_INPUT_MIN_SIZE);
	f32                  seed     = make_image_node_input_value(node, FLOOD_FILL_INPUT_SEED);
	p->connectivity8              = flood_fill_node_button(node, FLOOD_FILL_BUTTON_CONNECTIVITY) != 0.0;
	p->wrap                       = flood_fill_node_button(node, FLOOD_FILL_BUTTON_WRAP) != 0.0;
	p->seed                       = (u32)math_floor(seed > 0.0 ? seed : 0.0);
	p->min_size                   = (i32)math_floor(min_size > 0.0 ? min_size : 0.0);
}

// Labels the captured mask into Image, Position x, Position y, Area per pixel; returns the kept islands
static i32 flood_fill_node_process(u8 *pixels, i32 size, void *params, f32 *result) {
	flood_fill_params_t p = *(flood_fill_params_t *)params;
	p.width               = size;
	p.height              = size;
	size_t n              = (size_t)size * size;
	u8    *inside         = malloc(n);
	if (inside == NULL) {
		return -1;
	}
	for (size_t i = 0; i < n; ++i) {
		inside[i] = pixels[i * 4] > 127; // The mask is in R, G and B
	}
	i32 islands = flood_fill_label(inside, &p, result);
	free(inside);
	for (size_t i = 0; islands >= 0 && i < n; ++i) {
		f32 *area = &result[i * 4 + 3];
		if (*area > 0.0 && *area < 1.0 / FLOOD_FILL_AREA_FLOOR) {
			*area = 1.0 / FLOOD_FILL_AREA_FLOOR; // Area above zero marks a kept island, Mask is derived from it
		}
	}
	return islands;
}

static image_node_type_t flood_fill_node_image_type = {
    .type          = "TEX_FLOOD_FILL",
    .result_format = GPU_TEXTURE_FORMAT_RGBA64,
    .params_size   = sizeof(flood_fill_params_t),
    .size          = flood_fill_node_size,
    .capture       = flood_fill_node_capture,
    .params        = flood_fill_node_params,
    .process       = flood_fill_node_process,
};

char *flood_fill_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	char *ff = make_image_node_sample(node);
	i32   i  = array_index_of(node->outputs, socket);
	if (i == FLOOD_FILL_OUTPUT_POSITION) {
		return string_tmp("%s.g", ff);
	}
	if (i == FLOOD_FILL_OUTPUT_AREA) {
		return string_tmp("%s.a", ff);
	}
	if (i == FLOOD_FILL_OUTPUT_MASK) {
		return string_tmp("clamp(%s.a * %s, 0.0, 1.0)", ff, parser_material_vec1(FLOOD_FILL_AREA_FLOOR));
	}
	return string_tmp("%s.r", ff);
}

char *flood_fill_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	if (array_index_of(node->outputs, socket) == FLOOD_FILL_OUTPUT_POSITION) {
		char *ff = make_image_node_sample(node);
		return string_tmp("float3(%s.g, %s.b, 0.0)", ff, ff);
	}
	return parser_material_to_vec3(flood_fill_node_value(node, socket));
}

static ui_node_socket_t *flood_fill_node_socket(char *name, char *type, u32 color, f32_array_t *default_value, f32 max, f32 precision) {
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

static ui_node_button_t *flood_fill_node_setting(char *name, char *type, f32 default_value, char *data) {
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

void flood_fill_node_init() {
	u32 value_color  = 0xffa1a1a1;
	u32 vector_color = 0xff6363c7;

	ui_node_t *flood_fill_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Flood Fill"),
	                .type   = "TEX_FLOOD_FILL",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        flood_fill_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 1.0, 100), // The mask
	                        flood_fill_node_socket(_tr("Threshold"), "VALUE", value_color, f32_array_create_x(0.5), 1.0, 100),
	                        flood_fill_node_socket(_tr("Min Size"), "VALUE", value_color, f32_array_create_x(0.0), 4096.0, 1),
	                        flood_fill_node_socket(_tr("Seed"), "VALUE", value_color, f32_array_create_x(0.0), 1000.0, 1),
	                    },
	                    4),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        flood_fill_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 1.0, 100), // Island grey
	                        flood_fill_node_socket(_tr("Position"), "VECTOR", vector_color, f32_array_create_xyz(0.0, 0.0, 0.0), 1.0, 100),
	                        flood_fill_node_socket(_tr("Area"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                        flood_fill_node_socket(_tr("Mask"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                    },
	                    4),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        flood_fill_node_setting(_tr("Size"), "ENUM", 1, "512\n1024\n2048\n4096"),
	                        make_image_node_state_button(),
	                        flood_fill_node_setting(_tr("Connectivity"), "ENUM", 0, "4\n8"),
	                        flood_fill_node_setting(_tr("Invert"), "BOOL", 0, NULL),
	                        flood_fill_node_setting(_tr("Wrap Edges"), "BOOL", 0, NULL),
	                    },
	                    5),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, flood_fill_node_def);
	any_map_set(parser_material_node_values, "TEX_FLOOD_FILL", flood_fill_node_value);
	any_map_set(parser_material_node_vectors, "TEX_FLOOD_FILL", flood_fill_node_vector);
	make_image_node_register(&flood_fill_node_image_type);
}
