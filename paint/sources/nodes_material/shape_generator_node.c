#include "../global.h"

// Shape Generator (Pattern menu): one shape (circle, square, rectangle) as a grey mask, 1 inside and 0 outside. An image
// node (render/make_image_node.c) without an Image input: nothing is captured but a constant, the result is computed on the
// cpu from the settings and the number inputs into one RGBA128 texture per node (float, so soft edges do not band).
// Width and Height are the full extent of the shape as a fraction of the image (Circle: the two diameters of an ellipse,
// Square: Width only). Per pixel the signed distance to the shape in pixels gives the value 1 - smoothstep(-w, w, d),
// w = Edge Softness in pixels, at least 1 so the edge is antialiased. Wrap Edges adds up the coverage of the nine image
// copies of the shape (clamped to 1, so a shape as large as the image stays 1 on its border), so a shape that crosses the image border continues on the other side (the centre is reduced into the image first, more
// copies each way when the shape extent plus the softness reaches farther than one image).
// The number inputs hold for the whole image (scalars, evaluated at the image centre when linked).
// Rotation follows the rotation convention of all image nodes (armorpaint-admin specs/image-nodes/ROTATION_CONVENTION.md):
// degrees, a positive angle turns the shape clockwise as seen in the editor view (y down), about the shape centre, after
// the Width and Height and before Position.

// Input socket indices
enum {
	SHAPE_GENERATOR_INPUT_WIDTH         = 0,
	SHAPE_GENERATOR_INPUT_HEIGHT        = 1,
	SHAPE_GENERATOR_INPUT_POSITION_X    = 2,
	SHAPE_GENERATOR_INPUT_POSITION_Y    = 3,
	SHAPE_GENERATOR_INPUT_ROTATION      = 4,
	SHAPE_GENERATOR_INPUT_CORNER_RADIUS = 5,
	SHAPE_GENERATOR_INPUT_EDGE_SOFTNESS = 6,
};

// Node button indices (settings)
enum {
	SHAPE_GENERATOR_BUTTON_SIZE   = 0, // The state row is button 1
	SHAPE_GENERATOR_BUTTON_SHAPE  = 2,
	SHAPE_GENERATOR_BUTTON_INVERT = 3,
	SHAPE_GENERATOR_BUTTON_WRAP   = 4,
};

enum {
	SHAPE_GENERATOR_CIRCLE    = 0,
	SHAPE_GENERATOR_SQUARE    = 1,
	SHAPE_GENERATOR_RECTANGLE = 2,
};

typedef struct shape_generator_params {
	i32 shape;
	i32 invert;
	i32 wrap;
	f32 width;
	f32 height;
	f32 position_x;
	f32 position_y;
	f32 rotation;
	f32 corner_radius;
	f32 edge_softness;
} shape_generator_params_t;

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 shape_generator_node_size(ui_node_t *node) {
	return make_image_node_size(node, SHAPE_GENERATOR_BUTTON_SIZE);
}

// Nothing depends on an image: a constant is captured
static char *shape_generator_node_capture(ui_node_t *node) {
	return "float4(0.0, 0.0, 0.0, 1.0)";
}

static void shape_generator_node_params(ui_node_t *node, void *params) {
	shape_generator_params_t *p = params;
	p->shape                    = (i32)make_image_node_button(node, SHAPE_GENERATOR_BUTTON_SHAPE);
	p->invert                   = (i32)make_image_node_button(node, SHAPE_GENERATOR_BUTTON_INVERT);
	p->wrap                     = (i32)make_image_node_button(node, SHAPE_GENERATOR_BUTTON_WRAP);
	p->width                    = make_image_node_input_value(node, SHAPE_GENERATOR_INPUT_WIDTH);
	p->height                   = make_image_node_input_value(node, SHAPE_GENERATOR_INPUT_HEIGHT);
	p->position_x               = make_image_node_input_value(node, SHAPE_GENERATOR_INPUT_POSITION_X);
	p->position_y               = make_image_node_input_value(node, SHAPE_GENERATOR_INPUT_POSITION_Y);
	p->rotation                 = make_image_node_input_value(node, SHAPE_GENERATOR_INPUT_ROTATION);
	p->corner_radius            = make_image_node_input_value(node, SHAPE_GENERATOR_INPUT_CORNER_RADIUS);
	p->edge_softness            = make_image_node_input_value(node, SHAPE_GENERATOR_INPUT_EDGE_SOFTNESS);
}

// What the shape uses (see shape_generator_node_coverage): a Square takes its Height from Width, a Circle (an ellipse) has no
// corners. Rotation is used by every shape. A linked socket still shows (ui_node_item_hidden).
static bool shape_generator_node_input_hidden(i32 shape, i32 input) {
	return (input == SHAPE_GENERATOR_INPUT_HEIGHT && shape == SHAPE_GENERATOR_SQUARE) ||
	       (input == SHAPE_GENERATOR_INPUT_CORNER_RADIUS && shape == SHAPE_GENERATOR_CIRCLE);
}

static bool shape_generator_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	return kind == UI_NODE_ITEM_INPUT && shape_generator_node_input_hidden((i32)make_image_node_button(node, SHAPE_GENERATOR_BUTTON_SHAPE), index);
}

static f32 shape_generator_node_clamp(f32 v, f32 lo, f32 hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

// Signed distance in pixels of the point (x, y), relative to the shape centre and already rotated, to a shape with the half
// extents (hx, hy): negative inside. Ellipse: first order approximation. Box: the rounded box form, r the corner radius.
static f32 shape_generator_node_distance(i32 shape, f32 x, f32 y, f32 hx, f32 hy, f32 r) {
	if (shape == SHAPE_GENERATOR_CIRCLE) {
		f32 nx = x / hx;
		f32 ny = y / hy;
		return (sqrtf(nx * nx + ny * ny) - 1.0f) * (hx < hy ? hx : hy);
	}
	f32 qx = fabsf(x) - hx + r;
	f32 qy = fabsf(y) - hy + r;
	f32 ox = qx > 0.0f ? qx : 0.0f;
	f32 oy = qy > 0.0f ? qy : 0.0f;
	f32 m  = qx > qy ? qx : qy;
	return sqrtf(ox * ox + oy * oy) + (m < 0.0f ? m : 0.0f) - r;
}

// Value of the mask at the pixel (x, y) of a size x size image
static f32 shape_generator_node_coverage(shape_generator_params_t *p, i32 size, f32 x, f32 y) {
	f32 s    = (f32)size;
	f32 hx   = shape_generator_node_clamp(p->width, 0.0f, 1.0f) * s * 0.5f;
	f32 hy   = p->shape == SHAPE_GENERATOR_SQUARE ? hx : shape_generator_node_clamp(p->height, 0.0f, 1.0f) * s * 0.5f;
	hx       = hx < 0.0001f ? 0.0001f : hx;
	hy       = hy < 0.0001f ? 0.0001f : hy;
	f32 r    = p->shape == SHAPE_GENERATOR_CIRCLE ? 0.0f : shape_generator_node_clamp(p->corner_radius, 0.0f, 0.5f) * 2.0f * (hx < hy ? hx : hy);
	f32 cx   = s * 0.5f + p->position_x * s;
	f32 cy   = s * 0.5f + p->position_y * s;
	if (p->wrap) {
		// The centre inside the image, so the copies at -1..1 are the ones next to it
		cx -= floorf(cx / s) * s;
		cy -= floorf(cy / s) * s;
	}
	f32 a    = p->rotation * (3.14159265f / 180.0f);
	f32 ca   = cosf(a);
	f32 sa   = sinf(a);
	f32 w    = p->edge_softness * s;
	w        = w < 1.0f ? 1.0f : w;
	// A copy matters as far away as the value is above 0: the shape extent plus the softness, and for the ellipse the first
	// order distance gets small far out along the long axis, so the softness band grows with the aspect ratio. A copy k
	// images away is at least k - 1 images from every pixel; the count is bounded for the thinnest shapes.
	f32 hmin  = hx < hy ? hx : hy;
	f32 hmax  = hx < hy ? hy : hx;
	f32 reach = p->shape == SHAPE_GENERATOR_CIRCLE ? hmax * (1.0f + w / hmin) : sqrtf(hx * hx + hy * hy) + w;
	i32 n     = (i32)ceilf(reach / s);
	n         = n < 1 ? 1 : n > 4 ? 4 : n;
	i32 from  = p->wrap ? -n : 0;
	i32 to    = p->wrap ? n : 0;
	f32 v    = 0.0f;
	for (i32 oy = from; oy <= to; ++oy) {
		for (i32 ox = from; ox <= to; ++ox) {
			f32 dx = x - (cx + (f32)ox * s);
			f32 dy = y - (cy + (f32)oy * s);
			f32 rx = dx * ca + dy * sa; // The pixel in the frame of the shape: inverse of the clockwise turn
			f32 ry = -dx * sa + dy * ca;
			f32 di = shape_generator_node_distance(p->shape, rx, ry, hx, hy, r);
			f32 t  = shape_generator_node_clamp((di + w) / (2.0f * w), 0.0f, 1.0f);
			v += 1.0f - t * t * (3.0f - 2.0f * t); // The coverages of the copies add up: two copies that abut share one antialiased edge (0.84 + 0.16)
		}
	}
	v = v > 1.0f ? 1.0f : v;
	return p->invert ? 1.0f - v : v;
}

typedef struct shape_generator_rows {
	shape_generator_params_t *p;
	f32                      *result;
	i32                       size;
} shape_generator_rows_t;

static void shape_generator_node_rows(void *param, i32 begin, i32 end) {
	shape_generator_rows_t *c = param;
	for (i32 y = begin; y < end; ++y) {
		for (i32 x = 0; x < c->size; ++x) {
			f32  v = shape_generator_node_coverage(c->p, c->size, (f32)x + 0.5f, (f32)y + 0.5f);
			f32 *r = c->result + ((size_t)y * c->size + x) * 4;
			r[0]   = v;
			r[1]   = v;
			r[2]   = v;
			r[3]   = 1.0f;
		}
	}
}

static i32 shape_generator_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	shape_generator_rows_t c = {.p = params, .result = result, .size = size};
	make_image_node_parallel(size, shape_generator_node_rows, &c);
	return 0;
}

static image_node_type_t shape_generator_node_image_type = {
    .type          = "TEX_SHAPE_GENERATOR",
    .result_format = GPU_TEXTURE_FORMAT_RGBA128, // RGBA128 on purpose: Edge Softness makes ramps as wide as the image that a Bump node or a blur reads, half floats would band at 4096
    .params_size   = sizeof(shape_generator_params_t),
    .size          = shape_generator_node_size,
    .capture       = shape_generator_node_capture,
    .smooth_sample = true,
    .params        = shape_generator_node_params,
    .process       = shape_generator_node_process,
};

char *shape_generator_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	char *v = string_tmp("%s.r", make_image_node_sample(node));
	return string_tmp("float3(%s, %s, %s)", v, v, v);
}

char *shape_generator_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

void shape_generator_node_init() {
	u32 value_color = 0xffa1a1a1;

	ui_node_t *shape_generator_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Shape Generator"),
	                .type   = "TEX_SHAPE_GENERATOR",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Width"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Height"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Position X"), "VALUE", value_color, f32_array_create_x(0.0), -1.0, 1.0, 100),
	                        make_image_node_socket(_tr("Position Y"), "VALUE", value_color, f32_array_create_x(0.0), -1.0, 1.0, 100),
	                        make_image_node_socket(_tr("Rotation"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 360.0, 10),
	                        make_image_node_socket(_tr("Corner Radius"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 0.5, 100),
	                        make_image_node_socket(_tr("Edge Softness"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                    },
	                    7),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Size"), "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS),
	                        make_image_node_state_button(),
	                        make_image_node_setting(_tr("Shape"), "ENUM", 0, "Circle\nSquare\nRectangle"),
	                        make_image_node_setting(_tr("Invert"), "BOOL", 0, NULL),
	                        make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                    },
	                    5),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, shape_generator_node_def);
	any_map_set(parser_material_node_values, "TEX_SHAPE_GENERATOR", shape_generator_node_value);
	any_map_set(parser_material_node_vectors, "TEX_SHAPE_GENERATOR", shape_generator_node_vector);
	any_map_set(ui_nodes_hidden_rules, "TEX_SHAPE_GENERATOR", shape_generator_node_item_hidden);
	make_image_node_sections("TEX_SHAPE_GENERATOR", NULL, 0); // Bake only: Placement needs the size inputs above it, see the input order
	make_image_node_register(&shape_generator_node_image_type);
}
