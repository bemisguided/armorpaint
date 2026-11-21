#include "../global.h"

// Gradient Generator (Pattern menu): a grey gradient image, linear or radial. An image node (render/make_image_node.c) that
// generates its image itself, like the Noise Generator: the capture shader writes the gradient per pixel in image space
// (uv across the image, y down, float, grey), the result is the baked texture at the Size setting (RGBA128, no banding),
// sampled by Tile UV like every other pattern node. No Image and no Vector input. The inputs are sockets (linkable, a linked
// image is a per pixel field); the Mode setting decides the formula and which rows are shown (gradient_generator_node_item_hidden,
// a linked socket is always shown). Spec: armorpaint-admin specs/image-nodes/IMAGE_NODES.md (Gradient Generator).
//
// Linear: d = (cos a, sin a) (a = Rotation in degrees, clockwise on screen, ROTATION_CONVENTION.md: 0 runs from black on the
//   left to white on the right, 90 from top to bottom). The position along the ramp, normalised so that one ramp covers the
//   projected extent of the square image at every rotation (the image corners are 0 and 1, no clamped corners at Tiling 1):
//     s = dot(uv - 0.5, d) / (|cos a| + |sin a|) + 0.5 + Offset,   t = s * Tiling
//   Tiling 1: f = clamp(t, 0, 1) (outside the image after an Offset the ends hold). Tiling n >= 2: f = t - floor(t), the whole
//   ramp repeated n times (a sawtooth: a hard step from 1 to 0 at each repeat; Offset shifts the phase of the repeats).
//   Mirror off: v = f. Mirror on: v = 1 - |2 f - 1| (a triangle: 0 at the tile edges, 1 at the tile centre, so the peak is white).
//   Softness (0..1): ease(v) = lerp(v, v * v * (3 - 2 v), Softness), a smoothstep blend, monotone, no overshoot.
//   Linear 1 = Mirror off, Softness 0; Linear 2 = Mirror on, Softness 1; Linear 3 = Mirror on, Softness 0.
// Radial: r = distance(uv, (Center X, Center Y)) / max(Radius, 1e-6), the centre in absolute image positions 0..1 (not an offset
//   from the image centre like Shape Generator's Position), Radius 0.5 touches the borders.
//   Cone: v = ease(1 - clamp(r, 0, 1)). Hemisphere: v = sqrt(1 - min(r, 1)^2) (Softness not used).
//   Radius 0 is a flat 0 (a single texel can be 1 when a texel centre is exactly the centre): no division by zero, no NaN.
// Invert: 1 - v. The peak is white: the centre of a mirrored linear gradient and of the radial shapes is white.
// Tiling n >= 2 repeats exactly across a tiled copy of the image where n * cos a / (|cos a| + |sin a|) and n * sin a / (...)
// are integers: rotation 0, 90, 180, 270 for any Tiling, 45 for an even Tiling. There is no Tileable setting.

char *str_gradient_generator = "\
float gradient_ease(float v, float soft) { \
	float s = clamp(soft, 0.0, 1.0); \
	return lerp(v, v * v * (3.0 - 2.0 * v), s); \
} \
float gradient_linear(float u, float v, float rotation, float tiling, float offset, float soft, float mirror) { \
	float a = rotation * 0.017453292519943295; \
	float c = cos(a); \
	float s = sin(a); \
	float p = ((u - 0.5) * c + (v - 0.5) * s) / (abs(c) + abs(s)) + 0.5 + offset; \
	float n = max(1.0, floor(tiling + 0.5)); \
	float t = p * n; \
	float f = clamp(t, 0.0, 1.0); \
	if (n > 1.5) { f = t - floor(t); } \
	float w = f; \
	if (mirror > 0.5) { w = 1.0 - abs(2.0 * f - 1.0); } \
	return gradient_ease(w, soft); \
} \
float gradient_radial(float u, float v, float cx, float cy, float radius, float soft, float hemisphere) { \
	float dx = u - cx; \
	float dy = v - cy; \
	float r = sqrt(dx * dx + dy * dy) / max(radius, 0.000001); \
	if (hemisphere > 0.5) { \
		float q = min(r, 1.0); \
		return sqrt(max(1.0 - q * q, 0.0)); \
	} \
	return gradient_ease(1.0 - clamp(r, 0.0, 1.0), soft); \
} \
";

// Input socket indices (all in the Parameters section). Softness is shared: Linear and the Cone use it.
enum {
	GRADIENT_GENERATOR_INPUT_ROTATION,
	GRADIENT_GENERATOR_INPUT_TILING,
	GRADIENT_GENERATOR_INPUT_OFFSET,
	GRADIENT_GENERATOR_INPUT_CENTER_X,
	GRADIENT_GENERATOR_INPUT_CENTER_Y,
	GRADIENT_GENERATOR_INPUT_RADIUS,
	GRADIENT_GENERATOR_INPUT_SOFTNESS,
};

// Node button indices (settings): Size, the state row, then the General section
enum {
	GRADIENT_GENERATOR_BUTTON_SIZE = 0, // The state row is button 1
	GRADIENT_GENERATOR_BUTTON_MODE = 2,
	GRADIENT_GENERATOR_BUTTON_SHAPE,
	GRADIENT_GENERATOR_BUTTON_MIRROR,
	GRADIENT_GENERATOR_BUTTON_INVERT,
};

enum {
	GRADIENT_GENERATOR_MODE_LINEAR,
	GRADIENT_GENERATOR_MODE_RADIAL,
};

enum {
	GRADIENT_GENERATOR_SHAPE_CONE,
	GRADIENT_GENERATOR_SHAPE_HEMISPHERE,
};

// What each mode shows: Linear has Rotation, Tiling, Offset, Softness, Mirror; Radial has Center X/Y, Radius, Shape, and
// Softness for the Cone only. Invert is always shown. A linked socket still shows (ui_node_item_hidden).
static bool gradient_generator_node_item_hidden_for(i32 mode, i32 shape, ui_node_item_t kind, i32 index) {
	if (kind == UI_NODE_ITEM_INPUT) {
		if (mode == GRADIENT_GENERATOR_MODE_LINEAR) {
			return index == GRADIENT_GENERATOR_INPUT_CENTER_X || index == GRADIENT_GENERATOR_INPUT_CENTER_Y || index == GRADIENT_GENERATOR_INPUT_RADIUS;
		}
		if (index == GRADIENT_GENERATOR_INPUT_ROTATION || index == GRADIENT_GENERATOR_INPUT_TILING || index == GRADIENT_GENERATOR_INPUT_OFFSET) {
			return true;
		}
		return index == GRADIENT_GENERATOR_INPUT_SOFTNESS && shape == GRADIENT_GENERATOR_SHAPE_HEMISPHERE;
	}
	if (kind == UI_NODE_ITEM_BUTTON) {
		if (mode == GRADIENT_GENERATOR_MODE_LINEAR) {
			return index == GRADIENT_GENERATOR_BUTTON_SHAPE;
		}
		return index == GRADIENT_GENERATOR_BUTTON_MIRROR;
	}
	return false;
}

static bool gradient_generator_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	i32 mode  = (i32)make_image_node_button(node, GRADIENT_GENERATOR_BUTTON_MODE);
	i32 shape = (i32)make_image_node_button(node, GRADIENT_GENERATOR_BUTTON_SHAPE);
	return gradient_generator_node_item_hidden_for(mode, shape, kind, index);
}

// The input as a shader value: a literal, or per pixel when linked
static char *gradient_generator_node_input(ui_node_t *node, i32 input) {
	if (input >= node->inputs->length) {
		return "0.0"; // A node saved with fewer inputs (make_image_node_reconcile completes it, this is the safety net)
	}
	return parser_material_parse_value_input(node->inputs->buffer[input], false);
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 gradient_generator_node_size(ui_node_t *node) {
	return make_image_node_size(node, GRADIENT_GENERATOR_BUTTON_SIZE);
}

// The capture is the generator: the gradient per pixel, float, the same value in R, G and B
static char *gradient_generator_node_capture(ui_node_t *node) {
	node_shader_add_function(parser_material_kong, str_gradient_generator);
	char *value;
	if ((i32)make_image_node_button(node, GRADIENT_GENERATOR_BUTTON_MODE) == GRADIENT_GENERATOR_MODE_RADIAL) {
		value = string_tmp("gradient_radial(tex_coord.x, tex_coord.y, %s, %s, %s, %s, %s)", gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_CENTER_X),
		                   gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_CENTER_Y), gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_RADIUS),
		                   gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_SOFTNESS),
		                   make_image_node_button(node, GRADIENT_GENERATOR_BUTTON_SHAPE) > 0.5 ? "1.0" : "0.0");
	}
	else {
		value = string_tmp("gradient_linear(tex_coord.x, tex_coord.y, %s, %s, %s, %s, %s)", gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_ROTATION),
		                   gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_TILING), gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_OFFSET),
		                   gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_SOFTNESS),
		                   make_image_node_button(node, GRADIENT_GENERATOR_BUTTON_MIRROR) > 0.5 ? "1.0" : "0.0");
	}
	if (make_image_node_button(node, GRADIENT_GENERATOR_BUTTON_INVERT) > 0.5) {
		value = string_tmp("(1.0 - %s)", value);
	}
	char *var = string_tmp("%s_gradient", parser_material_store_var_name(node));
	parser_material_write(parser_material_kong, string_tmp("float %s = %s;", var, value));
	return string_tmp("float4(%s, %s, %s, 1.0)", var, var, var);
}

// Everything is in the shader (and so in the signature of the captured pixels): no processing parameters
static void gradient_generator_node_params(ui_node_t *node, void *params) {
}

// The result is the capture: copied into the RGBA result
static i32 gradient_generator_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	memcpy(result, bytes, (size_t)size * size * 4 * sizeof(f32));
	return 0;
}

static image_node_type_t gradient_generator_node_image_type = {
    .type          = "TEX_GRADIENT_GENERATOR",
    .result_format = GPU_TEXTURE_FORMAT_RGBA128, // Float: a ramp as wide as the image bands in half floats
    .params_size   = 0,
    .size          = gradient_generator_node_size,
    .capture       = gradient_generator_node_capture,
    .capture_format = IMAGE_CAPTURE_RGBA32F,
    .smooth_sample = true,
    .params        = gradient_generator_node_params,
    .process       = gradient_generator_node_process,
};

char *gradient_generator_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	char *v = string_tmp("%s.r", make_image_node_sample(node));
	return string_tmp("float3(%s, %s, %s)", v, v, v);
}

char *gradient_generator_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

void gradient_generator_node_init() {
	u32 value_color = 0xffa1a1a1;

	ui_node_t *gradient_generator_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Gradient Generator"),
	                .type   = "TEX_GRADIENT_GENERATOR",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Rotation"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 360.0, 10),
	                        make_image_node_socket(_tr("Tiling"), "VALUE", value_color, f32_array_create_x(1.0), 1.0, 64.0, 1),
	                        make_image_node_socket(_tr("Offset"), "VALUE", value_color, f32_array_create_x(0.0), -1.0, 1.0, 100),
	                        make_image_node_socket(_tr("Center X"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Center Y"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Radius"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 2.0, 100),
	                        make_image_node_socket(_tr("Softness"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
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
	                        make_image_node_setting(_tr("Mode"), "ENUM", 0, "Linear\nRadial"),
	                        make_image_node_setting(_tr("Shape"), "ENUM", 0, "Cone\nHemisphere"),
	                        make_image_node_setting(_tr("Mirror"), "BOOL", 0, NULL),
	                        make_image_node_setting(_tr("Invert"), "BOOL", 0, NULL),
	                    },
	                    6),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, gradient_generator_node_def);
	any_map_set(parser_material_node_vectors, "TEX_GRADIENT_GENERATOR", gradient_generator_node_vector);
	any_map_set(parser_material_node_values, "TEX_GRADIENT_GENERATOR", gradient_generator_node_value);
	any_map_set(ui_nodes_hidden_rules, "TEX_GRADIENT_GENERATOR", gradient_generator_node_item_hidden);
	make_image_node_sections(gradient_generator_node_def, NULL, 0, 0); // General: Mode, Shape, Mirror, Invert; Parameters: all inputs
	make_image_node_register(&gradient_generator_node_image_type);
}
