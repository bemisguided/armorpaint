#include "../global.h"

// Advanced Blur (Image menu): blurs the Image input. An image node (render/make_image_node.c): Image and Mask are
// evaluated in image space and captured in 32 bit float (R and B = Image, G = Mask, so a red/blue swap on readback
// does not matter), the blur is computed on the cpu into one RGBA64 texture per node, R = the blurred image.
// The texture is sampled like an Image Texture at tex_coord, repeating (Tile UV places it on the uv). The image repeats, so
// the blur wraps around the edges.
//
// Modes: Gaussian and Box blur along the Angle direction by Radius times Aspect and then across it by Radius
// (Aspect 1 is a round blur, higher stretches it along Angle: anisotropic blur); Directional blurs only along
// Angle by Radius (a line, like a motion blur). Radius is a fraction of Size.
// Radius, Angle and Aspect hold for the whole image (scalar, evaluated at the image centre when linked).
// Mask multiplies the radius per pixel (Non-Uniform Blur): each pass takes the radius at the pixel it writes, so a
// varying radius is an approximation, exact only where the mask is constant. Taps are never more than one pixel apart:
// wide boxes are two passes (a dense box and a comb of boxes), wide gaussians three boxes, so large radii do not alias.
// The result is kept in 32 bit float (a half float steps by 1/2048 near 1, which a Bump node shows on a soft gradient).

// Input socket indices
enum {
	ADVANCED_BLUR_INPUT_IMAGE  = 0,
	ADVANCED_BLUR_INPUT_RADIUS = 1,
	ADVANCED_BLUR_INPUT_ANGLE  = 2,
	ADVANCED_BLUR_INPUT_ASPECT = 3,
	ADVANCED_BLUR_INPUT_MASK   = 4,
};

// Node button indices (settings)
enum {
	ADVANCED_BLUR_BUTTON_SIZE = 0, // Button 1 is the state row
	ADVANCED_BLUR_BUTTON_MODE = 2,
};

enum {
	ADVANCED_BLUR_MODE_GAUSSIAN    = 0,
	ADVANCED_BLUR_MODE_BOX         = 1,
	ADVANCED_BLUR_MODE_DIRECTIONAL = 2,
};

#define ADVANCED_BLUR_MAX_TAPS 65
#define ADVANCED_BLUR_BOX_DENSE 64   // A box up to this width (pixels) is one dense pass, wider ones two
#define ADVANCED_BLUR_GAUSS_DENSE 32 // A gaussian up to this radius (pixels) is one dense pass, larger ones three boxes

typedef struct advanced_blur_params {
	i32 mode;
	f32 radius; // Fraction of Size
	f32 angle;  // Radians
	f32 aspect;
} advanced_blur_params_t;

static f32 advanced_blur_node_button(ui_node_t *node, i32 i) {
	return node->buttons->buffer[i]->default_value->buffer[0];
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 advanced_blur_node_size(ui_node_t *node) {
	i32 i = (i32)advanced_blur_node_button(node, ADVANCED_BLUR_BUTTON_SIZE);
	return 512 << (i < 0 ? 0 : i > 3 ? 3 : i);
}

// Captured per pixel: Image in R and B, Mask in G
static char *advanced_blur_node_capture(ui_node_t *node) {
	char *image = parser_material_parse_value_input(node->inputs->buffer[ADVANCED_BLUR_INPUT_IMAGE], false);
	char *mask  = parser_material_parse_value_input(node->inputs->buffer[ADVANCED_BLUR_INPUT_MASK], false);
	char *name  = parser_material_store_var_name(node);
	parser_material_write(parser_material_kong, string_tmp("float %s_image = %s;", name, image));
	parser_material_write(parser_material_kong, string_tmp("float %s_mask = %s;", name, mask));
	return string_tmp("float4(%s_image, %s_mask, %s_image, 1.0)", name, name, name);
}

static void advanced_blur_node_params(ui_node_t *node, void *params) {
	advanced_blur_params_t *p = params;
	p->mode                   = (i32)advanced_blur_node_button(node, ADVANCED_BLUR_BUTTON_MODE);
	p->radius                 = make_image_node_input_value(node, ADVANCED_BLUR_INPUT_RADIUS);
	p->angle                  = make_image_node_input_value(node, ADVANCED_BLUR_INPUT_ANGLE) * (f32)(3.14159265358979 / 180.0);
	p->aspect                 = make_image_node_input_value(node, ADVANCED_BLUR_INPUT_ASPECT);
}

// Bilinear sample of a single channel image at pixel position (x, y), wrapping
static f32 advanced_blur_node_sample(f32 *img, i32 size, f32 x, f32 y) {
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

// Value of a box of the given width (pixels) along (dx, dy) at (x, y). Up to ADVANCED_BLUR_BOX_DENSE pixels wide it is one
// pass of taps one pixel apart (stage 0; stage 1 copies). Wider boxes take two passes of about sqrt(width) taps: stage 0 a
// dense box of width / k, stage 1 k taps width / k apart, which together are exactly the wide box. A pass never skips pixels,
// so there is no comb or stair pattern at large radii.
static f32 advanced_blur_node_box(f32 *src, i32 size, i32 x, i32 y, f32 dx, f32 dy, f32 width, i32 stage) {
	size_t i = (size_t)y * size + x;
	if (width < 1.0f) {
		return src[i];
	}
	i32 n;
	f32 s;
	if (width <= (f32)ADVANCED_BLUR_BOX_DENSE) {
		if (stage == 1) {
			return src[i];
		}
		n = (i32)ceilf(width);
		s = width / (f32)n;
	}
	else {
		i32 k  = (i32)ceilf(sqrtf(width));
		f32 w1 = width / (f32)k;
		if (stage == 0) {
			n = (i32)(w1 + 0.5f);
			n = n < 1 ? 1 : n;
			s = w1 / (f32)n;
		}
		else {
			n = k;
			s = w1;
		}
	}
	f32 sum = 0.0f;
	for (i32 j = 0; j < n; ++j) {
		f32 t = ((f32)j - (f32)(n - 1) * 0.5f) * s;
		sum += advanced_blur_node_sample(src, size, (f32)x + dx * t, (f32)y + dy * t);
	}
	return sum / (f32)n;
}

// Gaussian of radius r (3 sigma) at (x, y). Up to ADVANCED_BLUR_GAUSS_DENSE pixels: one dense pass (stage 0). Larger: three
// boxes of width 2r / 3 (their sum is a Gaussian within a few percent, variance 3 * width^2 / 12 = sigma^2), each box
// two stages, stages 1 to 6.
static f32 advanced_blur_node_gauss(f32 *src, i32 size, i32 x, i32 y, f32 dx, f32 dy, f32 r, i32 stage) {
	size_t i = (size_t)y * size + x;
	if (r < 0.5f) {
		return src[i];
	}
	if (r > (f32)ADVANCED_BLUR_GAUSS_DENSE) {
		if (stage == 0) {
			return src[i];
		}
		return advanced_blur_node_box(src, size, x, y, dx, dy, 2.0f * r / 3.0f, (stage - 1) & 1);
	}
	if (stage != 0) {
		return src[i];
	}
	i32 n     = (i32)(2.0f * r) + 1;
	n         = n > ADVANCED_BLUR_MAX_TAPS ? ADVANCED_BLUR_MAX_TAPS : n;
	n         = n < 3 ? 3 : n;
	f32 sum   = 0.0f;
	f32 wsum  = 0.0f;
	f32 sigma = r / 3.0f;
	for (i32 k = 0; k < n; ++k) {
		f32 t = -r + 2.0f * r * (f32)k / (f32)(n - 1);
		f32 w = expf(-t * t / (2.0f * sigma * sigma));
		sum += w * advanced_blur_node_sample(src, size, (f32)x + dx * t, (f32)y + dy * t);
		wsum += w;
	}
	return sum / wsum;
}

// One blur along (dx, dy) of radius pixels * the mask of each pixel (G of px), gaussian or box (box width 2 * radius), from
// *cur into *cur again (the buffers are swapped between the stages)
static void advanced_blur_node_run(f32 **cur, f32 **nxt, f32 *px, i32 size, f32 dx, f32 dy, f32 radius, bool gaussian) {
	i32 stages = gaussian ? 7 : 2;
	for (i32 st = 0; st < stages; ++st) {
		f32 *src = *cur;
		f32 *dst = *nxt;
		for (i32 y = 0; y < size; ++y) {
			for (i32 x = 0; x < size; ++x) {
				size_t i = (size_t)y * size + x;
				f32    m = px[i * 4 + 1];
				f32    r = radius * (m < 0.0f ? 0.0f : m);
				dst[i]   = gaussian ? advanced_blur_node_gauss(src, size, x, y, dx, dy, r, st) : advanced_blur_node_box(src, size, x, y, dx, dy, 2.0f * r, st);
			}
		}
		*cur = dst;
		*nxt = src;
	}
}

// Blurred image from the captured float pixels; returns 0, or -1 when memory runs out
static i32 advanced_blur_node_process(u8 *pixels, i32 size, void *params, f32 *result) {
	advanced_blur_params_t p  = *(advanced_blur_params_t *)params;
	f32                   *px = (f32 *)pixels;
	size_t                 n  = (size_t)size * size;
	f32                   *a  = malloc(n * sizeof(f32));
	f32                   *b  = malloc(n * sizeof(f32));
	if (a == NULL || b == NULL) {
		free(a);
		free(b);
		return -1;
	}
	for (size_t i = 0; i < n; ++i) {
		a[i] = px[i * 4];
	}
	f32  radius   = (p.radius < 0.0f ? 0.0f : p.radius) * (f32)size;
	f32  aspect   = p.aspect < 1.0f ? 1.0f : p.aspect;
	f32  ux       = cosf(p.angle);
	f32  uy       = sinf(p.angle);
	bool gaussian = p.mode == ADVANCED_BLUR_MODE_GAUSSIAN;
	f32 *cur      = a;
	f32 *nxt      = b;
	if (p.mode == ADVANCED_BLUR_MODE_DIRECTIONAL) {
		advanced_blur_node_run(&cur, &nxt, px, size, ux, uy, radius, false);
	}
	else {
		advanced_blur_node_run(&cur, &nxt, px, size, ux, uy, radius * aspect, gaussian);
		advanced_blur_node_run(&cur, &nxt, px, size, -uy, ux, radius, gaussian);
	}
	for (size_t i = 0; i < n; ++i) {
		f32 v             = cur[i] < 0.0f ? 0.0f : cur[i] > 1.0f ? 1.0f : cur[i];
		result[i * 4]     = v;
		result[i * 4 + 1] = v;
		result[i * 4 + 2] = v;
		result[i * 4 + 3] = 1.0f;
	}
	free(a);
	free(b);
	return 0;
}

static image_node_type_t advanced_blur_node_image_type = {
    .type          = "TEX_ADVANCED_BLUR",
    .result_format = GPU_TEXTURE_FORMAT_RGBA128,
    .params_size   = sizeof(advanced_blur_params_t),
    .size          = advanced_blur_node_size,
    .capture       = advanced_blur_node_capture,
    .capture_float = true,
    .smooth_sample = true,
    .params        = advanced_blur_node_params,
    .process       = advanced_blur_node_process,
};

char *advanced_blur_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

char *advanced_blur_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return parser_material_to_vec3(advanced_blur_node_value(node, socket));
}

static ui_node_socket_t *advanced_blur_node_socket(char *name, char *type, u32 color, f32_array_t *default_value, f32 max, f32 precision) {
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

static ui_node_button_t *advanced_blur_node_setting(char *name, char *type, f32 default_value, char *data) {
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

void advanced_blur_node_init() {
	u32 value_color  = 0xffa1a1a1;

	ui_node_t *advanced_blur_node_def =
	    ALLOC_INIT(ui_node_t, {.id     = 0,
	                           .name   = _tr("Advanced Blur"),
	                           .type   = "TEX_ADVANCED_BLUR",
	                           .x      = 0,
	                           .y      = 0,
	                           .color  = NODES_MATERIAL_IMAGE_COLOR,
	                           .inputs = any_array_create_from_raw(
	                               (void *[]){
	                                   advanced_blur_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 1.0, 100),
	                                   advanced_blur_node_socket(_tr("Radius"), "VALUE", value_color, f32_array_create_x(0.01), 0.25, 1000),
	                                   advanced_blur_node_socket(_tr("Angle"), "VALUE", value_color, f32_array_create_x(0.0), 360.0, 10),
	                                   advanced_blur_node_socket(_tr("Aspect"), "VALUE", value_color, f32_array_create_x(1.0), 10.0, 100),
	                                   advanced_blur_node_socket(_tr("Mask"), "VALUE", value_color, f32_array_create_x(1.0), 1.0, 100),
	                               },
	                               5),
	                           .outputs = any_array_create_from_raw(
	                               (void *[]){
	                                   advanced_blur_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 1.0, 100),
	                               },
	                               1),
	                           .buttons = any_array_create_from_raw(
	                               (void *[]){
	                                   advanced_blur_node_setting(_tr("Size"), "ENUM", 1, "512\n1024\n2048\n4096"),
	                                   make_image_node_state_button(),
	                                   advanced_blur_node_setting(_tr("Mode"), "ENUM", 0, "Gaussian\nBox\nDirectional"),
	                               },
	                               3),
	                           .width = 0,
	                           .flags = 0});

	any_array_push(nodes_material_filter, advanced_blur_node_def);
	any_map_set(parser_material_node_values, "TEX_ADVANCED_BLUR", advanced_blur_node_value);
	any_map_set(parser_material_node_vectors, "TEX_ADVANCED_BLUR", advanced_blur_node_vector);
	make_image_node_register(&advanced_blur_node_image_type);
}
