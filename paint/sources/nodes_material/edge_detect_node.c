#include "../global.h"

// Edge Detect (Image menu): edges of the Image input. An image node (render/make_image_node.c): the Image input is
// evaluated in image space (grey in R, G and B) and the edge image is computed on the cpu into one RGBA64 texture
// per node, R = Image (the edge mask). The texture is sampled like an Image Texture: at the Vector input or
// tex_coord, repeating.
//
// Per pixel the largest difference to the eight neighbours at distance Edge Width (a fraction of Size, at least
// one pixel) is taken. Gradient mode: ramps from 0 at Tolerance to 1 at a difference of 1. Boundary mode: 1 where
// the difference exceeds Tolerance, else 0.
// Edge Width and Tolerance hold for the whole image (scalar, evaluated at the image centre when linked): the
// neighbour distance has to be known per bake, and a per pixel link has no neighbours to compare in image space.

// Input socket indices
enum {
	EDGE_DETECT_INPUT_IMAGE      = 0,
	EDGE_DETECT_INPUT_EDGE_WIDTH = 1,
	EDGE_DETECT_INPUT_TOLERANCE  = 2,
	EDGE_DETECT_INPUT_VECTOR     = 3,
};

// Node button indices (settings)
enum {
	EDGE_DETECT_BUTTON_SIZE = 0,
	EDGE_DETECT_BUTTON_MODE = 1,
	EDGE_DETECT_BUTTON_WRAP = 2,
};

typedef struct edge_detect_params {
	i32 boundary; // 0 Gradient, 1 Boundary
	i32 wrap;
	f32 edge_width;
	f32 tolerance;
} edge_detect_params_t;

static f32 edge_detect_node_button(ui_node_t *node, i32 i) {
	return node->buttons->buffer[i]->default_value->buffer[0];
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 edge_detect_node_size(ui_node_t *node) {
	i32 i = (i32)edge_detect_node_button(node, EDGE_DETECT_BUTTON_SIZE);
	return 512 << (i < 0 ? 0 : i > 3 ? 3 : i);
}

// Captured per pixel: the Image input as grey
static char *edge_detect_node_capture(ui_node_t *node) {
	char *image = parser_material_parse_value_input(node->inputs->buffer[EDGE_DETECT_INPUT_IMAGE], false);
	char *grey  = string_tmp("%s_grey", parser_material_store_var_name(node));
	parser_material_write(parser_material_kong, string_tmp("float %s = clamp(%s, 0.0, 1.0);", grey, image));
	return string_tmp("float4(%s, %s, %s, 1.0)", grey, grey, grey);
}

static void edge_detect_node_params(ui_node_t *node, void *params) {
	edge_detect_params_t *p = params;
	p->boundary             = edge_detect_node_button(node, EDGE_DETECT_BUTTON_MODE) != 0.0;
	p->wrap                 = edge_detect_node_button(node, EDGE_DETECT_BUTTON_WRAP) != 0.0;
	p->edge_width           = make_image_node_input_value(node, EDGE_DETECT_INPUT_EDGE_WIDTH);
	p->tolerance            = make_image_node_input_value(node, EDGE_DETECT_INPUT_TOLERANCE);
}

static i32 edge_detect_node_wrap_index(i32 i, i32 size, bool wrap) {
	if (wrap) {
		return ((i % size) + size) % size;
	}
	return i < 0 ? 0 : i >= size ? size - 1 : i;
}

// Edge image from the captured grey (R of every pixel); returns 0
static i32 edge_detect_node_process(u8 *pixels, i32 size, void *params, f32 *result) {
	edge_detect_params_t p = *(edge_detect_params_t *)params;
	f32 width_px           = p.edge_width * (f32)size;
	i32 r                  = (i32)(width_px + 0.5);
	r                      = r < 1 ? 1 : r > size / 2 ? size / 2 : r;
	f32 tol                = p.tolerance < 0.0 ? 0.0 : p.tolerance > 1.0 ? 1.0 : p.tolerance;
	f32 ramp               = 1.0 - tol;
	static const i32 dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
	static const i32 dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
	for (i32 y = 0; y < size; ++y) {
		for (i32 x = 0; x < size; ++x) {
			i32 c    = pixels[((size_t)y * size + x) * 4];
			i32 best = 0;
			for (i32 k = 0; k < 8; ++k) {
				i32 nx = edge_detect_node_wrap_index(x + dx[k] * r, size, p.wrap);
				i32 ny = edge_detect_node_wrap_index(y + dy[k] * r, size, p.wrap);
				i32 d  = (i32)pixels[((size_t)ny * size + nx) * 4] - c;
				d      = d < 0 ? -d : d;
				best   = d > best ? d : best;
			}
			f32 diff = (f32)best / 255.0;
			f32 edge;
			if (p.boundary) {
				edge = diff > tol ? 1.0 : 0.0;
			}
			else {
				edge = diff <= tol ? 0.0 : ramp > 0.0001 ? (diff - tol) / ramp : 1.0;
				edge = edge > 1.0 ? 1.0 : edge;
			}
			f32 *out = &result[((size_t)y * size + x) * 4];
			out[0]   = edge;
			out[1]   = edge;
			out[2]   = edge;
			out[3]   = 1.0;
		}
	}
	return 0;
}

static image_node_type_t edge_detect_node_image_type = {
    .type          = "TEX_EDGE_DETECT",
    .result_format = GPU_TEXTURE_FORMAT_RGBA64,
    .params_size   = sizeof(edge_detect_params_t),
    .size          = edge_detect_node_size,
    .capture       = edge_detect_node_capture,
    .params        = edge_detect_node_params,
    .process       = edge_detect_node_process,
};

char *edge_detect_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node, EDGE_DETECT_INPUT_VECTOR));
}

char *edge_detect_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return parser_material_to_vec3(edge_detect_node_value(node, socket));
}

static ui_node_socket_t *edge_detect_node_socket(char *name, char *type, u32 color, f32_array_t *default_value, f32 max, f32 precision) {
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

static ui_node_button_t *edge_detect_node_setting(char *name, char *type, f32 default_value, char *data) {
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

void edge_detect_node_init() {
	u32 value_color  = 0xffa1a1a1;
	u32 vector_color = 0xff6363c7;

	ui_node_t *edge_detect_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Edge Detect"),
	                .type   = "TEX_EDGE_DETECT",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        edge_detect_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 1.0, 100),
	                        edge_detect_node_socket(_tr("Edge Width"), "VALUE", value_color, f32_array_create_x(0.005), 0.1, 1000),
	                        edge_detect_node_socket(_tr("Tolerance"), "VALUE", value_color, f32_array_create_x(0.1), 1.0, 100),
	                        edge_detect_node_socket(_tr("Vector"), "VECTOR", vector_color, f32_array_create_xyz(0.0, 0.0, 0.0), 1.0, 100),
	                    },
	                    4),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        edge_detect_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        edge_detect_node_setting(_tr("Size"), "ENUM", 1, "512\n1024\n2048\n4096"),
	                        edge_detect_node_setting(_tr("Mode"), "ENUM", 0, "Gradient\nBoundary"),
	                        edge_detect_node_setting(_tr("Wrap Edges"), "BOOL", 0, NULL),
	                        make_image_node_state_button(),
	                    },
	                    4),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_image, edge_detect_node_def);
	any_map_set(parser_material_node_values, "TEX_EDGE_DETECT", edge_detect_node_value);
	any_map_set(parser_material_node_vectors, "TEX_EDGE_DETECT", edge_detect_node_vector);
	make_image_node_register(&edge_detect_node_image_type);
}
