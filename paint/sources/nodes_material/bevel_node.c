#include "../global.h"

// Bevel (Filter menu): distance from the edge of a mask, shaped into a height. An image node (render/make_image_node.c):
// the Image input (the mask) is evaluated in image space (grey in R, G and B, 32 bit float), the distance field and the
// bevel are computed on the cpu into one RGBA128 texture per node, R = Image (the bevel height), G = Distance (the
// distance itself). The texture is sampled like an Image Texture at tex_coord, repeating (Tile UV places it on the uv).
//
// The mask is split at 0.5 into inside and outside. Mode Inner measures the distance of every inside pixel to the
// nearest outside pixel, Outer the distance of every outside pixel to the nearest inside pixel, Both does both. The
// distance is an exact Euclidean distance transform (Felzenszwalb and Huttenlocher, two separable passes, every line
// independent so the lines run on all hardware threads), measured to the edge half a pixel before the neighbouring
// pixel centre. Wrap Edges measures across the image border (the passes run over three copies of every line).
//
// Distance is a fraction of Size (the width of the bevel), Smoothing shapes it. With t the distance divided by Distance
// (0 at the edge, 1 at the full width): Smoothing 0 is a chamfer (height = t), Smoothing 1 a round bevel (a quarter
// circle, height = sqrt(1 - (1 - t)^2)), in between a mix of both. The Distance output is the same without the shaping.
//   Inner: 0 outside the mask and on its edge, rising to 1 at Distance inside.
//   Outer: 1 inside the mask and on its edge, falling to 0 at Distance outside.
//   Both:  0.5 on the edge, rising to 1 at Distance inside and falling to 0 at Distance outside.
// Distance and Smoothing hold for the whole image (scalar, evaluated at the image centre when linked): the width has
// to be known per bake, and a per pixel link has no neighbours to measure in image space.

// Input socket indices
enum {
	BEVEL_INPUT_IMAGE     = 0,
	BEVEL_INPUT_DISTANCE  = 1,
	BEVEL_INPUT_SMOOTHING = 2,
};

// Output socket indices
enum {
	BEVEL_OUTPUT_IMAGE    = 0,
	BEVEL_OUTPUT_DISTANCE = 1,
};

// Node button indices (settings)
enum {
	BEVEL_BUTTON_SIZE = 0, // Button 1 is the state row
	BEVEL_BUTTON_MODE = 2,
	BEVEL_BUTTON_WRAP = 3,
};

// Mode setting
enum {
	BEVEL_MODE_INNER = 0,
	BEVEL_MODE_OUTER = 1,
	BEVEL_MODE_BOTH  = 2,
};

#define BEVEL_FAR 1.0e20 // Squared distance of a pixel without any feature in its line (no edge at all)

typedef struct bevel_params {
	i32 mode;
	i32 wrap;
	f32 distance;
	f32 smoothing;
} bevel_params_t;

static f32 bevel_node_button(ui_node_t *node, i32 i) {
	return node->buttons->buffer[i]->default_value->buffer[0];
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 bevel_node_size(ui_node_t *node) {
	i32 i = (i32)bevel_node_button(node, BEVEL_BUTTON_SIZE);
	return 512 << (i < 0 ? 0 : i > 3 ? 3 : i);
}

// Captured per pixel: the Image input (the mask) as grey
static char *bevel_node_capture(ui_node_t *node) {
	char *image = parser_material_parse_value_input(node->inputs->buffer[BEVEL_INPUT_IMAGE], false);
	char *grey  = string_tmp("%s_grey", parser_material_store_var_name(node));
	parser_material_write(parser_material_kong, string_tmp("float %s = clamp(%s, 0.0, 1.0);", grey, image));
	return string_tmp("float4(%s, %s, %s, 1.0)", grey, grey, grey);
}

static void bevel_node_params(ui_node_t *node, void *params) {
	bevel_params_t *p = params;
	p->mode           = (i32)bevel_node_button(node, BEVEL_BUTTON_MODE);
	p->wrap           = bevel_node_button(node, BEVEL_BUTTON_WRAP) != 0.0;
	p->distance       = make_image_node_input_value(node, BEVEL_INPUT_DISTANCE);
	p->smoothing      = make_image_node_input_value(node, BEVEL_INPUT_SMOOTHING);
}

// Squared distance transform of f over n samples (d[q] = min over p of (q - p)^2 + f[p]): the lower envelope of the parabolas
// rooted at every sample. v, z are scratch (n and n + 1 entries)
static void bevel_node_edt_line(const f64 *f, f64 *d, i32 n, i32 *v, f64 *z) {
	i32 k = 0;
	v[0]  = 0;
	z[0]  = -BEVEL_FAR;
	z[1]  = BEVEL_FAR;
	for (i32 q = 1; q < n; ++q) {
		f64 s;
		while (true) {
			i32 p = v[k];
			s     = ((f[q] + (f64)q * q) - (f[p] + (f64)p * p)) / (2.0 * q - 2.0 * p);
			if (s <= z[k] && k > 0) {
				--k;
			}
			else {
				break;
			}
		}
		++k;
		v[k]     = q;
		z[k]     = s;
		z[k + 1] = BEVEL_FAR;
	}
	k = 0;
	for (i32 q = 0; q < n; ++q) {
		while (z[k + 1] < q) {
			++k;
		}
		f64 dq = (f64)(q - v[k]);
		d[q]   = dq * dq + f[v[k]];
	}
}

typedef struct bevel_edt_ctx {
	const u8 *feature; // 1 where the pixel is a feature (distance 0), size * size
	f32      *g;       // Squared distance along the columns, size * size
	f32      *dist;    // Result: the distance in pixels, size * size
	i32       size;
	bool      wrap;
} bevel_edt_ctx_t;

// One line of the transform: f has size samples, tripled when wrap (the middle copy is the result, so the distance
// reaches through the border); the result d is read back into out with the stride
static void bevel_node_edt_run(const f64 *line, f64 *work, f64 *d, i32 *v, f64 *z, i32 size, bool wrap) {
	i32 n = wrap ? size * 3 : size;
	if (wrap) {
		for (i32 c = 0; c < 3; ++c) {
			memcpy(work + (size_t)c * size, line, (size_t)size * sizeof(f64));
		}
		bevel_node_edt_line(work, d, n, v, z);
	}
	else {
		bevel_node_edt_line(line, d, n, v, z);
	}
}

// Columns [begin, end): the distance to the nearest feature of the same column (squared)
static void bevel_node_edt_columns(void *param, i32 begin, i32 end) {
	bevel_edt_ctx_t *c    = param;
	i32              size = c->size;
	i32              n    = c->wrap ? size * 3 : size;
	f64             *line = malloc((size_t)size * sizeof(f64));
	f64             *work = malloc((size_t)n * sizeof(f64));
	f64             *d    = malloc((size_t)n * sizeof(f64));
	i32             *v    = malloc((size_t)n * sizeof(i32));
	f64             *z    = malloc((size_t)(n + 1) * sizeof(f64));
	if (line != NULL && work != NULL && d != NULL && v != NULL && z != NULL) {
		for (i32 x = begin; x < end; ++x) {
			for (i32 y = 0; y < size; ++y) {
				line[y] = c->feature[(size_t)y * size + x] != 0 ? 0.0 : BEVEL_FAR;
			}
			bevel_node_edt_run(line, work, d, v, z, size, c->wrap);
			f64 *out = c->wrap ? d + size : d;
			for (i32 y = 0; y < size; ++y) {
				c->g[(size_t)y * size + x] = (f32)(out[y] > BEVEL_FAR ? BEVEL_FAR : out[y]);
			}
		}
	}
	free(line);
	free(work);
	free(d);
	free(v);
	free(z);
}

// Rows [begin, end): the transform of the column distances along the row, then the square root, half a pixel nearer
static void bevel_node_edt_rows(void *param, i32 begin, i32 end) {
	bevel_edt_ctx_t *c    = param;
	i32              size = c->size;
	i32              n    = c->wrap ? size * 3 : size;
	f64             *line = malloc((size_t)size * sizeof(f64));
	f64             *work = malloc((size_t)n * sizeof(f64));
	f64             *d    = malloc((size_t)n * sizeof(f64));
	i32             *v    = malloc((size_t)n * sizeof(i32));
	f64             *z    = malloc((size_t)(n + 1) * sizeof(f64));
	if (line != NULL && work != NULL && d != NULL && v != NULL && z != NULL) {
		for (i32 y = begin; y < end; ++y) {
			for (i32 x = 0; x < size; ++x) {
				line[x] = c->g[(size_t)y * size + x];
			}
			bevel_node_edt_run(line, work, d, v, z, size, c->wrap);
			f64 *out = c->wrap ? d + size : d;
			for (i32 x = 0; x < size; ++x) {
				// The pixel centres sit half a pixel from the edge between an inside and an outside pixel
				f64 px                         = out[x] >= BEVEL_FAR ? BEVEL_FAR : sqrt(out[x]) - 0.5;
				c->dist[(size_t)y * size + x] = (f32)(px < 0.0 ? 0.0 : px);
			}
		}
	}
	free(line);
	free(work);
	free(d);
	free(v);
	free(z);
}

// Distance in pixels of every pixel to the nearest pixel where feature is set (BEVEL_FAR when there is none)
static void bevel_node_distance(const u8 *feature, f32 *g, f32 *dist, i32 size, bool wrap) {
	bevel_edt_ctx_t c = {.feature = feature, .g = g, .dist = dist, .size = size, .wrap = wrap};
	make_image_node_parallel(size, bevel_node_edt_columns, &c);
	make_image_node_parallel(size, bevel_node_edt_rows, &c);
}

// Shaped height of t in 0..1: a chamfer at smoothing 0, a quarter circle at 1
static f32 bevel_node_profile(f32 t, f32 smoothing) {
	f32 u     = 1.0f - t;
	f32 round = sqrtf(u < 1.0f ? 1.0f - u * u : 0.0f);
	return t + (round - t) * smoothing;
}

typedef struct bevel_shape_ctx {
	const f32 *inner; // Distance of the inside pixels in pixels, NULL when not needed
	const f32 *outer; // Distance of the outside pixels in pixels, NULL when not needed
	const u8  *inside;
	f32       *result;
	i32        size;
	i32        mode;
	f32        width_px;
	f32        smoothing;
} bevel_shape_ctx_t;

// Rows [begin, end): the height (R) and the plain distance (G) of every pixel
static void bevel_node_shape_rows(void *param, i32 begin, i32 end) {
	bevel_shape_ctx_t *c    = param;
	i32                size = c->size;
	for (i32 y = begin; y < end; ++y) {
		for (i32 x = 0; x < size; ++x) {
			size_t i      = (size_t)y * size + x;
			bool   in     = c->inside[i] != 0;
			f32    height = 0.0f;
			f32    dist   = 0.0f;
			if (c->mode == BEVEL_MODE_INNER) {
				f32 t  = in ? fminf(c->inner[i] / c->width_px, 1.0f) : 0.0f;
				dist   = t;
				height = bevel_node_profile(t, c->smoothing);
			}
			else if (c->mode == BEVEL_MODE_OUTER) {
				f32 t  = in ? 0.0f : fminf(c->outer[i] / c->width_px, 1.0f);
				dist   = 1.0f - t;
				height = 1.0f - bevel_node_profile(t, c->smoothing);
			}
			else {
				if (in) {
					f32 t  = fminf(c->inner[i] / c->width_px, 1.0f);
					dist   = 0.5f + 0.5f * t;
					height = 0.5f + 0.5f * bevel_node_profile(t, c->smoothing);
				}
				else {
					f32 t  = fminf(c->outer[i] / c->width_px, 1.0f);
					dist   = 0.5f - 0.5f * t;
					height = 0.5f - 0.5f * bevel_node_profile(t, c->smoothing);
				}
			}
			c->result[i * 4]     = height;
			c->result[i * 4 + 1] = dist;
			c->result[i * 4 + 2] = height;
			c->result[i * 4 + 3] = 1.0f;
		}
	}
}

static i32 bevel_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	f32           *pixels = (f32 *)bytes; // Float capture
	bevel_params_t p      = *(bevel_params_t *)params;
	size_t         n      = (size_t)size * size;
	u8            *inside = malloc(n);
	u8            *edge   = malloc(n);
	f32           *g      = malloc(n * sizeof(f32));
	f32           *inner  = p.mode != BEVEL_MODE_OUTER ? malloc(n * sizeof(f32)) : NULL;
	f32           *outer  = p.mode != BEVEL_MODE_INNER ? malloc(n * sizeof(f32)) : NULL;
	if (inside == NULL || edge == NULL || g == NULL || (p.mode != BEVEL_MODE_OUTER && inner == NULL) || (p.mode != BEVEL_MODE_INNER && outer == NULL)) {
		free(inside);
		free(edge);
		free(g);
		free(inner);
		free(outer);
		return -1;
	}
	for (size_t i = 0; i < n; ++i) {
		inside[i] = pixels[i * 4] >= 0.5f ? 1 : 0;
	}
	if (inner != NULL) { // Inside pixels: the nearest outside pixel is the feature
		for (size_t i = 0; i < n; ++i) {
			edge[i] = inside[i] ? 0 : 1;
		}
		bevel_node_distance(edge, g, inner, size, p.wrap != 0);
	}
	if (outer != NULL) { // Outside pixels: the nearest inside pixel is the feature
		bevel_node_distance(inside, g, outer, size, p.wrap != 0);
	}
	f32 width_px = p.distance * (f32)size;
	width_px     = width_px < 1.0f ? 1.0f : width_px;
	f32 smooth   = p.smoothing < 0.0f ? 0.0f : p.smoothing > 1.0f ? 1.0f : p.smoothing;
	bevel_shape_ctx_t sc = {.inner = inner, .outer = outer, .inside = inside, .result = result, .size = size, .mode = p.mode, .width_px = width_px, .smoothing = smooth};
	make_image_node_parallel(size, bevel_node_shape_rows, &sc);
	free(inside);
	free(edge);
	free(g);
	free(inner);
	free(outer);
	return 0;
}

static image_node_type_t bevel_node_image_type = {
    .type          = "TEX_BEVEL",
    .result_format = GPU_TEXTURE_FORMAT_RGBA128,
    .params_size   = sizeof(bevel_params_t),
    .size          = bevel_node_size,
    .capture       = bevel_node_capture,
    .capture_float = true,
    .smooth_sample = true,
    .params        = bevel_node_params,
    .process       = bevel_node_process,
};

char *bevel_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	char *bevel = make_image_node_sample(node);
	if (array_index_of(node->outputs, socket) == BEVEL_OUTPUT_DISTANCE) {
		return string_tmp("%s.g", bevel);
	}
	return string_tmp("%s.r", bevel);
}

char *bevel_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return parser_material_to_vec3(bevel_node_value(node, socket));
}

static ui_node_socket_t *bevel_node_socket(char *name, char *type, u32 color, f32_array_t *default_value, f32 max, f32 precision) {
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

static ui_node_button_t *bevel_node_setting(char *name, char *type, f32 default_value, char *data) {
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

void bevel_node_init() {
	u32 value_color = 0xffa1a1a1;

	ui_node_t *bevel_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Bevel"),
	                .type   = "TEX_BEVEL",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        bevel_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(1.0), 1.0, 100),
	                        bevel_node_socket(_tr("Distance"), "VALUE", value_color, f32_array_create_x(0.05), 0.5, 1000),
	                        bevel_node_socket(_tr("Smoothing"), "VALUE", value_color, f32_array_create_x(0.5), 1.0, 100),
	                    },
	                    3),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        bevel_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 1.0, 100),
	                        bevel_node_socket(_tr("Distance"), "VALUE", value_color, f32_array_create_x(0.0), 1.0, 100),
	                    },
	                    2),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        bevel_node_setting(_tr("Size"), "ENUM", 1, "512\n1024\n2048\n4096"),
	                        make_image_node_state_button(),
	                        bevel_node_setting(_tr("Mode"), "ENUM", 0, "Inner\nOuter\nBoth"),
	                        bevel_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                    },
	                    4),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, bevel_node_def);
	any_map_set(parser_material_node_values, "TEX_BEVEL", bevel_node_value);
	any_map_set(parser_material_node_vectors, "TEX_BEVEL", bevel_node_vector);
	make_image_node_register(&bevel_node_image_type);
}
