#include "../global.h"

// Transform (Filter menu): moves, rotates, scales and flips an image in image space. An image node
// (render/make_image_node.c): the Image input is captured at Size (32 bit float, colour kept) and the result is
// computed on the cpu into one RGBA128 texture per node. It never touches the uv, Tile UV places an image on the mesh.
//
// Everything is about the Pivot: flip, then scale, then rotation, then Offset. Output pixel q reads the source at
// p = pivot + inverse(R * S * F) * (q - pivot - offset), R = [cos a, -sin a; sin a, cos a] on the image space (y down).
// Rotation follows the rotation convention of all image nodes (armorpaint-admin specs/image-nodes/ROTATION_CONVENTION.md):
// degrees, a positive angle turns the image clockwise as seen in the editor view (+90 sends the top left corner to the
// top right), about the Pivot, after the scale and before the Offset.
// Cubic is Catmull-Rom (make_image_node_sample_rgba, the shared cpu sampler), not Tile UV's B-spline: it passes through the texels, so the default settings give back
// the input pixel for pixel. No mipmaps: a source shrunk by more than about 2x aliases.
//
// Rotation and Scale are used as set for every Tiling mode. The output of a Tile Transform is periodic with period 1
// only when the rotation maps the lattice onto itself (multiples of 90 degrees with an integer 1 / Scale), otherwise
// it shows a seam when tiled (known limit). The maths is in armorpaint-admin specs/image-nodes/IMAGE_NODES.md, section Transform.

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

// Filter setting values (the order of the combo); the sampler takes IMAGE_FILTER_*, mapped in transform_node_params.
// The Tiling setting values are IMAGE_TILING_* (types.h) in the same order as its combo.
enum {
	TRANSFORM_FILTER_LINEAR,
	TRANSFORM_FILTER_CUBIC,
	TRANSFORM_FILTER_NEAREST,
};

typedef struct transform_params {
	f32 offset_x;
	f32 offset_y;
	f32 rotation; // Degrees, positive turns the image clockwise on screen
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
	i32 filter            = (i32)transform_node_button(node, TRANSFORM_BUTTON_FILTER);
	p->filter             = filter == TRANSFORM_FILTER_LINEAR ? IMAGE_FILTER_LINEAR : filter == TRANSFORM_FILTER_CUBIC ? IMAGE_FILTER_CUBIC : IMAGE_FILTER_NEAREST;
	p->flip_x             = transform_node_button(node, TRANSFORM_BUTTON_FLIP_X) != 0.0f;
	p->flip_y             = transform_node_button(node, TRANSFORM_BUTTON_FLIP_Y) != 0.0f;
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
			f32 rx = dx * ca + dy * sa; // inverse(R) (q - pivot - offset): the source is read where the image content came from
			f32 ry = -dx * sa + dy * ca;
			rx /= sx;
			ry /= sy;
			rx = p->flip_x ? -rx : rx;
			ry = p->flip_y ? -ry : ry;
			make_image_node_sample_rgba(c->pixels, size, p->pivot_x + rx, p->pivot_y + ry, p->filter, p->tiling, c->result + ((size_t)y * size + x) * 4);
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
	                        transform_node_socket(_tr("Rotation"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 360.0, 10),
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
	                        transform_node_setting(_tr("Tiling"), "ENUM", IMAGE_TILING_TILE, "Tile\nMirror\nClamp\nNone"),
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
