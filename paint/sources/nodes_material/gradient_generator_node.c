#include "../global.h"

// Gradient Generator (Pattern menu): a grey gradient, linear or radial. A pass-through shader node like Map Range and Levels: the gradient is
// computed per pixel in the material shader (str_gradient_generator below) on the texture coordinate, no baked result, no Size, no state row,
// no Texture section. Every input (Rotation, Tiling, Offset, Center X, Center Y, Radius, Softness) can be linked and is evaluated per pixel
// (Tiling is rounded to a whole number per pixel); Mode, Shape, Mirror and Invert are settings. The Mode setting decides the formula and which
// rows are shown (gradient_generator_node_item_hidden, a linked socket is always shown). The output is the usual colour output: the grey value
// in R, G and B (the alpha of a colour is not carried by shader colour sockets).
// The coordinate comes from the Vector input (make_image_node_coord, the rule of all coordinate generators): unlinked it is the uv the material sees
// (tex_coord), linked the xy of the linked expression (Mapping, Texture Coordinate...). Wrap Edges (default on) reduces it into 0..1 (x - floor(x)), so
// the picture repeats across a tiled or scaled uv or a Mapping scale, exactly as the baked texture of the first build did; off it is the raw coordinate
// (Blender style: Tiling 1 clamps the ends, Tiling n >= 2 still repeats the ramp n times). Under triplanar projection an unlinked Vector is evaluated
// once per projection and blended (make_image_node_eval_coord), a linked one once. Captured by an image node (Edge Detect, Transform...) tex_coord is the
// image uv and the gradient fills the image, wrapped or raw alike over 0..1. Spec: armorpaint-admin specs/image-nodes/IMAGE_NODES.md (Gradient Generator).
//
// Linear: d = (cos a, sin a) (a = Rotation in degrees, clockwise on screen, ROTATION_CONVENTION.md: 0 runs from black on the
//   left to white on the right, 90 from top to bottom). The position along the ramp, normalised so that one ramp covers the
//   projected extent of the square image at every rotation (the image corners are 0 and 1, no clamped corners at Tiling 1):
//     s = dot(uv - 0.5, d) / (|cos a| + |sin a|) + 0.5 + Offset,   t = s * Tiling
//   Tiling 1: f = clamp(t, 0, 1) (outside the image after an Offset the ends hold). Tiling n >= 2: f = t - floor(t), the whole
//   ramp repeated n times (a sawtooth: a hard step from 1 to 0 at each repeat; Offset shifts the phase of the repeats).
//   The Tiling is rounded to a whole number per pixel (n = max(1, floor(Tiling + 0.5))). While n >= 2 the Rotation SNAPS to the nearest multiple of
//   90 degrees (0, 90, 180, 270; a half way angle goes up, Substance Designer's tiling gradient only has steps of 90 too), so a tiled gradient always
//   repeats exactly across a tiled copy of the image: with cos a and sin a one of 0 and +-1 the span of the ramp is 1 and n repeats fit the image.
//   Tiling 1 (clamped, nothing repeats) keeps the continuous Rotation. The shader snaps per pixel (a linked Rotation or Tiling too); the stored
//   Rotation value is written back (gradient_generator_node_settle) while Tiling >= 2 and both sockets are unlinked, so the slider shows the angle
//   that is drawn (Tiling back to 1 leaves the snapped value as it is: the continuous value is lost).
//   Mirror off: v = f. Mirror on: v = 1 - |2 f - 1| (a triangle: 0 at the tile edges, 1 at the tile centre, so the peak is white).
//   Softness (0..1): ease(v) = lerp(v, v * v * (3 - 2 v), Softness), a smoothstep blend, monotone, no overshoot.
//   Linear 1 = Mirror off, Softness 0; Linear 2 = Mirror on, Softness 1; Linear 3 = Mirror on, Softness 0.
// Radial: r = distance(uv, (Center X, Center Y)) / max(Radius, 1e-6), the centre in absolute image positions 0..1 (not an offset
//   from the image centre like Shape Generator's Position), Radius 0.5 touches the borders.
//   Cone: v = ease(1 - clamp(r, 0, 1)). Hemisphere: v = sqrt(1 - min(r, 1)^2) (Softness not used).
//   Radius 0 is a flat 0 (a single texel can be 1 when a texel centre is exactly the centre): no division by zero, no NaN.
// Invert: 1 - v. The peak is white: the centre of a mirrored linear gradient and of the radial shapes is white.
// Tiling n >= 2 repeats exactly across a tiled copy of the image at every Rotation because the Rotation is a multiple of 90 then (see above).
// There is no Tileable setting.

char *str_gradient_generator = "\
float gradient_ease(float v, float soft) { \
	float s = clamp(soft, 0.0, 1.0); \
	return lerp(v, v * v * (3.0 - 2.0 * v), s); \
} \
float gradient_linear(float u, float v, float rotation, float tiling, float offset, float soft, float mirror) { \
	float n = max(1.0, floor(tiling + 0.5)); \
	float c = 1.0; \
	float s = 0.0; \
	if (n > 1.5) { \
		float k = floor(rotation / 90.0 + 0.5); \
		k = k - 4.0 * floor(k / 4.0); \
		if (k > 0.5 && k < 1.5) { c = 0.0; s = 1.0; } \
		else if (k > 1.5 && k < 2.5) { c = -1.0; s = 0.0; } \
		else if (k > 2.5) { c = 0.0; s = -1.0; } \
	} \
	else { \
		float a = rotation * 0.017453292519943295; \
		c = cos(a); \
		s = sin(a); \
	} \
	float p = ((u - 0.5) * c + (v - 0.5) * s) / (abs(c) + abs(s)) + 0.5 + offset; \
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
	GRADIENT_GENERATOR_INPUT_VECTOR, // Last, like Tile UV: the older saves keep their indices
	GRADIENT_GENERATOR_INPUT_COUNT,
};

// Node button indices (settings), all in the General section
enum {
	GRADIENT_GENERATOR_BUTTON_MODE,
	GRADIENT_GENERATOR_BUTTON_SHAPE,
	GRADIENT_GENERATOR_BUTTON_MIRROR,
	GRADIENT_GENERATOR_BUTTON_INVERT,
	GRADIENT_GENERATOR_BUTTON_WRAP, // Appended last: the indices of the older settings stay
	GRADIENT_GENERATOR_BUTTON_COUNT,
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
// Softness for the Cone only. Invert, Wrap Edges and the Vector input are always shown. A linked socket still shows (ui_node_item_hidden).
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

// The Tiling as the shader rounds it: a whole number, at least 1
static i32 gradient_generator_node_tiling_count(f32 tiling) {
	i32 n = (i32)floorf(tiling + 0.5f);
	return n < 1 ? 1 : n;
}

// The Rotation to store for a Linear gradient: the nearest multiple of 90 (0, 90, 180, 270) while the Tiling is 2 or more, else the Rotation as it is
// (the shader snaps the same way, see gradient_linear)
static f32 gradient_generator_node_settled_rotation(f32 rotation, f32 tiling) {
	if (gradient_generator_node_tiling_count(tiling) < 2) {
		return rotation;
	}
	i32 k = (i32)floorf(rotation / 90.0f + 0.5f);
	return (f32)(((k % 4) + 4) % 4) * 90.0f;
}

static bool gradient_generator_node_input_linked(ui_node_canvas_t *canvas, ui_node_t *node, i32 input) {
	for (i32 i = 0; i < canvas->links->length; ++i) {
		ui_node_link_t *l = canvas->links->buffer[i];
		if (l->to_id == node->id && l->to_socket == input) {
			return true;
		}
	}
	return false;
}

// Writes the snapped Rotation back so the slider shows the angle that is drawn: Linear mode, Rotation and Tiling both unlinked (a linked one is snapped
// by the shader only), Tiling 2 or more. Called while the node is laid out (ui_nodes_settle_rules), so also a node of an old save or one set by a script.
// Writes only when the value changes (one write per edit, no undo or dirty traffic per frame): while the slider is dragged the value jumps in steps of 90.
static void gradient_generator_node_settle(ui_node_t *node, ui_node_canvas_t *canvas) {
	if (node->inputs->length <= GRADIENT_GENERATOR_INPUT_TILING || (i32)make_image_node_button(node, GRADIENT_GENERATOR_BUTTON_MODE) != GRADIENT_GENERATOR_MODE_LINEAR) {
		return;
	}
	if (gradient_generator_node_input_linked(canvas, node, GRADIENT_GENERATOR_INPUT_ROTATION) || gradient_generator_node_input_linked(canvas, node, GRADIENT_GENERATOR_INPUT_TILING)) {
		return;
	}
	f32_array_t *rotation = node->inputs->buffer[GRADIENT_GENERATOR_INPUT_ROTATION]->default_value;
	f32          settled  = gradient_generator_node_settled_rotation(rotation->buffer[0], node->inputs->buffer[GRADIENT_GENERATOR_INPUT_TILING]->default_value->buffer[0]);
	if (settled != rotation->buffer[0]) {
		rotation->buffer[0] = settled;
	}
}

// The input as a shader value: a literal, or per pixel when linked
static char *gradient_generator_node_input(ui_node_t *node, i32 input) {
	if (input >= node->inputs->length) {
		return "0.0"; // A node saved with fewer inputs (the safety net)
	}
	return parser_material_parse_value_input(node->inputs->buffer[input], false);
}

// The gradient value (before Invert) for the coordinate uv (a float2 expression: wrapped or raw, see make_image_node_coord)
static char *gradient_generator_node_value_at(ui_node_t *node, char *uv, char *uv_unwrapped, char *store) {
	if ((i32)make_image_node_button(node, GRADIENT_GENERATOR_BUTTON_MODE) == GRADIENT_GENERATOR_MODE_RADIAL) {
		return string_tmp("gradient_radial(%s.x, %s.y, %s, %s, %s, %s, %s)", uv, uv, gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_CENTER_X),
		                  gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_CENTER_Y), gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_RADIUS),
		                  gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_SOFTNESS),
		                  make_image_node_button(node, GRADIENT_GENERATOR_BUTTON_SHAPE) > 0.5 ? "1.0" : "0.0");
	}
	return string_tmp("gradient_linear(%s.x, %s.y, %s, %s, %s, %s, %s)", uv, uv, gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_ROTATION),
	                  gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_TILING), gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_OFFSET),
	                  gradient_generator_node_input(node, GRADIENT_GENERATOR_INPUT_SOFTNESS),
	                  make_image_node_button(node, GRADIENT_GENERATOR_BUTTON_MIRROR) > 0.5 ? "1.0" : "0.0");
}

// The gradient per pixel on the coordinate of the Vector input, the same value in R, G and B. Invert after the triplanar blend (the weights add up to 1)
char *gradient_generator_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	node_shader_add_function(parser_material_kong, str_gradient_generator);
	bool  wrap  = make_image_node_button(node, GRADIENT_GENERATOR_BUTTON_WRAP) != 0.0f;
	char *value = make_image_node_eval_coord(node, GRADIENT_GENERATOR_INPUT_VECTOR, wrap, string_tmp("%s_gradient", parser_material_store_var_name(node)),
	                                         gradient_generator_node_value_at);
	return string_tmp("gradient_out3(%s, %s)", value, make_image_node_button(node, GRADIENT_GENERATOR_BUTTON_INVERT) > 0.5 ? "1.0" : "0.0");
}

// The sections of the node: General (the settings) and Parameters (the inputs). No Texture section: the node has no baked texture.
static void gradient_generator_node_sections(ui_node_t *def) {
	ui_node_section_t *list = calloc(3, sizeof(ui_node_section_t)); // The list ends with the zeroed entry
	list[0]                 = (ui_node_section_t){.title = _tr("General"), .kind = UI_NODE_ITEM_BUTTON, .first = 0, .count = def->buttons->length, .closed_by_default = false};
	list[1]                 = (ui_node_section_t){.title = _tr("Parameters"), .kind = UI_NODE_ITEM_INPUT, .first = 0, .count = def->inputs->length, .closed_by_default = false};
	any_map_set(ui_nodes_sections, def->type, list);
}

void gradient_generator_node_init() {
	u32 color_socket = NODES_MATERIAL_RGBA_COLOR;
	u32 value_color  = NODES_MATERIAL_VALUE_COLOR;

	ui_node_t *gradient_generator_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Gradient Generator"),
	                .type   = "GRADIENT_GENERATOR",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_PASS_THROUGH_COLOR, // A pass-through node, not an orange image node
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Rotation"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 360.0, 10),
	                        make_image_node_socket(_tr("Tiling"), "VALUE", value_color, f32_array_create_x(1.0), 1.0, 64.0, 1),
	                        make_image_node_socket(_tr("Offset"), "VALUE", value_color, f32_array_create_x(0.0), -1.0, 1.0, 100),
	                        make_image_node_socket(_tr("Center X"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Center Y"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Radius"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 2.0, 100),
	                        make_image_node_socket(_tr("Softness"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_vector_socket(),
	                    },
	                    GRADIENT_GENERATOR_INPUT_COUNT),
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
	                        make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                    },
	                    GRADIENT_GENERATOR_BUTTON_COUNT),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, gradient_generator_node_def);
	any_map_set(parser_material_node_vectors, "GRADIENT_GENERATOR", gradient_generator_node_vector);
	any_map_set(ui_nodes_hidden_rules, "GRADIENT_GENERATOR", gradient_generator_node_item_hidden);
	any_map_set(ui_nodes_settle_rules, "GRADIENT_GENERATOR", gradient_generator_node_settle);
	gradient_generator_node_sections(gradient_generator_node_def);
}
