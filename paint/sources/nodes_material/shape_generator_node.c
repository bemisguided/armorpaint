#include "../global.h"

// Shape Generator (Pattern menu): one shape (circle, square, rectangle) as a grey mask, 1 inside and 0 outside. A pass-through shader
// node like Gradient Texture and Map Range: the mask is computed per pixel in the material shader (str_shape_generator below), no baked
// result, no Size, no state row. Every number input (Width, Height, Position X/Y, Rotation, Corner Radius, Edge Softness) can be linked and
// is evaluated per pixel; Shape, Invert and Wrap Edges are settings. The node has no Image input and no Vector input: it draws over the
// tex_coord of whatever evaluates it (the uv of the mesh in a material, the image uv of a capture when an image node reads it).
// Width and Height are the full extent of the shape as a fraction of the image (Circle: the two diameters of an ellipse,
// Square: Width only). Per pixel the signed distance to the shape in pixels gives the value 1 - smoothstep(-w, w, d),
// w = Edge Softness in pixels, at least 1 so the edge is antialiased. Wrap Edges adds up the coverage of the nine image
// copies of the shape (clamped to 1, so a shape as large as the image stays 1 on its border), so a shape that crosses the image border
// continues on the other side (the centre is reduced into the image first, more copies each way when the shape extent plus the softness
// reaches farther than one image). Wrap Edges off: the shape continues beyond the image where it covers the border (see
// shape_border_shift in the shader), so such a border texel is not an edge.
// Rotation follows the rotation convention of all image nodes (armorpaint-admin specs/image-nodes/ROTATION_CONVENTION.md):
// degrees, a positive angle turns the shape clockwise as seen in the editor view (y down), about the shape centre, after
// the Width and Height and before Position.
//
// The pixel: there is no image size here, so "one pixel" is the screen space derivative of the uv (decision D4 of the bake audit):
// px = the larger of |ddx| + |ddy| of tex_coord.x and of tex_coord.y, the uv size of one pixel of the surface being drawn (limited to
// 1/65536 .. 1). The shader works in units of that pixel (s = 1 / px is "the image size", x = u * s): Edge Softness, a fraction of the
// image as before, is w = Edge Softness * s, and the one pixel minimum of the antialiasing is w >= 1. An image node that reads this node
// captures it over a full screen triangle of Size x Size pixels, so there px is exactly 1 / Size and the picture is the one the baked node
// drew; on a mesh px is the uv size of a screen pixel, so the edge stays one screen pixel wide at any distance. Outside the fragment stage
// (no derivatives) px is 1 / 1024. Wrap Edges and the border rule reach the same texels as before through s.

// Input socket indices
enum {
	SHAPE_GENERATOR_INPUT_WIDTH         = 0,
	SHAPE_GENERATOR_INPUT_HEIGHT        = 1,
	SHAPE_GENERATOR_INPUT_POSITION_X    = 2,
	SHAPE_GENERATOR_INPUT_POSITION_Y    = 3,
	SHAPE_GENERATOR_INPUT_ROTATION      = 4,
	SHAPE_GENERATOR_INPUT_CORNER_RADIUS = 5,
	SHAPE_GENERATOR_INPUT_EDGE_SOFTNESS = 6,
	SHAPE_GENERATOR_INPUT_COUNT         = 7,
};

// Node button indices (settings)
enum {
	SHAPE_GENERATOR_BUTTON_SHAPE  = 0,
	SHAPE_GENERATOR_BUTTON_INVERT = 1,
	SHAPE_GENERATOR_BUTTON_WRAP   = 2,
	SHAPE_GENERATOR_BUTTON_COUNT  = 3,
};

enum {
	SHAPE_GENERATOR_CIRCLE    = 0,
	SHAPE_GENERATOR_SQUARE    = 1,
	SHAPE_GENERATOR_RECTANGLE = 2,
};

// What the shape uses: a Square takes its Height from Width, a Circle (an ellipse) has no corners. Rotation is used by every shape.
// A linked socket still shows (ui_node_item_hidden).
static bool shape_generator_node_input_hidden(i32 shape, i32 input) {
	return (input == SHAPE_GENERATOR_INPUT_HEIGHT && shape == SHAPE_GENERATOR_SQUARE) ||
	       (input == SHAPE_GENERATOR_INPUT_CORNER_RADIUS && shape == SHAPE_GENERATOR_CIRCLE);
}

static bool shape_generator_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	return kind == UI_NODE_ITEM_INPUT && shape_generator_node_input_hidden((i32)make_image_node_button(node, SHAPE_GENERATOR_BUTTON_SHAPE), index);
}

// The mask in the material shader (kong). Written in the common subset of kong and C (plain float functions, no ints, no ternary) so that the
// host test compiles this very text and compares it with the CPU function of the first (baked) build. Units: pixels, s = the image size in
// pixels (1 / px), the pixel (x, y) is the uv times s. shape: 0 circle, 1 square, 2 rectangle (floats, so no int conversions).
char *str_shape_generator = "\
float shape_fin(float v, float fb) { \
	if (v != v) { return fb; } \
	return v; \
} \
float shape_distance(float shape, float x, float y, float hx, float hy, float r) { \
	if (shape < 0.5) { \
		float nx = x / hx; \
		float ny = y / hy; \
		return (sqrt(nx * nx + ny * ny) - 1.0) * min(hx, hy); \
	} \
	float qx = abs(x) - hx + r; \
	float qy = abs(y) - hy + r; \
	float ox = max(qx, 0.0); \
	float oy = max(qy, 0.0); \
	float m = max(qx, qy); \
	return sqrt(ox * ox + oy * oy) + min(m, 0.0) - r; \
} \
float shape_frame_distance(float shape, float hx, float hy, float r, float cx, float cy, float ca, float sa, float s, float x, float y, float ox, float oy) { \
	float dx = x - (cx + ox * s); \
	float dy = y - (cy + oy * s); \
	float rx = dx * ca + dy * sa; \
	float ry = 0.0 - dx * sa + dy * ca; \
	return shape_distance(shape, rx, ry, hx, hy, r); \
} \
float shape_smooth_coverage(float di, float w) { \
	float t = clamp((di + w) / (2.0 * w), 0.0, 1.0); \
	return 1.0 - t * t * (3.0 - 2.0 * t); \
} \
float shape_border_shift(float shape, float hx, float hy, float r, float cx, float cy, float ca, float sa, float s, float x, float y, float horizontal) { \
	float a = y; \
	float b = x; \
	if (horizontal > 0.5) { a = x; b = y; } \
	float edge = 0.0; \
	float inward = 0.5; \
	if (a >= 1.0) { \
		if (a <= s - 1.0) { return 0.0; } \
		edge = s; \
		inward = 0.0 - 0.5; \
	} \
	for (float k = 0.0 - 1.0; k <= 1.0; k += 1.0) { \
		float across = b + k * 0.5; \
		float d = shape_frame_distance(shape, hx, hy, r, cx, cy, ca, sa, s, across, edge, 0.0, 0.0); \
		if (horizontal > 0.5) { d = shape_frame_distance(shape, hx, hy, r, cx, cy, ca, sa, s, edge, across, 0.0, 0.0); } \
		if (d > 0.001) { return 0.0; } \
	} \
	return inward; \
} \
float shape_coverage(float shape, float invert, float wrap, float width, float height, float position_x, float position_y, float rotation, float corner_radius, float softness, float u, float v, float px) { \
	float pix = clamp(shape_fin(px, 0.0009765625), 0.0000152587890625, 1.0); \
	float s = 1.0 / pix; \
	float x = u * s; \
	float y = v * s; \
	float hx = clamp(shape_fin(width, 0.5), 0.0, 1.0) * s * 0.5; \
	float hy = clamp(shape_fin(height, 0.5), 0.0, 1.0) * s * 0.5; \
	if (shape > 0.5 && shape < 1.5) { hy = hx; } \
	hx = max(hx, 0.0001); \
	hy = max(hy, 0.0001); \
	float r = 0.0; \
	if (shape > 0.5) { r = clamp(shape_fin(corner_radius, 0.0), 0.0, 0.5) * 2.0 * min(hx, hy); } \
	float cx = s * 0.5 + shape_fin(position_x, 0.0) * s; \
	float cy = s * 0.5 + shape_fin(position_y, 0.0) * s; \
	if (wrap > 0.5) { \
		cx = cx - floor(cx / s) * s; \
		cy = cy - floor(cy / s) * s; \
	} \
	float a = shape_fin(rotation, 0.0) * 0.017453292519943295; \
	float ca = cos(a); \
	float sa = sin(a); \
	float w = clamp(shape_fin(softness, 0.0), 0.0, 1.0) * s; \
	w = max(w, 1.0); \
	float hmin = min(hx, hy); \
	float hmax = max(hx, hy); \
	float reach = sqrt(hx * hx + hy * hy) + w; \
	if (shape < 0.5) { reach = hmax * (1.0 + w / hmin); } \
	float n = clamp(ceil(reach / s), 1.0, 4.0); \
	float lo = 0.0; \
	float hi = 0.0; \
	if (wrap > 0.5) { lo = 0.0 - n; hi = n; } \
	float val = 0.0; \
	for (float oy = lo; oy <= hi; oy += 1.0) { \
		for (float ox = lo; ox <= hi; ox += 1.0) { \
			val = val + shape_smooth_coverage(shape_frame_distance(shape, hx, hy, r, cx, cy, ca, sa, s, x, y, ox, oy), w); \
		} \
	} \
	val = min(val, 1.0); \
	if (wrap < 0.5 && (x < 1.0 || y < 1.0 || x > s - 1.0 || y > s - 1.0)) { \
		float d_own = shape_frame_distance(shape, hx, hy, r, cx, cy, ca, sa, s, x, y, 0.0, 0.0); \
		float sx = shape_border_shift(shape, hx, hy, r, cx, cy, ca, sa, s, x, y, 1.0); \
		float sy = shape_border_shift(shape, hx, hy, r, cx, cy, ca, sa, s, x, y, 0.0); \
		if (sx != 0.0 || sy != 0.0) { \
			float d_in = shape_frame_distance(shape, hx, hy, r, cx, cy, ca, sa, s, x + sx, y + sy, 0.0, 0.0); \
			if (d_in <= d_own - 0.499) { \
				val = max(val, shape_smooth_coverage(d_in, w)); \
			} \
		} \
	} \
	if (invert > 0.5) { return 1.0 - val; } \
	return val; \
} \
";

// The uv size of one pixel: see the comment at the top. Written once per projection as statements of the fragment, outside any branch.
static char *shape_generator_node_pixel(char *store, char *uv) {
	if (!parser_material_is_frag) {
		return "0.0009765625";
	}
	parser_material_write(parser_material_kong, string_tmp("float %s_px = max(abs(ddx(%s.x)) + abs(ddy(%s.x)), abs(ddx(%s.y)) + abs(ddy(%s.y)));", store, uv, uv, uv, uv));
	return string_tmp("%s_px", store);
}

// The mask of the node as a float expression (a stored variable, written once per node)
static char *shape_generator_node_mask(ui_node_t *node) {
	char *store = string_tmp("%s_shape", parser_material_store_var_name(node));
	if (string_array_index_of(parser_material_parsed, store) >= 0) {
		return store;
	}
	any_array_push(parser_material_parsed, store);
	node_shader_add_function(parser_material_kong, str_shape_generator);
	char *in[SHAPE_GENERATOR_INPUT_COUNT];
	for (i32 i = 0; i < SHAPE_GENERATOR_INPUT_COUNT; ++i) {
		in[i] = parser_material_parse_value_input(node->inputs->buffer[i], false);
	}
	i32   shape  = (i32)make_image_node_button(node, SHAPE_GENERATOR_BUTTON_SHAPE);
	char *invert = make_image_node_button(node, SHAPE_GENERATOR_BUTTON_INVERT) != 0.0 ? "1.0" : "0.0";
	char *wrap   = make_image_node_button(node, SHAPE_GENERATOR_BUTTON_WRAP) != 0.0 ? "1.0" : "0.0";
	if (parser_material_triplanar && !parser_material_image_space) { // Like Image Texture: one evaluation per projection, blended
		node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
		char *uvs[3] = {"tex_coord", "tex_coord1", "tex_coord2"};
		for (i32 k = 0; k < 3; ++k) {
			char *px = shape_generator_node_pixel(string_tmp("%s%d", store, k), uvs[k]);
			parser_material_write(parser_material_kong,
			                      string_tmp("float %s%d = shape_coverage(%d.0, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s.x, %s.y, %s);", store, k, shape, invert, wrap, in[0], in[1],
			                                 in[2], in[3], in[4], in[5], in[6], uvs[k], uvs[k], px));
		}
		parser_material_write(parser_material_kong, string_tmp("float %s = %s0 * tex_coord_blend.x + %s1 * tex_coord_blend.y + %s2 * tex_coord_blend.z;", store, store, store, store));
		return store;
	}
	if (!parser_material_image_space) {
		node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
	}
	char *px = shape_generator_node_pixel(store, "tex_coord");
	parser_material_write(parser_material_kong,
	                      string_tmp("float %s = shape_coverage(%d.0, %s, %s, %s, %s, %s, %s, %s, %s, %s, tex_coord.x, tex_coord.y, %s);", store, shape, invert, wrap, in[0], in[1],
	                                 in[2], in[3], in[4], in[5], in[6], px));
	return store;
}

char *shape_generator_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	char *v = shape_generator_node_mask(node);
	return string_tmp("float3(%s, %s, %s)", v, v, v);
}

char *shape_generator_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return shape_generator_node_mask(node);
}

// The sections of the node: General (the settings) and Parameters (all inputs). No Texture section: the node has no baked texture.
static void shape_generator_node_sections(ui_node_t *def) {
	ui_node_section_t *list = calloc(3, sizeof(ui_node_section_t)); // The list ends with the zeroed entry
	list[0] = (ui_node_section_t){.title = _tr("General"), .kind = UI_NODE_ITEM_BUTTON, .first = 0, .count = def->buttons->length, .closed_by_default = false};
	list[1] = (ui_node_section_t){.title = _tr("Parameters"), .kind = UI_NODE_ITEM_INPUT, .first = 0, .count = def->inputs->length, .closed_by_default = false};
	any_map_set(ui_nodes_sections, def->type, list);
}

void shape_generator_node_init() {
	u32 value_color = NODES_MATERIAL_VALUE_COLOR;

	ui_node_t *shape_generator_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Shape Generator"),
	                .type   = "SHAPE_GENERATOR",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_PASS_THROUGH_COLOR, // A pass-through node, not an orange image node
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
	                    SHAPE_GENERATOR_INPUT_COUNT),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Shape"), "ENUM", 0, "Circle\nSquare\nRectangle"),
	                        make_image_node_setting(_tr("Invert"), "BOOL", 0, NULL),
	                        make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                    },
	                    SHAPE_GENERATOR_BUTTON_COUNT),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, shape_generator_node_def);
	any_map_set(parser_material_node_values, "SHAPE_GENERATOR", shape_generator_node_value);
	any_map_set(parser_material_node_vectors, "SHAPE_GENERATOR", shape_generator_node_vector);
	any_map_set(ui_nodes_hidden_rules, "SHAPE_GENERATOR", shape_generator_node_item_hidden);
	shape_generator_node_sections(shape_generator_node_def);
	// The baked build (type TEX_SHAPE_GENERATOR, Size and a state row first) is resolved to this node by the node layout type alias in util_node_layout.c
	// (items matched by name: Width, Height, Position X/Y, Rotation, Corner Radius, Edge Softness, Shape, Invert, Wrap Edges keep their values and links,
	// Size and the state row are dropped); without the alias such a node loads as an unknown node with a black output.
}
