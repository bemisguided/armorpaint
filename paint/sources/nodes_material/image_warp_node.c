#include "../global.h"

// Warp (Filter menu): moves the pixels of the Image input along a direction, optionally smearing them along the way. An image
// node (render/make_image_node.c): Image and Map are evaluated in image space and captured together in 32 bit float (R and B =
// Image, G = Map, so a red/blue swap on readback does not matter), the warp is computed on the cpu into one RGBA128 texture
// per node, R = the warped image. The texture is sampled like an Image Texture at tex_coord, repeating (Tile UV places it on
// the uv). With Wrap Edges on (the default) the image repeats, so the warp wraps around the edges; off, samples outside the
// image (the warp tap and the Map reads) clamp to the nearest edge pixel. This is Directional Warp and Slope Blur in one node
// (the user merged them, ticket ARM-toozq5i5mf). Not related to the Warp node of the Color menu (DIRECT_WARP, warp_node.c), which
// distorts a vector.
//
// Every output pixel reads the image at p - d (the content moves along d, a negative Strength moves it the other way):
//   Direction Angle:        d = (cos Angle, sin Angle) * Strength * Size * Map(p), Map clamped to 0..1.
//   Direction Slope of Map: g = central difference of Map at p, in Map units per image width (per pixel * Size);
//                           d = g / |g| * min(|g|, 1) * Strength * Size. So a Map that rises by 1 across the image moves the
//                           content by exactly Strength of the image, steeper slopes are clamped to that, flat areas do not move.
// Strength is a fraction of the image (the whole image is 1), Angle is in degrees (0 along x, y down like the uv).
// Blur smears along the path: 0 is a pure warp (one tap at p - d); above 0 the output averages taps on the segment from
// p - d * (1 - Blur) to p - d, so Blur 1 smears over the whole displacement (Slope Blur, Directional Blur along a map).
// Taps are one pixel apart or closer, up to WARP_MAX_TAPS; a smear longer than that aliases (spacing above one pixel).
// Strength, Angle and Blur hold for the whole image (scalar, evaluated at the image centre when linked); Map is per pixel.

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
	IMAGE_WARP_BUTTON_SIZE      = 0, // Button 1 is the state row
	IMAGE_WARP_BUTTON_DIRECTION = 2,
	IMAGE_WARP_BUTTON_WRAP      = 3, // Wrap Edges
};

enum {
	IMAGE_WARP_DIRECTION_ANGLE = 0,
	IMAGE_WARP_DIRECTION_SLOPE = 1,
};

#define IMAGE_WARP_MAX_TAPS 129

typedef struct image_warp_params {
	i32 direction;
	f32 strength; // Fraction of Size
	f32 angle;    // Radians
	f32 blur;     // 0 to 1
	i32 wrap;     // Wrap Edges: samples outside the image wrap around (1) or clamp to the edge (0); part of the node signature
} image_warp_params_t;

static f32 image_warp_node_button(ui_node_t *node, i32 i) {
	return node->buttons->buffer[i]->default_value->buffer[0];
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 image_warp_node_size(ui_node_t *node) {
	i32 i = (i32)image_warp_node_button(node, IMAGE_WARP_BUTTON_SIZE);
	return 512 << (i < 0 ? 0 : i > 3 ? 3 : i);
}

// Captured per pixel: Image in R and B, Map in G
static char *image_warp_node_capture(ui_node_t *node) {
	char *image = parser_material_parse_value_input(node->inputs->buffer[IMAGE_WARP_INPUT_IMAGE], false);
	char *map   = parser_material_parse_value_input(node->inputs->buffer[IMAGE_WARP_INPUT_MAP], false);
	char *name  = parser_material_store_var_name(node);
	parser_material_write(parser_material_kong, string_tmp("float %s_image = %s;", name, image));
	parser_material_write(parser_material_kong, string_tmp("float %s_map = %s;", name, map));
	return string_tmp("float4(%s_image, %s_map, %s_image, 1.0)", name, name, name);
}

static void image_warp_node_params(ui_node_t *node, void *params) {
	image_warp_params_t *p = params;
	p->direction           = (i32)image_warp_node_button(node, IMAGE_WARP_BUTTON_DIRECTION);
	p->strength            = make_image_node_input_value(node, IMAGE_WARP_INPUT_STRENGTH);
	p->angle               = make_image_node_input_value(node, IMAGE_WARP_INPUT_ANGLE) * (f32)(3.14159265358979 / 180.0);
	p->blur                = make_image_node_input_value(node, IMAGE_WARP_INPUT_BLUR);
	p->wrap                = image_warp_node_button(node, IMAGE_WARP_BUTTON_WRAP) != 0.0f ? 1 : 0;
}

typedef f32 (*image_warp_sample_t)(f32 *img, i32 size, f32 x, f32 y);

// Bilinear sample of a single channel image at pixel position (x, y), wrapping
static f32 image_warp_node_sample_wrap(f32 *img, i32 size, f32 x, f32 y) {
	f32 fx = floorf(x);
	f32 fy = floorf(y);
	f32 tx = x - fx;
	f32 ty = y - fy;
	i32 x0 = ((i32)fx % size + size) % size;
	i32 y0 = ((i32)fy % size + size) % size;
	i32 x1 = (x0 + 1) % size;
	i32 y1 = (y0 + 1) % size;
	f32 a  = img[(size_t)y0 * size + x0] * (1.0f - tx) + img[(size_t)y0 * size + x1] * tx;
	f32 b  = img[(size_t)y1 * size + x0] * (1.0f - tx) + img[(size_t)y1 * size + x1] * tx;
	return a * (1.0f - ty) + b * ty;
}

// Bilinear sample at pixel position (x, y), positions outside the image clamp to the nearest edge pixel
static f32 image_warp_node_sample_clamp(f32 *img, i32 size, f32 x, f32 y) {
	f32 fx = floorf(x);
	f32 fy = floorf(y);
	f32 tx = x - fx;
	f32 ty = y - fy;
	f32 hi = (f32)size;
	fx     = fx < -1.0f ? -1.0f : fx > hi ? hi : fx; // Keeps the cast in range
	fy     = fy < -1.0f ? -1.0f : fy > hi ? hi : fy;
	i32 mx = size - 1;
	i32 x0 = (i32)fx;
	i32 y0 = (i32)fy;
	i32 x1 = x0 + 1;
	i32 y1 = y0 + 1;
	x0     = x0 < 0 ? 0 : x0 > mx ? mx : x0;
	y0     = y0 < 0 ? 0 : y0 > mx ? mx : y0;
	x1     = x1 < 0 ? 0 : x1 > mx ? mx : x1;
	y1     = y1 < 0 ? 0 : y1 > mx ? mx : y1;
	f32 a  = img[(size_t)y0 * size + x0] * (1.0f - tx) + img[(size_t)y0 * size + x1] * tx;
	f32 b  = img[(size_t)y1 * size + x0] * (1.0f - tx) + img[(size_t)y1 * size + x1] * tx;
	return a * (1.0f - ty) + b * ty;
}

typedef struct image_warp_ctx {
	f32                *px; // Captured float pixels (R and B = Image, G = Map)
	f32                *img;
	f32                *dst;
	i32                 size;
	image_warp_params_t p;
	f32                 ux; // Angle direction
	f32                 uy;
	image_warp_sample_t sample; // Wrap or clamp, picked once
} image_warp_ctx_t;

// Map value at a pixel, wrapping or clamping the position per Wrap Edges, the value clamped to 0..1
static f32 image_warp_node_map(image_warp_ctx_t *c, i32 x, i32 y) {
	i32 xx;
	i32 yy;
	if (c->p.wrap) {
		xx = (x % c->size + c->size) % c->size;
		yy = (y % c->size + c->size) % c->size;
	}
	else {
		xx = x < 0 ? 0 : x >= c->size ? c->size - 1 : x;
		yy = y < 0 ? 0 : y >= c->size ? c->size - 1 : y;
	}
	f32 m  = c->px[((size_t)yy * c->size + xx) * 4 + 1];
	return m < 0.0f ? 0.0f : m > 1.0f ? 1.0f : m;
}

// Rows [begin, end) of the warp
static void image_warp_node_rows(void *param, i32 begin, i32 end) {
	image_warp_ctx_t *c    = param;
	f32               size = (f32)c->size;
	for (i32 y = begin; y < end; ++y) {
		for (i32 x = 0; x < c->size; ++x) {
			f32 dx;
			f32 dy;
			if (c->p.direction == IMAGE_WARP_DIRECTION_SLOPE) {
				f32 gx = (image_warp_node_map(c, x + 1, y) - image_warp_node_map(c, x - 1, y)) * 0.5f * size;
				f32 gy = (image_warp_node_map(c, x, y + 1) - image_warp_node_map(c, x, y - 1)) * 0.5f * size;
				f32 g  = sqrtf(gx * gx + gy * gy);
				if (g > 1e-6f) {
					f32 k = (g < 1.0f ? g : 1.0f) / g * c->p.strength * size;
					dx    = gx * k;
					dy    = gy * k;
				}
				else {
					dx = 0.0f;
					dy = 0.0f;
				}
			}
			else {
				f32 k = image_warp_node_map(c, x, y) * c->p.strength * size;
				dx    = c->ux * k;
				dy    = c->uy * k;
			}
			f32 blur = c->p.blur < 0.0f ? 0.0f : c->p.blur > 1.0f ? 1.0f : c->p.blur;
			f32 len  = blur * sqrtf(dx * dx + dy * dy);
			f32 v;
			if (len < 0.5f) {
				v = c->sample(c->img, c->size, (f32)x - dx, (f32)y - dy);
			}
			else {
				i32 n = (i32)ceilf(len) + 1;
				n     = n > IMAGE_WARP_MAX_TAPS ? IMAGE_WARP_MAX_TAPS : n;
				f32 sum = 0.0f;
				for (i32 j = 0; j < n; ++j) {
					f32 t = (f32)j / (f32)(n - 1); // 0 to 1 along the segment from p - d * (1 - blur) to p - d
					f32 s = 1.0f - blur + blur * t;
					sum += c->sample(c->img, c->size, (f32)x - dx * s, (f32)y - dy * s);
				}
				v = sum / (f32)n;
			}
			c->dst[(size_t)y * c->size + x] = v;
		}
	}
}

// Warped image from the captured float pixels; returns 0, or -1 when memory runs out
static i32 image_warp_node_process(u8 *pixels, i32 size, void *params, f32 *result) {
	size_t n   = (size_t)size * size;
	f32   *img = malloc(n * sizeof(f32));
	f32   *dst = malloc(n * sizeof(f32));
	if (img == NULL || dst == NULL) {
		free(img);
		free(dst);
		return -1;
	}
	f32 *px = (f32 *)pixels;
	for (size_t i = 0; i < n; ++i) {
		img[i] = px[i * 4];
	}
	image_warp_ctx_t c = {.px = px, .img = img, .dst = dst, .size = size, .p = *(image_warp_params_t *)params};
	c.ux               = cosf(c.p.angle);
	c.uy               = sinf(c.p.angle);
	c.sample           = c.p.wrap ? image_warp_node_sample_wrap : image_warp_node_sample_clamp;
	make_image_node_parallel(size, image_warp_node_rows, &c);
	for (size_t i = 0; i < n; ++i) {
		f32 v             = dst[i] < 0.0f ? 0.0f : dst[i] > 1.0f ? 1.0f : dst[i];
		result[i * 4]     = v;
		result[i * 4 + 1] = v;
		result[i * 4 + 2] = v;
		result[i * 4 + 3] = 1.0f;
	}
	free(img);
	free(dst);
	return 0;
}

static image_node_type_t image_warp_node_image_type = {
    .type          = "TEX_IMAGE_WARP",
    .result_format = GPU_TEXTURE_FORMAT_RGBA128,
    .params_size   = sizeof(image_warp_params_t),
    .size          = image_warp_node_size,
    .capture       = image_warp_node_capture,
    .capture_float = true,
    .smooth_sample = true,
    .params        = image_warp_node_params,
    .process       = image_warp_node_process,
};

char *image_warp_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

char *image_warp_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return parser_material_to_vec3(image_warp_node_value(node, socket));
}

static ui_node_socket_t *image_warp_node_socket(char *name, char *type, u32 color, f32_array_t *default_value, f32 min, f32 max, f32 precision) {
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

static ui_node_button_t *image_warp_node_setting(char *name, char *type, f32 default_value, char *data) {
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

void image_warp_node_init() {
	u32 value_color = 0xffa1a1a1;

	ui_node_t *image_warp_node_def =
	    ALLOC_INIT(ui_node_t, {.id     = 0,
	                           .name   = _tr("Warp"),
	                           .type   = "TEX_IMAGE_WARP",
	                           .x      = 0,
	                           .y      = 0,
	                           .color  = NODES_MATERIAL_IMAGE_COLOR,
	                           .inputs = any_array_create_from_raw(
	                               (void *[]){
	                                   image_warp_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                                   image_warp_node_socket(_tr("Map"), "VALUE", value_color, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                                   image_warp_node_socket(_tr("Strength"), "VALUE", value_color, f32_array_create_x(0.05), -0.5, 0.5, 1000),
	                                   image_warp_node_socket(_tr("Angle"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 360.0, 10),
	                                   image_warp_node_socket(_tr("Blur"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                               },
	                               5),
	                           .outputs = any_array_create_from_raw(
	                               (void *[]){
	                                   image_warp_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                               },
	                               1),
	                           .buttons = any_array_create_from_raw(
	                               (void *[]){
	                                   image_warp_node_setting(_tr("Size"), "ENUM", 1, "512\n1024\n2048\n4096"),
	                                   make_image_node_state_button(),
	                                   image_warp_node_setting(_tr("Direction"), "ENUM", 0, "Angle\nSlope of Map"),
	                                   image_warp_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                               },
	                               4),
	                           .width = 0,
	                           .flags = 0});

	any_array_push(nodes_material_filter, image_warp_node_def);
	any_map_set(parser_material_node_values, "TEX_IMAGE_WARP", image_warp_node_value);
	any_map_set(parser_material_node_vectors, "TEX_IMAGE_WARP", image_warp_node_vector);
	make_image_node_register(&image_warp_node_image_type);
}
