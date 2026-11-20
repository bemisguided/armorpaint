#include "../global.h"

// Edge Detect (Filter menu): edges of the Image input. An image node (render/make_image_node.c): the Image input is
// evaluated in image space (grey in R, G and B, 32 bit float so smooth inputs do not band) and the edge image is computed on the cpu into one RGBA128 texture
// per node, R = Image (the edge mask). The texture is sampled like an Image Texture at tex_coord, repeating (Tile UV places it on the uv).
//
// A one pixel edge (Sobel gradient magnitude) widened by Edge Width (a fraction of Size) with an isotropic blur, see
// edge_detect_node_process. Edge Width 0 (below about 0.0007 at Size 1024) is not widened but antialiased: the captured
// grey (one sample per pixel, so a diagonal border is a hard staircase) is smoothed with [1 2 1] / 4 before the Sobel,
// giving a line 1 to 2.5 pixels wide whose slope has no stairs. Gradient mode: ramps from 0 at Tolerance to 1 at the largest gradient of the image, so a
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
	EDGE_DETECT_BUTTON_SIZE = 0, // Button 1 is the state row
	EDGE_DETECT_BUTTON_MODE = 2,
	EDGE_DETECT_BUTTON_WRAP = 3,
};

typedef struct edge_detect_params {
	i32 boundary; // 0 Gradient, 1 Boundary
	i32 wrap;
	f32 edge_width;
	f32 tolerance;
} edge_detect_params_t;

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 edge_detect_node_size(ui_node_t *node) {
	return make_image_node_size(node, EDGE_DETECT_BUTTON_SIZE);
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
	p->boundary             = make_image_node_button(node, EDGE_DETECT_BUTTON_MODE) != 0.0;
	p->wrap                 = make_image_node_button(node, EDGE_DETECT_BUTTON_WRAP) != 0.0;
	p->edge_width           = make_image_node_input_value(node, EDGE_DETECT_INPUT_EDGE_WIDTH);
	p->tolerance            = make_image_node_input_value(node, EDGE_DETECT_INPUT_TOLERANCE);
}

static i32 edge_detect_node_wrap_index(i32 i, i32 size, bool wrap) {
	if (wrap) {
		return ((i % size) + size) % size;
	}
	return i < 0 ? 0 : i >= size ? size - 1 : i;
}

typedef struct edge_detect_box_ctx {
	f32 *src;
	f32 *dst;
	i32  size;
	i32  w;
	bool wrap;
	bool horizontal;
} edge_detect_box_ctx_t;

// Lines [begin, end) of one box blur pass of odd width w along x (horizontal) or y, wrapping or clamping, by a running sum
static void edge_detect_node_box_lines(void *param, i32 begin, i32 end) {
	edge_detect_box_ctx_t *c = param;
	f32 *src = c->src, *dst = c->dst;
	i32  size = c->size, w = c->w, half = w / 2;
	bool wrap = c->wrap, horizontal = c->horizontal;
	for (i32 l = begin; l < end; ++l) {
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

static void edge_detect_node_box_pass(f32 *src, f32 *dst, i32 size, i32 w, bool wrap, bool horizontal) {
	edge_detect_box_ctx_t c = {.src = src, .dst = dst, .size = size, .w = w, .wrap = wrap, .horizontal = horizontal};
	make_image_node_parallel(size, edge_detect_node_box_lines, &c);
}

typedef struct edge_detect_binomial_ctx {
	f32 *src;
	f32 *dst;
	i32  src_stride; // Floats between two pixels of src: 4 for the captured RGBA, 1 for a plane
	i32  size;
	bool wrap;
	bool horizontal;
} edge_detect_binomial_ctx_t;

// Rows [begin, end) of the output of one [1 2 1] / 4 pass along x (horizontal) or y, wrapping or clamping
static void edge_detect_node_binomial_rows(void *param, i32 begin, i32 end) {
	edge_detect_binomial_ctx_t *c = param;
	f32 *src = c->src, *dst = c->dst;
	i32  size = c->size, stride = c->src_stride;
	for (i32 y = begin; y < end; ++y) {
		f32 *out = dst + (size_t)y * size;
		if (c->horizontal) {
			f32 *row = src + (size_t)y * size * stride;
			for (i32 x = 0; x < size; ++x) {
				i32 xm = edge_detect_node_wrap_index(x - 1, size, c->wrap);
				i32 xp = edge_detect_node_wrap_index(x + 1, size, c->wrap);
				out[x] = 0.25f * row[(size_t)xm * stride] + 0.5f * row[(size_t)x * stride] + 0.25f * row[(size_t)xp * stride];
			}
		}
		else {
			f32 *rm = src + (size_t)edge_detect_node_wrap_index(y - 1, size, c->wrap) * size * stride;
			f32 *r0 = src + (size_t)y * size * stride;
			f32 *rp = src + (size_t)edge_detect_node_wrap_index(y + 1, size, c->wrap) * size * stride;
			for (i32 x = 0; x < size; ++x) {
				out[x] = 0.25f * rm[(size_t)x * stride] + 0.5f * r0[(size_t)x * stride] + 0.25f * rp[(size_t)x * stride];
			}
		}
	}
}

static void edge_detect_node_binomial_pass(f32 *src, i32 src_stride, f32 *dst, i32 size, bool wrap, bool horizontal) {
	edge_detect_binomial_ctx_t c = {.src = src, .dst = dst, .src_stride = src_stride, .size = size, .wrap = wrap, .horizontal = horizontal};
	make_image_node_parallel(size, edge_detect_node_binomial_rows, &c);
}

typedef struct edge_detect_sobel_ctx {
	f32 *pixels;
	i32  stride; // Floats between two pixels of pixels: 4 for the captured RGBA, 1 for a plane
	f32 *e;
	f32 *max_rows; // Largest magnitude of every row
	f32  scale;    // 0.25 gives a step of 1 the magnitude 1; 1 / 3 for a [1 2 1] smoothed input (a smoothed step gives 0.75)
	i32  size;
	bool wrap;
} edge_detect_sobel_ctx_t;

// Rows [begin, end) of the Sobel gradient magnitude of the grey (a step of 1 gives 1)
static void edge_detect_node_sobel_rows(void *param, i32 begin, i32 end) {
	edge_detect_sobel_ctx_t *c = param;
	f32 *pixels = c->pixels;
	i32  size = c->size, stride = c->stride;
	for (i32 y = begin; y < end; ++y) {
		f32 row_max = 0.0;
		for (i32 x = 0; x < size; ++x) {
			i32 xm = edge_detect_node_wrap_index(x - 1, size, c->wrap);
			i32 xp = edge_detect_node_wrap_index(x + 1, size, c->wrap);
			i32 ym = edge_detect_node_wrap_index(y - 1, size, c->wrap);
			i32 yp = edge_detect_node_wrap_index(y + 1, size, c->wrap);
#define ED_AT(px_, py_) pixels[((size_t)(py_) * size + (px_)) * stride]
			f32 gx  = (ED_AT(xp, ym) + 2.0f * ED_AT(xp, y) + ED_AT(xp, yp)) - (ED_AT(xm, ym) + 2.0f * ED_AT(xm, y) + ED_AT(xm, yp));
			f32 gy  = (ED_AT(xm, yp) + 2.0f * ED_AT(x, yp) + ED_AT(xp, yp)) - (ED_AT(xm, ym) + 2.0f * ED_AT(x, ym) + ED_AT(xp, ym));
#undef ED_AT
			f32 mag = sqrtf(gx * gx + gy * gy) * c->scale;
			c->e[(size_t)y * size + x] = mag;
			row_max                    = mag > row_max ? mag : row_max;
		}
		c->max_rows[y] = row_max;
	}
}

// Edge image from the captured grey (R of every pixel); returns 0.
// Two steps: a one pixel edge (Sobel gradient magnitude, a step of 1 gives 1), then the Edge Width: the edge is blurred
// (three box passes in x and y, a Gaussian within a few percent, isotropic) and scaled back so a straight edge peaks at
// 1. Every direction is treated alike, so a diagonal border has no gaps or stairs (comparing eight neighbours at a
// distance does). Sigma is 0.85 times the Edge Width in pixels, so the ridge is about twice Edge Width wide at half height.
// Without widening (sigma < 0.6) the input is smoothed with [1 2 1] / 4 (x then y) before the Sobel and the magnitude is
// scaled by 4 / 3, so a step of 1 still gives 1 but its slope spreads over two pixels: a diagonal border does not
// show the texel staircase of the capture (which has no coverage information) in the surface slope Bump reads.
static i32 edge_detect_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	f32 *pixels = (f32 *)bytes; // Float capture
	edge_detect_params_t p = *(edge_detect_params_t *)params;
	f32 width_px           = p.edge_width * (f32)size;
	width_px               = width_px > (f32)size ? (f32)size : width_px; // The box width is capped to the size below anyway, keeps huge linked values out of the int cast
	f32 tol                = p.tolerance < 0.0 ? 0.0 : p.tolerance > 1.0 ? 1.0 : p.tolerance;
	size_t n               = (size_t)size * size;
	size_t plane_bytes     = n * sizeof(f32);
	f32 *e                 = make_image_node_plane_alloc(plane_bytes);
	f32 *t                 = make_image_node_plane_alloc(plane_bytes);
	if (e == NULL || t == NULL) {
		make_image_node_plane_free(e, plane_bytes);
		make_image_node_plane_free(t, plane_bytes);
		return -1;
	}
	f32 *max_rows = malloc((size_t)size * sizeof(f32));
	if (max_rows == NULL) {
		make_image_node_plane_free(e, plane_bytes);
		make_image_node_plane_free(t, plane_bytes);
		return -1;
	}
	f32 sigma = 0.85f * width_px;
	bool smooth = sigma < 0.6f; // No widening follows: antialias the input instead
	edge_detect_sobel_ctx_t sc = {.pixels = pixels, .stride = 4, .e = e, .max_rows = max_rows, .scale = 0.25f, .size = size, .wrap = p.wrap};
	if (smooth) {
		// pixels -> t (x) -> e (y), the Sobel reads e and writes t, which then becomes the edge plane
		edge_detect_node_binomial_pass(pixels, 4, t, size, p.wrap, true);
		edge_detect_node_binomial_pass(t, 1, e, size, p.wrap, false);
		sc.pixels = e;
		sc.stride = 1;
		sc.e      = t;
		sc.scale  = 0.25f * (4.0f / 3.0f);
	}
	make_image_node_parallel(size, edge_detect_node_sobel_rows, &sc);
	if (smooth) {
		f32 *s = e;
		e      = t;
		t      = s;
	}
	f32 max_e = 0.0;
	for (i32 y = 0; y < size; ++y) {
		max_e = max_rows[y] > max_e ? max_rows[y] : max_e;
	}
	free(max_rows);
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
	if (!smooth) {
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
	make_image_node_plane_free(t, plane_bytes);
	for (size_t i = 0; i < n; ++i) {
		f32 v = e[i];
		result[i * 4]     = v;
		result[i * 4 + 1] = v;
		result[i * 4 + 2] = v;
		result[i * 4 + 3] = 1.0;
	}
	make_image_node_plane_free(e, plane_bytes);
	return 0;
}

static image_node_type_t edge_detect_node_image_type = {
    .type          = "TEX_EDGE_DETECT",
    .result_format = GPU_TEXTURE_FORMAT_RGBA64, // A mask in 0..1 with ramps no wider than the blur: half floats keep at least a pixel per step
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

void edge_detect_node_init() {
	u32 value_color  = 0xffa1a1a1;

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
	                        make_image_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Edge Width"), "VALUE", value_color, f32_array_create_x(0.005), 0.0, 0.1, 1000),
	                        make_image_node_socket(_tr("Tolerance"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                    },
	                    3),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Size"), "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS),
	                        make_image_node_state_button(),
	                        make_image_node_setting(_tr("Mode"), "ENUM", 0, "Gradient\nBoundary"),
	                        make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                    },
	                    4),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, edge_detect_node_def);
	any_map_set(parser_material_node_values, "TEX_EDGE_DETECT", edge_detect_node_value);
	any_map_set(parser_material_node_vectors, "TEX_EDGE_DETECT", edge_detect_node_vector);
	make_image_node_sections(edge_detect_node_def, NULL, 0, 1); // General: the settings and Image
	make_image_node_register(&edge_detect_node_image_type);
}
