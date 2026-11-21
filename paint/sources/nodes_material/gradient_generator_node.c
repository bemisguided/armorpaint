#include "../global.h"

// Gradient Generator (Pattern menu): a grey gradient, linear or radial. A pass-through shader node like Map Range and Levels: the gradient is
// computed per pixel in the material shader (str_gradient_generator below) on the texture coordinate, no baked result, no Size, no state row,
// no Texture section. Every input (Rotation, Tiling, Offset, Center X, Center Y, Radius, Softness) can be linked and is evaluated per pixel
// (Tiling is rounded to a whole number per pixel); Mode, Shape, Mirror and Invert are settings. The Mode setting decides the formula and which
// rows are shown (gradient_generator_node_item_hidden, a linked socket is always shown). The output is the usual colour output: the grey value
// in R, G and B (the alpha of a colour is not carried by shader colour sockets).
// The coordinate is the uv the material sees (tex_coord) wrapped into 0..1 (gradient_fract), so the picture repeats across a tiled or scaled
// uv exactly as the baked texture of the first build did; captured by an image node (Edge Detect, Transform...) tex_coord is the image uv and the
// gradient fills the image. Spec: armorpaint-admin specs/image-nodes/IMAGE_NODES.md (Gradient Generator).
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
float gradient_fract(float x) { \
	return x - floor(x); \
} \
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
float3 gradient_out3(float v, float invert) { \
	float g = v; \
	if (invert > 0.5) { g = 1.0 - v; } \
	return float3(g, g, g); \
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

// Node button indices (settings), all in the General section
enum {
	GRADIENT_GENERATOR_BUTTON_MODE,
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

static f32 gradient_generator_node_button(ui_node_t *node, i32 index) {
	return node->buttons->buffer[index]->default_value->buffer[0];
}

static bool gradient_generator_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	i32 mode  = (i32)gradient_generator_node_button(node, GRADIENT_GENERATOR_BUTTON_MODE);
	i32 shape = (i32)gradient_generator_node_button(node, GRADIENT_GENERATOR_BUTTON_SHAPE);
	return gradient_generator_node_item_hidden_for(mode, shape, kind, index);
}

// The input as a shader value: a literal, or per pixel when linked
static char *gradient_generator_node_input(ui_node_t *node, i32 input) {
	if (input >= node->inputs->length) {
		return "0.0"; // A node saved with fewer inputs (the safety net)
	}
	return parser_material_parse_value_input(node->inputs->buffer[input], false);
}

// The gradient per pixel on the wrapped texture coordinate, the same value in R, G and B
char *gradient_generator_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	node_shader_add_function(parser_material_kong, str_gradient_generator);
	char *uv = "gradient_fract(tex_coord.x), gradient_fract(tex_coord.y)";
	char *value;
	if ((i32)gradient_generator_node_button(node, GRADIENT_GENERATOR_BUTTON_MODE) == GRADIENT_GENERATOR_MODE_RADIAL) {
		value = string_tmp("gradient_radial(%s, %s, %s, %s, %s, %s)", uv, gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_CENTER_X),
		                   gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_CENTER_Y), gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_RADIUS),
		                   gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_SOFTNESS),
		                   gradient_generator_node_button(node, GRADIENT_GENERATOR_BUTTON_SHAPE) > 0.5 ? "1.0" : "0.0");
	}
	else {
		value = string_tmp("gradient_linear(%s, %s, %s, %s, %s, %s)", uv, gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_ROTATION),
		                   gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_TILING), gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_OFFSET),
		                   gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_SOFTNESS),
		                   gradient_generator_node_button(node, GRADIENT_GENERATOR_BUTTON_MIRROR) > 0.5 ? "1.0" : "0.0");
	}
	return string_tmp("gradient_out3(%s, %s)", value, gradient_generator_node_button(node, GRADIENT_GENERATOR_BUTTON_INVERT) > 0.5 ? "1.0" : "0.0");
}

// The sections of the node: General (the settings) and Parameters (the inputs). No Texture section: the node has no baked texture.
static void gradient_generator_node_sections(ui_node_t *def) {
	ui_node_section_t *list = calloc(3, sizeof(ui_node_section_t)); // The list ends with the zeroed entry
	list[0]                 = (ui_node_section_t){.title = _tr("General"), .kind = UI_NODE_ITEM_BUTTON, .first = 0, .count = def->buttons->length, .closed_by_default = false};
	list[1]                 = (ui_node_section_t){.title = _tr("Parameters"), .kind = UI_NODE_ITEM_INPUT, .first = 0, .count = def->inputs->length, .closed_by_default = false};
	any_map_set(ui_nodes_sections, def->type, list);
}

void gradient_generator_node_init() {
	u32 color_socket = 0xffc7c729; // The usual yellow of an RGBA socket
	u32 value_color  = 0xffa1a1a1;

	ui_node_t *gradient_generator_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Gradient Generator"),
	                .type   = "GRADIENT_GENERATOR",
	                .x      = 0,
	                .y      = 0,
	                .color  = 0xff62676d, // The grey header of Map Range and the Color nodes: a pass-through node, not an orange image node
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
	                        make_image_node_socket(_tr("Image"), "RGBA", color_socket, f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Mode"), "ENUM", 0, "Linear\nRadial"),
	                        make_image_node_setting(_tr("Shape"), "ENUM", 0, "Cone\nHemisphere"),
	                        make_image_node_setting(_tr("Mirror"), "BOOL", 0, NULL),
	                        make_image_node_setting(_tr("Invert"), "BOOL", 0, NULL),
	                    },
	                    4),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, gradient_generator_node_def);
	any_map_set(parser_material_node_vectors, "GRADIENT_GENERATOR", gradient_generator_node_vector);
	any_map_set(ui_nodes_hidden_rules, "GRADIENT_GENERATOR", gradient_generator_node_item_hidden);
	gradient_generator_node_sections(gradient_generator_node_def);
}
