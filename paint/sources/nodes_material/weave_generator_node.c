#include "../global.h"

// Weave Generator (Pattern menu): a woven block pattern as a height map (cloth, wicker, basket, mesh, carbon fibre). A pass-through shader node
// like the Gradient and Shape Generators: the pattern is a closed form function of the texture coordinate (str_weave_generator below), computed per
// pixel in the material shader: no baked result, no Size, no state row, no Texture section, resolution independent, every number input linkable per
// pixel. Fork only (never proposed upstream). Spec: armorpaint-admin specs/image-nodes/WEAVE_GENERATOR_PROPOSAL.md (decisions of 2026-11-23).
//
// Outputs (all grey VALUE): Height (0 = black corner squares, the grey of the thread underneath in a gap line, up to 1 on a ribbon), Mask (1 on a ribbon, 0 in a gap) and ID (one
// value per ribbon, 0..1, a hash of the ribbon's start cell and orientation, constant over the whole ribbon and its gaps, so it can drive a Color Ramp or a Tile UV).
// Inputs (Parameters): Tile X, Tile Y (blocks per axis, rounded per pixel, 1..20), Shape (0 flat ribbon .. 1 round ribbon), Weave (ribbons per block across, = the cells of a block
// per axis, rounded per pixel, 1..10), Gap (0..1, the thickness of a gap line in cells), then the Vector coordinate input LAST. Settings (General): Non Square Expansion (off) and
// Wrap Edges (on). The coordinate comes from make_image_node_coord (the rule of all coordinate generators).
//
// The structure (read off the reference picture, spec section 8; ALL formulas are in the shader text, the tuning constants at its top): the lattice has Tile X * Weave by
// Tile Y * Weave cells; a ribbon is Weave cells long and ONE cell wide; the ribbons form a diagonal basket: the cell (cx, cy) is horizontal when (cx - cy) mod 2 Weave is 0 or
// above Weave and vertical otherwise (Weave 1: the plain weave checker; the reference frame is Tile 2 x 2, Weave 2). Gap lines (a Gap thick band centred on a cell edge, the grey of
// the thread underneath) run only along the edges between two different ribbons; where gap lines meet there is a black square. Both ends of a ribbon are shaded where it dives
// under its neighbours: the under end deeper and longer, the over end lightly; the under end alternates with the ribbon number. Across a ribbon Shape goes from flat to round and
// lengthens the end shades. Non Square Expansion gives both gap bands the same physical thickness and keeps the end shades in proportion to the ribbon width when Tile X != Tile Y.
//
// Anti-aliasing (decision D9): the pixel is the screen space uv derivative as in the Shape Generator. The gap bands are filtered by the area they cover in one pixel, and when the
// cells are smaller than a pixel the pattern fades to its mean value (closed form per output).
// Tiling: Tile X and Tile Y are whole numbers, so the unit square repeats exactly (Wrap Edges on); the weave itself closes around the repeat only when Tile X and Tile Y are even (its
// period is 2 Weave cells; an odd count leaves one flipped row or column in a tiled copy).

// The shader (kong). Written in the common subset of kong and C (plain float functions, no ints, no ternary) so that the host test compiles this very
// text. Units: u and v are the (wrapped) texture coordinate, px is the uv size of one pixel.
// weave_eval(which, ...): which 0 = Height, 1 = Mask, 2 = ID.
char *str_weave_generator = "\
float weave_gap_level() { return 0.5; } \
float weave_under_depth() { return 0.35; } \
float weave_over_depth() { return 0.12; } \
float weave_under_len(float shape) { return 0.2 + 0.5 * shape; } \
float weave_over_len(float shape) { return 0.1 + 0.25 * shape; } \
float weave_sharpness(float shape) { float q = 1.0 - shape; return 1.0 + 24.0 * q * q; } \
float weave_fin(float v, float fb) { \
	if (v != v) { return fb; } \
	return v; \
} \
float weave_count(float v, float lo, float hi) { return clamp(floor(weave_fin(v, lo) + 0.5), lo, hi); } \
float weave_mod(float a, float m) { return a - m * floor(a / m); } \
float weave_smooth(float t) { \
	float c = clamp(t, 0.0, 1.0); \
	return c * c * (3.0 - 2.0 * c); \
} \
float weave_across(float t, float k) { return clamp(k * sin(3.141592653589793 * clamp(t, 0.0, 1.0)), 0.0, 1.0); } \
float weave_overlap(float dd, float gw) { \
	float lo = max(dd - 0.5, 0.0 - 0.5 * gw); \
	float hi = min(dd + 0.5, 0.5 * gw); \
	return clamp(hi - lo, 0.0, 1.0); \
} \
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
float weave_end_mean(float len, float lt) { \
	float x = min(lt / max(len, 0.0001), 1.0); \
	return len * (x - x * x * x + 0.5 * x * x * x * x) / max(lt, 0.0001); \
} \
float weave_mean_height(float n, float gta, float gtc, float ex, float s) { \
	float lt = max(n - gta, 0.0); \
	float along = 1.0 - weave_under_depth() * weave_end_mean(weave_under_len(s) * ex, lt) - weave_over_depth() * weave_end_mean(weave_over_len(s) * ex, lt); \
	return lt * (1.0 - gtc) / n * weave_mean_across(weave_sharpness(s)) * along; \
} \
float weave_eval(float which, float u, float v, float tx_in, float ty_in, float shape_in, float weave_in, float gap_in, float nse, float px_in) { \
	float tx = weave_count(tx_in, 1.0, 20.0); \
	float ty = weave_count(ty_in, 1.0, 20.0); \
	float n = weave_count(weave_in, 1.0, 10.0); \
	float s = clamp(weave_fin(shape_in, 0.5), 0.0, 1.0); \
	float g = clamp(weave_fin(gap_in, 0.1), 0.0, 1.0); \
	float pix = clamp(weave_fin(px_in, 0.0009765625), 0.0000152587890625, 1.0); \
	float x = clamp(weave_fin(u, 0.0), -1000000.0, 1000000.0) * tx * n; \
	float y = clamp(weave_fin(v, 0.0), -1000000.0, 1000000.0) * ty * n; \
	float cx = floor(x); \
	float cy = floor(y); \
	float fx = x - cx; \
	float fy = y - cy; \
	float cw = 1.0 / (tx * n); \
	float ch = 1.0 / (ty * n); \
	float d = weave_mod(cx - cy, 2.0 * n); \
	float horiz = 1.0; \
	float k = n - 1.0; \
	if (d > n + 0.5) { k = d - n - 1.0; } \
	if (d > 0.5 && d < n + 0.5) { horiz = 0.0; k = n - d; } \
	float detail = clamp(1.0 / (n * max(tx, ty) * pix) - 1.0, 0.0, 1.0); \
	float sx = cx - k * horiz; \
	float sy = cy - k * (1.0 - horiz); \
	if (which > 1.5) { return 0.5 + (weave_hash(sx, sy, 1.0 - horiz) - 0.5) * detail; } \
	float gx = g; \
	float gy = g; \
	if (nse > 0.5) { gx = g * min(1.0, ch / cw); gy = g * min(1.0, cw / ch); } \
	float act_l = 1.0; \
	float act_r = 1.0; \
	float act_t = 1.0; \
	float act_b = 1.0; \
	if (horiz > 0.5) { act_l = 1.0 - min(k, 1.0); act_r = 1.0 - min(n - 1.0 - k, 1.0); } \
	if (horiz < 0.5) { act_t = 1.0 - min(k, 1.0); act_b = 1.0 - min(n - 1.0 - k, 1.0); } \
	float ox_l = weave_overlap(fx * cw / pix, gx * cw / pix); \
	float ox_r = weave_overlap((1.0 - fx) * cw / pix, gx * cw / pix); \
	float oy_t = weave_overlap(fy * ch / pix, gy * ch / pix); \
	float oy_b = weave_overlap((1.0 - fy) * ch / pix, gy * ch / pix); \
	float cover = clamp(1.0 - act_l * ox_l - act_r * ox_r, 0.0, 1.0) * clamp(1.0 - act_t * oy_t - act_b * oy_b, 0.0, 1.0); \
	float black = min((ox_l + ox_r) * (oy_t + oy_b), 1.0 - cover); \
	float grey = 1.0 - cover - black; \
	float tm = 0.5 * ((n - gx) * (1.0 - gy) + (1.0 - gx) * (n - gy)) / n; \
	float mean = tm; \
	float value = cover; \
	if (which < 0.5) { \
		float fa = fx; \
		float fc = fy; \
		float gta = gx; \
		float gtc = gy; \
		float exh = 1.0; \
		float exv = 1.0; \
		if (nse > 0.5) { exh = ch / cw; exv = cw / ch; } \
		float ex = exh; \
		float idx = cy; \
		if (horiz < 0.5) { fa = fy; fc = fx; gta = gy; gtc = gx; ex = exv; idx = cx; } \
		float lt = max(n - gta, 0.0001); \
		float tp = clamp((fc - 0.5 * gtc) / max(1.0 - gtc, 0.0001), 0.0, 1.0); \
		float ap = clamp((k + fa - 0.5 * gta) / lt, 0.0, 1.0); \
		float odd = weave_mod(idx, 2.0); \
		float du = ap * lt; \
		float dov = (1.0 - ap) * lt; \
		if (odd > 0.5) { du = dov; dov = ap * lt; } \
		float along = 1.0 - weave_under_depth() * (1.0 - weave_smooth(du / (weave_under_len(s) * ex))) - weave_over_depth() * (1.0 - weave_smooth(dov / (weave_over_len(s) * ex))); \
		value = cover * weave_across(tp, weave_sharpness(s)) * along + grey * weave_gap_level(); \
		mean = 0.5 * (weave_mean_height(n, gx, gy, exh, s) + weave_mean_height(n, gy, gx, exv, s)) + (1.0 - tm - gx * gy) * weave_gap_level(); \
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
