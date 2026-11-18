#include "../global.h"

// Seamless (Filter menu): makes the Image input repeat without a visible seam. An image node (render/make_image_node.c):
// the Image input is evaluated in image space and captured at Size (32 bit float, colour kept), the result is
// computed on the cpu into one RGBA128 texture per node. It is sampled like an Image Texture at tex_coord, repeating
// (Tile UV places it on the uv).
//
// The usual make-it-tile: the image is blended with a copy of itself shifted by half the image in x and y. The weight
// of the original is 0 on the borders and ramps (smoothstep) to 1 over Blend Width, a fraction of the image measured
// from each border, in x and y. On a border the result is the shifted copy, which meets itself across the repeat, and
// the weight is 0 on both sides of it, so the image repeats without a seam. The price is a cross fade inside the
// Blend Width: detail there is mixed from two places (ghosting on strong features), a smaller Blend Width keeps more
// of the original and shows the join more.
// Blend Width holds for the whole image (scalar, evaluated at the image centre when linked).

// Input socket indices
enum {
	SEAMLESS_INPUT_IMAGE       = 0,
	SEAMLESS_INPUT_BLEND_WIDTH = 1,
};

// Node button indices (settings)
enum {
	SEAMLESS_BUTTON_SIZE = 0,
};

typedef struct seamless_params {
	f32 blend_width;
} seamless_params_t;

static f32 seamless_node_button(ui_node_t *node, i32 i) {
	return node->buttons->buffer[i]->default_value->buffer[0];
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 seamless_node_size(ui_node_t *node) {
	i32 i = (i32)seamless_node_button(node, SEAMLESS_BUTTON_SIZE);
	return 512 << (i < 0 ? 0 : i > 3 ? 3 : i);
}

// Captured per pixel: the Image input
static char *seamless_node_capture(ui_node_t *node) {
	char *image = parser_material_parse_vector_input(node->inputs->buffer[SEAMLESS_INPUT_IMAGE]);
	return string_tmp("float4(%s, 1.0)", image);
}

static void seamless_node_params(ui_node_t *node, void *params) {
	seamless_params_t *p = params;
	p->blend_width       = make_image_node_input_value(node, SEAMLESS_INPUT_BLEND_WIDTH);
}

// Weight of the original at distance d (0 border, 0.5 centre of the image, both as a fraction of the image)
static f32 seamless_node_weight(f32 d, f32 blend_width) {
	if (blend_width <= 0.0001f) {
		return d > 0.0f ? 1.0f : 0.0f;
	}
	f32 t = d / blend_width;
	t     = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return t * t * (3.0f - 2.0f * t);
}

static i32 seamless_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	f32              *pixels = (f32 *)bytes; // Float capture
	seamless_params_t p      = *(seamless_params_t *)params;
	f32               bw     = p.blend_width < 0.0f ? 0.0f : p.blend_width > 0.5f ? 0.5f : p.blend_width;
	i32               half   = size / 2;
	f32              *wx     = malloc((size_t)size * sizeof(f32));
	if (wx == NULL) {
		return -1;
	}
	for (i32 x = 0; x < size; ++x) {
		f32 u = ((f32)x + 0.5f) / (f32)size;
		f32 d = u < 1.0f - u ? u : 1.0f - u;
		wx[x] = seamless_node_weight(d, bw);
	}
	for (i32 y = 0; y < size; ++y) {
		i32 ys = (y + half) % size;
		f32 wy = wx[y];
		for (i32 x = 0; x < size; ++x) {
			i32 xs = (x + half) % size;
			f32 w  = wx[x] * wy;
			f32 *a = pixels + ((size_t)y * size + x) * 4;
			f32 *b = pixels + ((size_t)ys * size + xs) * 4;
			f32 *r = result + ((size_t)y * size + x) * 4;
			for (i32 c = 0; c < 3; ++c) {
				r[c] = a[c] * w + b[c] * (1.0f - w);
			}
			r[3] = 1.0f;
		}
	}
	free(wx);
	return 0;
}

static image_node_type_t seamless_node_image_type = {
    .type          = "TEX_SEAMLESS",
    .result_format = GPU_TEXTURE_FORMAT_RGBA128,
    .params_size   = sizeof(seamless_params_t),
    .size          = seamless_node_size,
    .capture       = seamless_node_capture,
    .capture_float = true,
    .smooth_sample = true,
    .params        = seamless_node_params,
    .process       = seamless_node_process,
};

char *seamless_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.rgb", make_image_node_sample(node));
}

char *seamless_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

static ui_node_socket_t *seamless_node_socket(char *name, char *type, u32 color, f32_array_t *default_value, f32 min, f32 max, f32 precision) {
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

static ui_node_button_t *seamless_node_setting(char *name, f32 default_value, char *data) {
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

void seamless_node_init() {
	u32 value_color = 0xffa1a1a1;

	ui_node_t *seamless_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Seamless"),
	                .type   = "TEX_SEAMLESS",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        seamless_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 0.0, 1.0, 100),
	                        seamless_node_socket(_tr("Blend Width"), "VALUE", value_color, f32_array_create_x(0.25), 0.0, 0.5, 100),
	                    },
	                    2),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        seamless_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        seamless_node_setting(_tr("Size"), 1, "512\n1024\n2048\n4096"),
	                        make_image_node_state_button(),
	                    },
	                    2),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, seamless_node_def);
	any_map_set(parser_material_node_values, "TEX_SEAMLESS", seamless_node_value);
	any_map_set(parser_material_node_vectors, "TEX_SEAMLESS", seamless_node_vector);
	make_image_node_register(&seamless_node_image_type);
}
