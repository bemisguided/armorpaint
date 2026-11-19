#include "../global.h"

// Transform (Filter menu): moves, rotates, scales and flips an image in image space. An image node
// (render/make_image_node.c): the Image input is captured at Size (32 bit float, colour kept) and the result is
// computed on the cpu into one RGBA128 texture per node. It never touches the uv, Tile UV places an image on the mesh.
//
// Everything is about the Pivot: flip, then scale, then rotation, then Offset. Output pixel q reads the source at
// p = pivot + inverse(R * S * F) * (q - pivot - offset). The angle is applied like Tile UV applies it to its
// coordinate (a positive Rotation turns the image counterclockwise in the editor view).
// Cubic is Catmull-Rom, not Tile UV's B-spline: it passes through the texels, so the default settings give back
// the input pixel for pixel. No mipmaps: a source shrunk by more than about 2x aliases.

// Input socket indices
enum {
	TRANSFORM_INPUT_IMAGE    = 0,
	TRANSFORM_INPUT_OFFSET_X = 1,
	TRANSFORM_INPUT_OFFSET_Y = 2,
	TRANSFORM_INPUT_ROTATION = 3,
	TRANSFORM_INPUT_SCALE_X  = 4,
	TRANSFORM_INPUT_SCALE_Y  = 5,
	TRANSFORM_INPUT_PIVOT_X  = 6,
	TRANSFORM_INPUT_PIVOT_Y  = 7,
};

// Node button indices (settings)
enum {
	TRANSFORM_BUTTON_SIZE   = 0, // The state row is button 1
	TRANSFORM_BUTTON_TILING = 2,
	TRANSFORM_BUTTON_FILTER = 3,
	TRANSFORM_BUTTON_FLIP_X = 4,
	TRANSFORM_BUTTON_FLIP_Y = 5,
};

enum {
	TRANSFORM_TILING_TILE,
	TRANSFORM_TILING_MIRROR,
	TRANSFORM_TILING_CLAMP,
	TRANSFORM_TILING_NONE,
};

enum {
	TRANSFORM_FILTER_LINEAR,
	TRANSFORM_FILTER_CUBIC,
	TRANSFORM_FILTER_NEAREST,
};

typedef struct transform_params {
	f32 offset_x;
	f32 offset_y;
	f32 rotation;
	f32 scale_x;
	f32 scale_y;
	f32 pivot_x;
	f32 pivot_y;
	i32 tiling;
	i32 filter;
	i32 flip_x;
	i32 flip_y;
} transform_params_t;

typedef struct transform_ctx {
	f32                *pixels;
	f32                *result;
	i32                 size;
	transform_params_t *p;
} transform_ctx_t;

static f32 transform_node_button(ui_node_t *node, i32 i) {
	return node->buttons->buffer[i]->default_value->buffer[0];
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 transform_node_size(ui_node_t *node) {
	i32 i = (i32)transform_node_button(node, TRANSFORM_BUTTON_SIZE);
	return 512 << (i < 0 ? 0 : i > 3 ? 3 : i);
}

// Captured per pixel: the Image input
static char *transform_node_capture(ui_node_t *node) {
	char *image = parser_material_parse_vector_input(node->inputs->buffer[TRANSFORM_INPUT_IMAGE]);
	return string_tmp("float4(%s, 1.0)", image);
}

static void transform_node_params(ui_node_t *node, void *params) {
	transform_params_t *p = params;
	p->offset_x           = make_image_node_input_value(node, TRANSFORM_INPUT_OFFSET_X);
	p->offset_y           = make_image_node_input_value(node, TRANSFORM_INPUT_OFFSET_Y);
	p->rotation           = make_image_node_input_value(node, TRANSFORM_INPUT_ROTATION);
	p->scale_x            = make_image_node_input_value(node, TRANSFORM_INPUT_SCALE_X);
	p->scale_y            = make_image_node_input_value(node, TRANSFORM_INPUT_SCALE_Y);
	p->pivot_x            = make_image_node_input_value(node, TRANSFORM_INPUT_PIVOT_X);
	p->pivot_y            = make_image_node_input_value(node, TRANSFORM_INPUT_PIVOT_Y);
	p->tiling             = (i32)transform_node_button(node, TRANSFORM_BUTTON_TILING);
	p->filter             = (i32)transform_node_button(node, TRANSFORM_BUTTON_FILTER);
	p->flip_x             = transform_node_button(node, TRANSFORM_BUTTON_FLIP_X) != 0.0f;
	p->flip_y             = transform_node_button(node, TRANSFORM_BUTTON_FLIP_Y) != 0.0f;
}

// Texel index after the tiling mode, -1 outside the source (None)
static i32 transform_node_wrap(i32 i, i32 size, i32 tiling) {
	if (i >= 0 && i < size) {
		return i;
	}
	switch (tiling) {
	case TRANSFORM_TILING_TILE:
		i %= size;
		return i < 0 ? i + size : i;
	case TRANSFORM_TILING_MIRROR:
		i %= size * 2;
		i = i < 0 ? i + size * 2 : i;
		return i < size ? i : size * 2 - 1 - i;
	case TRANSFORM_TILING_CLAMP:
		return i < 0 ? 0 : size - 1;
	}
	return -1;
}

static void transform_node_fetch(f32 *pixels, i32 size, i32 x, i32 y, i32 tiling, f32 *out) {
	x = transform_node_wrap(x, size, tiling);
	y = transform_node_wrap(y, size, tiling);
	if (x < 0 || y < 0) {
		out[0] = out[1] = out[2] = out[3] = 0.0f;
		return;
	}
	f32 *s = pixels + ((size_t)y * size + x) * 4;
	out[0] = s[0];
	out[1] = s[1];
	out[2] = s[2];
	out[3] = s[3];
}

// Catmull-Rom weights for the four texels around a fraction t
static void transform_node_cubic_weights(f32 t, f32 *w) {
	f32 t2 = t * t;
	f32 t3 = t2 * t;
	w[0]   = 0.5f * (-t3 + 2.0f * t2 - t);
	w[1]   = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
	w[2]   = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
	w[3]   = 0.5f * (t3 - t2);
}

// Source read at (u, v), image fractions
static void transform_node_sample(f32 *pixels, i32 size, f32 u, f32 v, i32 filter, i32 tiling, f32 *out) {
	f32 fx = u * (f32)size;
	f32 fy = v * (f32)size;
	if (filter == TRANSFORM_FILTER_NEAREST) {
		transform_node_fetch(pixels, size, (i32)floorf(fx), (i32)floorf(fy), tiling, out);
		return;
	}
	fx -= 0.5f;
	fy -= 0.5f;
	f32 flx = floorf(fx);
	f32 fly = floorf(fy);
	f32 tx  = fx - flx;
	f32 ty  = fy - fly;
	// Float error must not blur a read that lands on a texel centre
	tx      = tx < 1e-4f ? 0.0f : tx > 1.0f - 1e-4f ? 1.0f : tx;
	ty      = ty < 1e-4f ? 0.0f : ty > 1.0f - 1e-4f ? 1.0f : ty;
	i32 ix  = (i32)flx;
	i32 iy  = (i32)fly;
	f32 wx[4];
	f32 wy[4];
	i32 n0;
	if (filter == TRANSFORM_FILTER_LINEAR) {
		wx[0] = 1.0f - tx;
		wx[1] = tx;
		wy[0] = 1.0f - ty;
		wy[1] = ty;
		n0    = 0;
	}
	else {
		transform_node_cubic_weights(tx, wx);
		transform_node_cubic_weights(ty, wy);
		n0 = -1;
	}
	i32 n = filter == TRANSFORM_FILTER_LINEAR ? 2 : 4;
	out[0] = out[1] = out[2] = out[3] = 0.0f;
	for (i32 j = 0; j < n; ++j) {
		if (wy[j] == 0.0f) {
			continue;
		}
		for (i32 i = 0; i < n; ++i) {
			if (wx[i] == 0.0f) {
				continue;
			}
			f32 t[4];
			transform_node_fetch(pixels, size, ix + n0 + i, iy + n0 + j, tiling, t);
			f32 w = wx[i] * wy[j];
			for (i32 k = 0; k < 4; ++k) {
				out[k] += t[k] * w;
			}
		}
	}
}

static void transform_node_rows(void *param, i32 begin, i32 end) {
	transform_ctx_t    *c    = param;
	transform_params_t *p    = c->p;
	i32                 size = c->size;
	f32                 a    = p->rotation * (3.14159265f / 180.0f);
	f32                 ca   = cosf(a);
	f32                 sa   = sinf(a);
	f32                 sx   = p->scale_x < 0.0001f ? 0.0001f : p->scale_x;
	f32                 sy   = p->scale_y < 0.0001f ? 0.0001f : p->scale_y;
	for (i32 y = begin; y < end; ++y) {
		for (i32 x = 0; x < size; ++x) {
			f32 dx = ((f32)x + 0.5f) / (f32)size - p->pivot_x - p->offset_x;
			f32 dy = ((f32)y + 0.5f) / (f32)size - p->pivot_y - p->offset_y;
			f32 rx = dx * ca - dy * sa;
			f32 ry = dx * sa + dy * ca;
			rx /= sx;
			ry /= sy;
			rx = p->flip_x ? -rx : rx;
			ry = p->flip_y ? -ry : ry;
			transform_node_sample(c->pixels, size, p->pivot_x + rx, p->pivot_y + ry, p->filter, p->tiling, c->result + ((size_t)y * size + x) * 4);
		}
	}
}

static i32 transform_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	transform_ctx_t c = {.pixels = (f32 *)bytes, .result = result, .size = size, .p = params}; // Float capture
	make_image_node_parallel(size, transform_node_rows, &c);
	return 0;
}

static image_node_type_t transform_node_image_type = {
    .type          = "TEX_TRANSFORM",
    .result_format = GPU_TEXTURE_FORMAT_RGBA128,
    .params_size   = sizeof(transform_params_t),
    .size          = transform_node_size,
    .capture       = transform_node_capture,
    .capture_float = true,
    .smooth_sample = true,
    .params        = transform_node_params,
    .process       = transform_node_process,
};

char *transform_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.rgb", make_image_node_sample(node));
}

char *transform_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

static ui_node_socket_t *transform_node_socket(char *name, char *type, u32 color, f32_array_t *default_value, f32 min, f32 max, f32 precision) {
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

static ui_node_button_t *transform_node_setting(char *name, char *type, f32 default_value, char *data) {
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

void transform_node_init() {
	u32 value_color = 0xffa1a1a1;

	ui_node_t *transform_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Transform"),
	                .type   = "TEX_TRANSFORM",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        transform_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 0.0, 1.0, 100),
	                        transform_node_socket(_tr("Offset X"), "VALUE", value_color, f32_array_create_x(0.0), -1.0, 1.0, 100),
	                        transform_node_socket(_tr("Offset Y"), "VALUE", value_color, f32_array_create_x(0.0), -1.0, 1.0, 100),
	                        transform_node_socket(_tr("Rotation"), "VALUE", value_color, f32_array_create_x(0.0), -180.0, 180.0, 10),
	                        transform_node_socket(_tr("Scale X"), "VALUE", value_color, f32_array_create_x(1.0), 0.01, 8.0, 100),
	                        transform_node_socket(_tr("Scale Y"), "VALUE", value_color, f32_array_create_x(1.0), 0.01, 8.0, 100),
	                        transform_node_socket(_tr("Pivot X"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        transform_node_socket(_tr("Pivot Y"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                    },
	                    8),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        transform_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        transform_node_setting(_tr("Size"), "ENUM", 1, "512\n1024\n2048\n4096"),
	                        make_image_node_state_button(),
	                        transform_node_setting(_tr("Tiling"), "ENUM", TRANSFORM_TILING_TILE, "Tile\nMirror\nClamp\nNone"),
	                        transform_node_setting(_tr("Filter"), "ENUM", TRANSFORM_FILTER_CUBIC, "Linear\nCubic\nNearest"),
	                        transform_node_setting(_tr("Flip X"), "BOOL", 0, NULL),
	                        transform_node_setting(_tr("Flip Y"), "BOOL", 0, NULL),
	                    },
	                    6),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, transform_node_def);
	any_map_set(parser_material_node_values, "TEX_TRANSFORM", transform_node_value);
	any_map_set(parser_material_node_vectors, "TEX_TRANSFORM", transform_node_vector);
	make_image_node_register(&transform_node_image_type);
}
