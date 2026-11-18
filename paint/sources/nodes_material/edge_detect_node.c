#include "../global.h"

// Edge Detect (Image menu): edges of the Image input. An image node (render/make_image_node.c): the Image input is
// evaluated in image space (grey in R, G and B, 32 bit float so smooth inputs do not band) and the edge image is computed on the cpu into one RGBA128 texture
// per node, R = Image (the edge mask). The texture is sampled like an Image Texture at tex_coord, repeating (Tile UV places it on the uv).
//
// A one pixel edge (Sobel gradient magnitude) widened by Edge Width (a fraction of Size) with an isotropic blur, see
// edge_detect_node_process. Gradient mode: ramps from 0 at Tolerance to 1 at the largest gradient of the image, so a
// smooth input (noise, distance fields) still gives visible edges. Boundary mode: 1 where the gradient exceeds
// Tolerance, else 0 (soft sides after the widening). The result is kept in 32 bit float.
// Edge Width and Tolerance hold for the whole image (scalar, evaluated at the image centre when linked): the blur
// width has to be known per bake, and a per pixel link has no neighbours to compare in image space.

// Input socket indices
enum {
	EDGE_DETECT_INPUT_IMAGE      = 0,
	EDGE_DETECT_INPUT_EDGE_WIDTH = 1,
	EDGE_DETECT_INPUT_TOLERANCE  = 2,
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

// One box blur pass of odd width w along x (horizontal) or y, wrapping or clamping, by a running sum
static void edge_detect_node_box_pass(f32 *src, f32 *dst, i32 size, i32 w, bool wrap, bool horizontal) {
	i32 half = w / 2;
	for (i32 l = 0; l < size; ++l) {
		f32 sum = 0.0;
		for (i32 k = -half; k <= half; ++k) {
			i32 i = edge_detect_node_wrap_index(k, size, wrap);
			sum += horizontal ? src[((size_t)l * size + i)] : src[((size_t)i * size + l)];
		}
		for (i32 i = 0; i < size; ++i) {
			if (horizontal) {
				dst[(size_t)l * size + i] = sum / (f32)w;
			}
			else {
				dst[(size_t)i * size + l] = sum / (f32)w;
			}
			i32 add = edge_detect_node_wrap_index(i + half + 1, size, wrap);
			i32 sub = edge_detect_node_wrap_index(i - half, size, wrap);
			sum += horizontal ? src[(size_t)l * size + add] - src[(size_t)l * size + sub] : src[(size_t)add * size + l] - src[(size_t)sub * size + l];
		}
	}
}

// Edge image from the captured grey (R of every pixel); returns 0.
// Two steps: a one pixel edge (Sobel gradient magnitude, a step of 1 gives 1), then the Edge Width: the edge is blurred
// (three box passes in x and y, a Gaussian within a few percent, isotropic) and scaled back so a straight edge peaks at
// 1. Every direction is treated alike, so a diagonal border has no gaps or stairs (comparing eight neighbours at a
// distance does). Sigma is 0.85 times the Edge Width in pixels, so the ridge is about twice Edge Width wide at half height.
static i32 edge_detect_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	f32 *pixels = (f32 *)bytes; // Float capture
	edge_detect_params_t p = *(edge_detect_params_t *)params;
	f32 width_px           = p.edge_width * (f32)size;
	f32 tol                = p.tolerance < 0.0 ? 0.0 : p.tolerance > 1.0 ? 1.0 : p.tolerance;
	size_t n               = (size_t)size * size;
	f32 *e                 = malloc(n * sizeof(f32));
	f32 *t                 = malloc(n * sizeof(f32));
	if (e == NULL || t == NULL) {
		free(e);
		free(t);
		return -1;
	}
	f32 max_e = 0.0;
	for (i32 y = 0; y < size; ++y) {
		for (i32 x = 0; x < size; ++x) {
			i32 xm = edge_detect_node_wrap_index(x - 1, size, p.wrap);
			i32 xp = edge_detect_node_wrap_index(x + 1, size, p.wrap);
			i32 ym = edge_detect_node_wrap_index(y - 1, size, p.wrap);
			i32 yp = edge_detect_node_wrap_index(y + 1, size, p.wrap);
#define ED_AT(px_, py_) pixels[((size_t)(py_) * size + (px_)) * 4]
			f32 gx  = (ED_AT(xp, ym) + 2.0f * ED_AT(xp, y) + ED_AT(xp, yp)) - (ED_AT(xm, ym) + 2.0f * ED_AT(xm, y) + ED_AT(xm, yp));
			f32 gy  = (ED_AT(xm, yp) + 2.0f * ED_AT(x, yp) + ED_AT(xp, yp)) - (ED_AT(xm, ym) + 2.0f * ED_AT(x, ym) + ED_AT(xp, ym));
#undef ED_AT
			f32 mag = sqrtf(gx * gx + gy * gy) * 0.25f;
			e[(size_t)y * size + x] = mag;
			max_e                   = mag > max_e ? mag : max_e;
		}
	}
	f32 ramp = max_e - tol;
	for (size_t i = 0; i < n; ++i) {
		f32 v = e[i];
		if (p.boundary) {
			e[i] = v > tol ? 1.0f : 0.0f;
		}
		else {
			v    = v <= tol || ramp <= 0.0001 ? 0.0f : (v - tol) / ramp;
			e[i] = v > 1.0f ? 1.0f : v;
		}
	}
	f32 sigma = 0.85f * width_px;
	if (sigma >= 0.6f) {
		i32 w = 2 * (i32)((sqrtf(4.0f * sigma * sigma + 1.0f) - 1.0f) * 0.5f + 0.5f) + 1; // Odd, three boxes of w: sigma^2 = (w^2 - 1) / 4
		w     = w > size - 1 ? size - 1 : w;
		for (i32 k = 0; k < 3; ++k) {
			edge_detect_node_box_pass(e, t, size, w, p.wrap, true);
			edge_detect_node_box_pass(t, e, size, w, p.wrap, false);
		}
		f32 fw   = (f32)w;
		f32 gain = (4.0f * fw * fw * fw) / (3.0f * fw * fw + 1.0f); // 1 / peak of a one pixel wide line
		for (size_t i = 0; i < n; ++i) {
			f32 v = e[i] * gain;
			v     = v > 1.0f ? 1.0f : v;
			if (p.boundary) {
				f32 u = (v - 0.4f) / 0.2f; // Smoothstep around half height: a band of constant width with soft sides
				u     = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
				v     = u * u * (3.0f - 2.0f * u);
			}
			e[i] = v;
		}
	}
	free(t);
	for (size_t i = 0; i < n; ++i) {
		f32 v = e[i];
		result[i * 4]     = v;
		result[i * 4 + 1] = v;
		result[i * 4 + 2] = v;
		result[i * 4 + 3] = 1.0;
	}
	free(e);
	return 0;
}

static image_node_type_t edge_detect_node_image_type = {
    .type          = "TEX_EDGE_DETECT",
    .result_format = GPU_TEXTURE_FORMAT_RGBA128,
    .params_size   = sizeof(edge_detect_params_t),
    .size          = edge_detect_node_size,
    .capture       = edge_detect_node_capture,
    .capture_float = true,
    .smooth_sample = true,
    .params        = edge_detect_node_params,
    .process       = edge_detect_node_process,
};

char *edge_detect_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
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

	ui_node_t *edge_detect_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Edge Detect"),
	                .type   = "TEX_EDGE_DETECT",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_FILTER_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        edge_detect_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 1.0, 100),
	                        edge_detect_node_socket(_tr("Edge Width"), "VALUE", value_color, f32_array_create_x(0.005), 0.1, 1000),
	                        edge_detect_node_socket(_tr("Tolerance"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                    },
	                    3),
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

	any_array_push(nodes_material_filter, edge_detect_node_def);
	any_map_set(parser_material_node_values, "TEX_EDGE_DETECT", edge_detect_node_value);
	any_map_set(parser_material_node_vectors, "TEX_EDGE_DETECT", edge_detect_node_vector);
	make_image_node_register(&edge_detect_node_image_type);
}
