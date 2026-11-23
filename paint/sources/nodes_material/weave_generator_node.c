#include "../global.h"

// Weave Generator (Pattern menu): a woven block pattern as a height map (cloth, wicker, basket, mesh, carbon fibre). A pass-through shader node
// like the Gradient and Shape Generators: the pattern is a closed form function of the texture coordinate (str_weave_generator below), computed per
// pixel in the material shader: no baked result, no Size, no state row, no Texture section, resolution independent, every number input linkable per
// pixel. Fork only (never proposed upstream). Spec: armorpaint-admin specs/image-nodes/WEAVE_GENERATOR_PROPOSAL.md (decisions of 2026-11-23).
//
// Outputs (all grey VALUE): Height (0 = gap, 1 = top of a thread), Mask (1 on a thread, 0 in a gap) and ID (one value per thread, 0..1, a hash of the
// block and thread index, constant over the whole strip of the thread including its half gaps, so it can drive a Color Ramp or a Tile UV).
// Inputs (Parameters): Tile X, Tile Y (blocks per axis, rounded per pixel, 1..20), Shape (0 flat ribbon .. 1 round thread), Weave (parallel threads
// per block, rounded per pixel, 1..10), Gap (0..1, the share of a thread strip left empty), then the Vector coordinate input LAST. Settings
// (General): Non Square Expansion (off) and Wrap Edges (on). The coordinate comes from make_image_node_coord (the rule of all coordinate generators).
//
// The geometry in short (the exact formulas are in the shader text, ALL of them in that one place, the tuning constants at its top):
//   blocks in a checker, horizontal threads in even blocks (bx + by even) and vertical threads in odd ones; Weave threads side by side per block;
//   Gap cuts a gap of Gap * strip width between threads (across) and a gap of Gap * thread width at both ends of a thread (along);
//   across a thread the height is a flat ribbon (Shape 0) .. a half cosine round thread (Shape 1); along a thread it is an asymmetric ramp from a
//   dark "under" level to 1 ("over", a sharp edge), whose length grows with Shape; the under end alternates with the checker;
//   Non Square Expansion makes the along features (end gap, ramp) keep their proportion to the thread width when Tile X != Tile Y.
//
// Anti-aliasing (decision D9): the pixel is the screen space uv derivative as in the Shape Generator. The gap edges are filtered by the area the gap
// covers in one pixel, and when a thread is thinner than a pixel the pattern fades to its mean value (closed form per output).
// Tiling: Tile X and Tile Y are whole numbers, so the unit square repeats exactly (Wrap Edges on); the over/under checker is consistent across the
// repeat only when Tile X and Tile Y are even (an odd count cannot be two coloured around a repeat: one row or column of the repeat joins two
// blocks of the same direction).

// The shader (kong). Written in the common subset of kong and C (plain float functions, no ints, no ternary) so that the host test compiles this very
// text. Units: u and v are the (wrapped) texture coordinate, px is the uv size of one pixel.
// weave_eval(which, ...): which 0 = Height, 1 = Mask, 2 = ID.
char *str_weave_generator = "\
float weave_under_level() { return 0.35; } \
float weave_ramp_widths(float shape) { return 0.15 + 0.6 * shape; } \
float weave_sharpness(float shape) { float q = 1.0 - shape; return 1.0 + 24.0 * q * q; } \
float weave_fin(float v, float fb) { \
	if (v != v) { return fb; } \
	return v; \
} \
float weave_count(float v, float lo, float hi) { return clamp(floor(weave_fin(v, lo) + 0.5), lo, hi); } \
float weave_smooth(float t) { \
	float c = clamp(t, 0.0, 1.0); \
	return c * c * (3.0 - 2.0 * c); \
} \
float weave_across(float t, float k) { return clamp(k * sin(3.141592653589793 * clamp(t, 0.0, 1.0)), 0.0, 1.0); } \
float weave_gap_overlap(float dd, float gw) { \
	float lo = max(dd - 0.5, 0.0 - 0.5 * gw); \
	float hi = min(dd + 0.5, 0.5 * gw); \
	return clamp(hi - lo, 0.0, 1.0); \
} \
float weave_gap_cover(float dd, float gw, float span) { return clamp(1.0 - weave_gap_overlap(dd, gw) - weave_gap_overlap(span - dd, gw), 0.0, 1.0); } \
float weave_hash(float x, float y, float z) { \
	float a = x * 0.1031 - floor(x * 0.1031); \
	float b = y * 0.1030 - floor(y * 0.1030); \
	float c = z * 0.0973 - floor(z * 0.0973); \
	float d = a * (c + 31.32) + b * (b + 31.32) + c * (a + 31.32); \
	a = a + d - floor(a + d); \
	b = b + d - floor(b + d); \
	c = c + d - floor(c + d); \
	float h = (a + b) * c * 13.7 + a * 5.3; \
	return h - floor(h); \
} \
float weave_mean_across(float k) { \
	float x0 = asin(min(1.0, 1.0 / k)) / 3.141592653589793; \
	return 1.0 - 2.0 * x0 + 2.0 * k * (1.0 - cos(3.141592653589793 * x0)) / 3.141592653589793; \
} \
float weave_thread_len(float w, float aspect, float nse) { \
	if (nse > 0.5) { return w * aspect; } \
	return w; \
} \
float weave_mean_height(float w, float aspect, float nse, float g, float s) { \
	float tl = weave_thread_len(w, aspect, nse); \
	float ge = min(g / tl, 1.0); \
	float lr = clamp(weave_ramp_widths(s) / tl, 0.0001, 1.0); \
	float under = weave_under_level(); \
	return (1.0 - ge) * weave_mean_across(weave_sharpness(s)) * (under + (1.0 - under) * (1.0 - 0.5 * lr)); \
} \
float weave_eval(float which, float u, float v, float tx_in, float ty_in, float shape_in, float weave_in, float gap_in, float nse, float px_in) { \
	float tx = weave_count(tx_in, 1.0, 20.0); \
	float ty = weave_count(ty_in, 1.0, 20.0); \
	float w = weave_count(weave_in, 1.0, 10.0); \
	float s = clamp(weave_fin(shape_in, 0.5), 0.0, 1.0); \
	float g = clamp(weave_fin(gap_in, 0.1), 0.0, 1.0); \
	float pix = clamp(weave_fin(px_in, 0.0009765625), 0.0000152587890625, 1.0); \
	float x = clamp(weave_fin(u, 0.0), -1000000.0, 1000000.0) * tx; \
	float y = clamp(weave_fin(v, 0.0), -1000000.0, 1000000.0) * ty; \
	float bx = floor(x); \
	float by = floor(y); \
	float parity = (bx + by) - 2.0 * floor((bx + by) * 0.5); \
	float a = x - bx; \
	float c = y - by; \
	float ua = 1.0 / tx; \
	float uc = 1.0 / ty; \
	float aspect = ty / tx; \
	if (parity > 0.5) { \
		a = y - by; \
		c = x - bx; \
		ua = 1.0 / ty; \
		uc = 1.0 / tx; \
		aspect = tx / ty; \
	} \
	float cw = c * w; \
	float ti = floor(cw); \
	float t = cw - ti; \
	float su = uc / w; \
	float detail = clamp(1.0 / (w * max(tx, ty) * pix) - 1.0, 0.0, 1.0); \
	if (which > 1.5) { return 0.5 + (weave_hash(bx, by, ti) - 0.5) * detail; } \
	float thread_len = weave_thread_len(w, aspect, nse); \
	float ge = min(g / thread_len, 1.0); \
	float dd = min(t, 1.0 - t) * su / pix; \
	float cover = weave_gap_cover(dd, g * su / pix, su / pix); \
	float da = min(a, 1.0 - a) * ua / pix; \
	cover = cover * weave_gap_cover(da, ge * ua / pix, ua / pix); \
	float value = cover; \
	float ah = ty / tx; \
	float av = tx / ty; \
	float ge_h = min(g / weave_thread_len(w, ah, nse), 1.0); \
	float ge_v = min(g / weave_thread_len(w, av, nse), 1.0); \
	float mean = (1.0 - g) * (1.0 - 0.5 * (ge_h + ge_v)); \
	if (which < 0.5) { \
		float k = weave_sharpness(s); \
		float lr = clamp(weave_ramp_widths(s) / thread_len, 0.0001, 1.0); \
		float under = weave_under_level(); \
		float tp = clamp((t - 0.5 * g) / max(1.0 - g, 0.0001), 0.0, 1.0); \
		float ap = clamp((a - 0.5 * ge) / max(1.0 - ge, 0.0001), 0.0, 1.0); \
		if (parity > 0.5) { ap = 1.0 - ap; } \
		float ramp = under + (1.0 - under) * weave_smooth(ap / lr); \
		value = cover * weave_across(tp, k) * ramp; \
		mean = (1.0 - g) * 0.5 * (weave_mean_height(w, ah, nse, g, s) + weave_mean_height(w, av, nse, g, s)); \
	} \
	return mean + (value - mean) * detail; \
} \
";

// Input socket indices
enum {
	WEAVE_GENERATOR_INPUT_TILE_X = 0,
	WEAVE_GENERATOR_INPUT_TILE_Y = 1,
	WEAVE_GENERATOR_INPUT_SHAPE  = 2,
	WEAVE_GENERATOR_INPUT_WEAVE  = 3,
	WEAVE_GENERATOR_INPUT_GAP    = 4,
	WEAVE_GENERATOR_INPUT_VECTOR = 5, // Last, like the other pass-through generators; not a number input
	WEAVE_GENERATOR_INPUT_COUNT  = 6,
};

// Node button indices (settings, the General section)
enum {
	WEAVE_GENERATOR_BUTTON_NON_SQUARE = 0,
	WEAVE_GENERATOR_BUTTON_WRAP       = 1,
	WEAVE_GENERATOR_BUTTON_COUNT      = 2,
};

// Output socket indices: the value of weave_eval(which, ...) is the index
enum {
	WEAVE_GENERATOR_OUTPUT_HEIGHT = 0,
	WEAVE_GENERATOR_OUTPUT_MASK   = 1,
	WEAVE_GENERATOR_OUTPUT_ID     = 2,
	WEAVE_GENERATOR_OUTPUT_COUNT  = 3,
};

// The uv size of one pixel (the Shape Generator rule, see there): the screen space derivative of the UNWRAPPED coordinate, written once per projection;
// outside the fragment stage (no derivatives) 1 / 1024
static char *weave_generator_node_pixel(char *store, char *uv) {
	if (!parser_material_is_frag) {
		return "0.0009765625";
	}
	parser_material_write(parser_material_kong, string_tmp("float %s_px = max(abs(ddx(%s.x)) + abs(ddy(%s.x)), abs(ddx(%s.y)) + abs(ddy(%s.y)));", store, uv, uv, uv, uv));
	return string_tmp("%s_px", store);
}

// The output being evaluated (value_at of make_image_node_eval_coord has no argument for it); the parser is single threaded
static i32 weave_generator_node_output = 0;

// The value of the output for the coordinate uv of one projection: the inputs and settings as arguments of weave_eval
static char *weave_generator_node_value_at(ui_node_t *node, char *uv, char *uv_unwrapped, char *store) {
	char *in[WEAVE_GENERATOR_INPUT_VECTOR];
	for (i32 i = 0; i < WEAVE_GENERATOR_INPUT_VECTOR; ++i) {
		in[i] = i < node->inputs->length ? parser_material_parse_value_input(node->inputs->buffer[i], false) : "0.0";
	}
	char *non_square = make_image_node_button(node, WEAVE_GENERATOR_BUTTON_NON_SQUARE) != 0.0 ? "1.0" : "0.0";
	char *px         = weave_generator_node_pixel(store, uv_unwrapped);
	return string_tmp("weave_eval(%d.0, %s.x, %s.y, %s, %s, %s, %s, %s, %s, %s)", weave_generator_node_output, uv, uv, in[WEAVE_GENERATOR_INPUT_TILE_X],
	                  in[WEAVE_GENERATOR_INPUT_TILE_Y], in[WEAVE_GENERATOR_INPUT_SHAPE], in[WEAVE_GENERATOR_INPUT_WEAVE], in[WEAVE_GENERATOR_INPUT_GAP], non_square, px);
}

// The float expression of one output (a stored variable, written once per output): one evaluation per projection under triplanar for an unlinked Vector
static char *weave_generator_node_output_value(ui_node_t *node, ui_node_socket_t *socket) {
	node_shader_add_function(parser_material_kong, str_weave_generator);
	i32 output = WEAVE_GENERATOR_OUTPUT_HEIGHT;
	for (i32 i = 0; i < node->outputs->length; ++i) {
		if (node->outputs->buffer[i] == socket) {
			output = i;
		}
	}
	weave_generator_node_output = output;
	bool wrap                   = make_image_node_button(node, WEAVE_GENERATOR_BUTTON_WRAP) != 0.0;
	return make_image_node_eval_coord(node, WEAVE_GENERATOR_INPUT_VECTOR, wrap, string_tmp("%s_weave%d", parser_material_store_var_name(node), output), weave_generator_node_value_at);
}

char *weave_generator_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	char *v = weave_generator_node_output_value(node, socket);
	return string_tmp("float3(%s, %s, %s)", v, v, v);
}

char *weave_generator_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return weave_generator_node_output_value(node, socket);
}

// The sections of the node: General (the settings) and Parameters (all inputs). No Texture section: the node has no baked texture.
static void weave_generator_node_sections(ui_node_t *def) {
	ui_node_section_t *list = calloc(3, sizeof(ui_node_section_t)); // The list ends with the zeroed entry
	list[0] = (ui_node_section_t){.title = _tr("General"), .kind = UI_NODE_ITEM_BUTTON, .first = 0, .count = def->buttons->length, .closed_by_default = false};
	list[1] = (ui_node_section_t){.title = _tr("Parameters"), .kind = UI_NODE_ITEM_INPUT, .first = 0, .count = def->inputs->length, .closed_by_default = false};
	any_map_set(ui_nodes_sections, def->type, list);
}

void weave_generator_node_init() {
	u32 value_color = NODES_MATERIAL_VALUE_COLOR;

	ui_node_t *weave_generator_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Weave Generator"),
	                .type   = "WEAVE_GENERATOR",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_PASS_THROUGH_COLOR, // A pass-through node, not an orange image node
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Tile X"), "VALUE", value_color, f32_array_create_x(4.0), 1.0, 20.0, 1),
	                        make_image_node_socket(_tr("Tile Y"), "VALUE", value_color, f32_array_create_x(4.0), 1.0, 20.0, 1),
	                        make_image_node_socket(_tr("Shape"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Weave"), "VALUE", value_color, f32_array_create_x(2.0), 1.0, 10.0, 1),
	                        make_image_node_socket(_tr("Gap"), "VALUE", value_color, f32_array_create_x(0.1), 0.0, 1.0, 100),
	                        make_image_node_vector_socket(),
	                    },
	                    WEAVE_GENERATOR_INPUT_COUNT),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Height"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Mask"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("ID"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                    },
	                    WEAVE_GENERATOR_OUTPUT_COUNT),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Non Square Expansion"), "BOOL", 0, NULL),
	                        make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                    },
	                    WEAVE_GENERATOR_BUTTON_COUNT),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, weave_generator_node_def);
	any_map_set(parser_material_node_values, "WEAVE_GENERATOR", weave_generator_node_value);
	any_map_set(parser_material_node_vectors, "WEAVE_GENERATOR", weave_generator_node_vector);
	weave_generator_node_sections(weave_generator_node_def);
}
