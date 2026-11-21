#include "../global.h"

// Distance (Filter menu): distance from the edge of a mask, plus the same distance shaped into a bevel height. An image
// node (render/make_image_node.c): the Image input (the mask) is evaluated in image space (grey in R, G and B, 32 bit
// float), the distance field and the bevel are computed on the cpu into one RGBA128 texture per node, R = the bevel
// height, G = the distance itself (the preview shows the grey height). The outputs read them in the order Distance (G),
// Bevel (R). The texture is sampled like an Image Texture at tex_coord, repeating (Tile UV places it on the uv).
//
// The mask is split at Threshold (default 0.5) into inside (mask >= Threshold) and outside. Mode Inner measures the
// distance of every inside pixel to the nearest outside pixel, Outer the distance of every outside pixel to the nearest
// inside pixel, Both does both. The distance is an exact Euclidean distance transform (Felzenszwalb and Huttenlocher,
// two separable passes, every line independent so the lines run on all hardware threads), measured to the edge half a
// pixel before the neighbouring pixel centre. Wrap Edges measures across the image border (the passes run over three
// copies of every line).
//
// Distance is a fraction of Size (the width of the band), Smoothing shapes the Bevel output only. With t the distance
// divided by Distance (0 at the edge, 1 at the full width): the Distance output is t itself, the Bevel output shapes it:
// Smoothing 0 is a chamfer (height = t), Smoothing 1 a round bevel (a quarter circle, height = sqrt(1 - (1 - t)^2)),
// in between a mix of both.
//   Inner: 0 outside the mask and on its edge, rising to 1 at Distance inside.
//   Outer: 1 inside the mask and on its edge, falling to 0 at Distance outside.
//   Both:  0.5 on the edge, rising to 1 at Distance inside and falling to 0 at Distance outside (signed distance).
// Distance, Smoothing and Threshold hold for the whole image (scalars, evaluated at the image centre when linked): the
// width has to be known per bake, and a per pixel link has no neighbours to measure in image space.

// Input socket indices
enum {
	DISTANCE_INPUT_IMAGE     = 0,
	DISTANCE_INPUT_DISTANCE  = 1,
	DISTANCE_INPUT_SMOOTHING = 2,
	DISTANCE_INPUT_THRESHOLD = 3,
};

// Output socket indices
enum {
	DISTANCE_OUTPUT_DISTANCE = 0,
	DISTANCE_OUTPUT_BEVEL    = 1,
};

// Node button indices (settings)
enum {
	DISTANCE_BUTTON_SIZE = 0, // Button 1 is the state row
	DISTANCE_BUTTON_MODE = 2,
	DISTANCE_BUTTON_WRAP = 3,
};

// Mode setting
enum {
	DISTANCE_MODE_INNER = 0,
	DISTANCE_MODE_OUTER = 1,
	DISTANCE_MODE_BOTH  = 2,
};

#define DISTANCE_FAR 1.0e20 // Squared distance of a pixel without any feature in its line (no edge at all)

typedef struct distance_params {
	i32 mode;
	i32 wrap;
	f32 distance;
	f32 smoothing;
	f32 threshold;
} distance_params_t;

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 distance_node_size(ui_node_t *node) {
	return make_image_node_size(node, DISTANCE_BUTTON_SIZE);
}

// Threshold of the node being captured (params ran first), clamped to 0..1 like the Image
static f32 distance_node_threshold(f32 threshold) {
	return threshold < 0.0f ? 0.0f : threshold > 1.0f ? 1.0f : threshold;
}

// A float as a shader literal that reads back as the same float (parser_material_vec1 keeps six decimals only)
static char *distance_node_literal(f32 v) {
	char buf[40];
	snprintf(buf, sizeof(buf), "%.9g", (double)v);
	for (char *c = buf; *c != '\0'; ++c) {
		if (*c == '.' || *c == 'e') {
			return string_tmp("%s", buf);
		}
	}
	return string_tmp("%s.0", buf);
}

// Captured per pixel: 1 where the Image input (the mask) is at or above the Threshold, else 0 (an R8 capture: the node only
// needs inside or outside, the Threshold is evaluated once for the whole image like the other numbers)
static char *distance_node_capture(ui_node_t *node) {
	distance_params_t *p      = make_image_node_capture_params();
	char              *image  = parser_material_parse_value_input(node->inputs->buffer[DISTANCE_INPUT_IMAGE], false);
	char              *inside = string_tmp("%s_inside", parser_material_store_var_name(node));
	parser_material_write(parser_material_kong, string_tmp("float %s = 0.0;", inside));
	parser_material_write(parser_material_kong, string_tmp("if (clamp(%s, 0.0, 1.0) >= %s) { %s = 1.0; }", image, distance_node_literal(distance_node_threshold(p->threshold)), inside));
	return string_tmp("float4(%s, %s, %s, 1.0)", inside, inside, inside);
}

static void distance_node_params(ui_node_t *node, void *params) {
	distance_params_t *p = params;
	p->mode           = (i32)make_image_node_button(node, DISTANCE_BUTTON_MODE);
	p->wrap           = make_image_node_button(node, DISTANCE_BUTTON_WRAP) != 0.0;
	p->distance       = make_image_node_input_value(node, DISTANCE_INPUT_DISTANCE);
	p->smoothing      = make_image_node_input_value(node, DISTANCE_INPUT_SMOOTHING);
	p->threshold      = make_image_node_input_value(node, DISTANCE_INPUT_THRESHOLD);
}

// Squared distance transform of f over n samples (d[q] = min over p of (q - p)^2 + f[p]): the lower envelope of the parabolas
// rooted at every sample. v, z are scratch (n and n + 1 entries)
static void distance_node_edt_line(const f64 *f, f64 *d, i32 n, i32 *v, f64 *z) {
	i32 k = 0;
	v[0]  = 0;
	z[0]  = -DISTANCE_FAR;
	z[1]  = DISTANCE_FAR;
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
		z[k + 1] = DISTANCE_FAR;
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

typedef struct distance_edt_ctx {
	const u8 *feature; // 1 where the pixel is a feature (distance 0), size * size
	f32      *g;       // Squared distance along the columns, size * size
	f32      *dist;    // Result: the distance in pixels, size * size
	i32       size;
	bool      wrap;
	bool      failed; // A worker could not allocate its lines, only ever set to true
} distance_edt_ctx_t;

// One line of the transform: f has size samples, tripled when wrap (the middle copy is the result, so the distance
// reaches through the border); the result d is read back into out with the stride
static void distance_node_edt_run(const f64 *line, f64 *work, f64 *d, i32 *v, f64 *z, i32 size, bool wrap) {
	i32 n = wrap ? size * 3 : size;
	if (wrap) {
		for (i32 c = 0; c < 3; ++c) {
			memcpy(work + (size_t)c * size, line, (size_t)size * sizeof(f64));
		}
		distance_node_edt_line(work, d, n, v, z);
	}
	else {
		distance_node_edt_line(line, d, n, v, z);
	}
}

// Columns [begin, end): the distance to the nearest feature of the same column (squared)
static void distance_node_edt_columns(void *param, i32 begin, i32 end) {
	distance_edt_ctx_t *c    = param;
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
				line[y] = c->feature[(size_t)y * size + x] != 0 ? 0.0 : DISTANCE_FAR;
			}
			distance_node_edt_run(line, work, d, v, z, size, c->wrap);
			f64 *out = c->wrap ? d + size : d;
			for (i32 y = 0; y < size; ++y) {
				c->g[(size_t)y * size + x] = (f32)(out[y] > DISTANCE_FAR ? DISTANCE_FAR : out[y]);
			}
		}
	}
	else {
		c->failed = true;
	}
	free(line);
	free(work);
	free(d);
	free(v);
	free(z);
}

// Rows [begin, end): the transform of the column distances along the row, then the square root, half a pixel nearer
static void distance_node_edt_rows(void *param, i32 begin, i32 end) {
	distance_edt_ctx_t *c    = param;
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
			distance_node_edt_run(line, work, d, v, z, size, c->wrap);
			f64 *out = c->wrap ? d + size : d;
			for (i32 x = 0; x < size; ++x) {
				// The pixel centres sit half a pixel from the edge between an inside and an outside pixel
				f64 px                         = out[x] >= DISTANCE_FAR ? DISTANCE_FAR : sqrt(out[x]) - 0.5;
				c->dist[(size_t)y * size + x] = (f32)(px < 0.0 ? 0.0 : px);
			}
		}
	}
	else {
		c->failed = true;
	}
	free(line);
	free(work);
	free(d);
	free(v);
	free(z);
}

// Distance in pixels of every pixel to the nearest pixel where feature is set (DISTANCE_FAR when there is none).
// Returns false when memory runs out, dist is not valid then.
static bool distance_node_distance(const u8 *feature, f32 *g, f32 *dist, i32 size, bool wrap) {
	distance_edt_ctx_t c = {.feature = feature, .g = g, .dist = dist, .size = size, .wrap = wrap};
	make_image_node_parallel(size, distance_node_edt_columns, &c);
	make_image_node_parallel(size, distance_node_edt_rows, &c);
	return !c.failed;
}

// Shaped height of t in 0..1: a chamfer at smoothing 0, a quarter circle at 1
static f32 distance_node_profile(f32 t, f32 smoothing) {
	f32 u     = 1.0f - t;
	f32 round = sqrtf(u < 1.0f ? 1.0f - u * u : 0.0f);
	return t + (round - t) * smoothing;
}

typedef struct distance_shape_ctx {
	const f32 *inner; // Distance of the inside pixels in pixels, NULL when not needed
	const f32 *outer; // Distance of the outside pixels in pixels, NULL when not needed
	const u8  *inside;
	f32       *result;
	i32        size;
	i32        mode;
	f32        width_px;
	f32        smoothing;
} distance_shape_ctx_t;

// Rows [begin, end): the height (R) and the plain distance (G) of every pixel
static void distance_node_shape_rows(void *param, i32 begin, i32 end) {
	distance_shape_ctx_t *c    = param;
	i32                size = c->size;
	for (i32 y = begin; y < end; ++y) {
		for (i32 x = 0; x < size; ++x) {
			size_t i      = (size_t)y * size + x;
			bool   in     = c->inside[i] != 0;
			f32    height = 0.0f;
			f32    dist   = 0.0f;
			if (c->mode == DISTANCE_MODE_INNER) {
				f32 t  = in ? fminf(c->inner[i] / c->width_px, 1.0f) : 0.0f;
				dist   = t;
				height = distance_node_profile(t, c->smoothing);
			}
			else if (c->mode == DISTANCE_MODE_OUTER) {
				f32 t  = in ? 0.0f : fminf(c->outer[i] / c->width_px, 1.0f);
				dist   = 1.0f - t;
				height = 1.0f - distance_node_profile(t, c->smoothing);
			}
			else {
				if (in) {
					f32 t  = fminf(c->inner[i] / c->width_px, 1.0f);
					dist   = 0.5f + 0.5f * t;
					height = 0.5f + 0.5f * distance_node_profile(t, c->smoothing);
				}
				else {
					f32 t  = fminf(c->outer[i] / c->width_px, 1.0f);
					dist   = 0.5f - 0.5f * t;
					height = 0.5f - 0.5f * distance_node_profile(t, c->smoothing);
				}
			}
			c->result[i * 4]     = height;
			c->result[i * 4 + 1] = dist;
			c->result[i * 4 + 2] = height;
			c->result[i * 4 + 3] = 1.0f;
		}
	}
}

static i32 distance_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	u8            *pixels = bytes; // R8 capture: 255 inside (Image at or above the Threshold), 0 outside
	distance_params_t p      = *(distance_params_t *)params;
	size_t         n      = (size_t)size * size;
	u8            *inside = malloc(n);
	u8            *edge   = malloc(n);
	size_t         plane_bytes = n * sizeof(f32);
	f32           *g           = make_image_node_plane_alloc(plane_bytes);
	f32           *inner       = p.mode != DISTANCE_MODE_OUTER ? make_image_node_plane_alloc(plane_bytes) : NULL;
	f32           *outer       = p.mode != DISTANCE_MODE_INNER ? make_image_node_plane_alloc(plane_bytes) : NULL;
	if (inside == NULL || edge == NULL || g == NULL || (p.mode != DISTANCE_MODE_OUTER && inner == NULL) || (p.mode != DISTANCE_MODE_INNER && outer == NULL)) {
		free(inside);
		free(edge);
		make_image_node_plane_free(g, plane_bytes);
		make_image_node_plane_free(inner, plane_bytes);
		make_image_node_plane_free(outer, plane_bytes);
		return -1;
	}
	bool ok        = true;
	for (size_t i = 0; i < n; ++i) {
		inside[i] = pixels[i] > 127 ? 1 : 0;
	}
	if (inner != NULL) { // Inside pixels: the nearest outside pixel is the feature
		for (size_t i = 0; i < n; ++i) {
			edge[i] = inside[i] ? 0 : 1;
		}
		ok = distance_node_distance(edge, g, inner, size, p.wrap != 0);
	}
	if (outer != NULL) { // Outside pixels: the nearest inside pixel is the feature
		ok = ok && distance_node_distance(inside, g, outer, size, p.wrap != 0);
	}
	if (!ok) {
		free(inside);
		free(edge);
		make_image_node_plane_free(g, plane_bytes);
		make_image_node_plane_free(inner, plane_bytes);
		make_image_node_plane_free(outer, plane_bytes);
		return -1;
	}
	f32 width_px = p.distance * (f32)size;
	width_px     = width_px < 1.0f ? 1.0f : width_px;
	f32 smooth   = p.smoothing < 0.0f ? 0.0f : p.smoothing > 1.0f ? 1.0f : p.smoothing;
	distance_shape_ctx_t sc = {.inner = inner, .outer = outer, .inside = inside, .result = result, .size = size, .mode = p.mode, .width_px = width_px, .smoothing = smooth};
	make_image_node_parallel(size, distance_node_shape_rows, &sc);
	free(inside);
	free(edge);
	make_image_node_plane_free(g, plane_bytes);
	make_image_node_plane_free(inner, plane_bytes);
	make_image_node_plane_free(outer, plane_bytes);
	return 0;
}

static image_node_type_t distance_node_image_type = {
    .type          = "TEX_DISTANCE",
    .result_format = GPU_TEXTURE_FORMAT_RGBA128, // RGBA128 on purpose: the result is a height, a Bump node reads its slope, half floats would band at 4096
    .params_size   = sizeof(distance_params_t),
    .size          = distance_node_size,
    .capture       = distance_node_capture,
    .capture_format = IMAGE_CAPTURE_R8,
    .smooth_sample = true,
    .params        = distance_node_params,
    .process       = distance_node_process,
};

char *distance_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	char *sample = make_image_node_sample(node);
	if (array_index_of(node->outputs, socket) == DISTANCE_OUTPUT_DISTANCE) {
		return string_tmp("%s.g", sample);
	}
	return string_tmp("%s.r", sample); // DISTANCE_OUTPUT_BEVEL
}

char *distance_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return parser_material_to_vec3(distance_node_value(node, socket));
}

void distance_node_init() {
	u32 value_color = 0xffa1a1a1;

	ui_node_t *distance_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Distance"),
	                .type   = "TEX_DISTANCE",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Distance"), "VALUE", value_color, f32_array_create_x(0.05), 0.0, 0.5, 1000),
	                        make_image_node_socket(_tr("Smoothing"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Threshold"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                    },
	                    4),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Distance"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Bevel"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                    },
	                    2),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Size"), "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS),
	                        make_image_node_state_button(),
	                        make_image_node_setting(_tr("Mode"), "ENUM", 0, "Inner\nOuter\nBoth"),
	                        make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                    },
	                    4),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, distance_node_def);
	any_map_set(parser_material_node_values, "TEX_DISTANCE", distance_node_value);
	any_map_set(parser_material_node_vectors, "TEX_DISTANCE", distance_node_vector);
	make_image_node_sections(distance_node_def, NULL, 0, 1); // General: the settings and Image
	make_image_node_register(&distance_node_image_type);
}
