#include "../global.h"

// Weave Generator (Pattern menu): a woven block pattern as a height map (cloth, wicker, basket, mesh, carbon fibre). A pass-through shader node
// like the Gradient and Shape Generators: the pattern is a closed form function of the texture coordinate (str_weave_generator below), computed per
// pixel in the material shader: no baked result, no Size, no state row, no Texture section, resolution independent, every number input linkable per
// pixel. Fork only (never proposed upstream). Spec: armorpaint-admin specs/image-nodes/WEAVE_GENERATOR_PROPOSAL.md (decisions of 2026-11-23).
//
// Outputs (all grey VALUE): Height (0 = black corner squares, the grey of the thread underneath in a gap line, up to 1 on a ribbon), Mask (1 on a ribbon, 0 in a gap) and ID (one
// value per ribbon, 0..1, a hash of the ribbon's start cell and orientation, so it can drive a Color Ramp or a Tile UV).
// Inputs (Parameters, the order of the Substance Designer node): Tile X, Tile Y (1..20), Shape (0..1), Weave (1..10, rounded per pixel), Gap X, Gap Y (0..1), then the Vector coordinate
// input LAST. Setting (General): Wrap Edges (on). The coordinate comes from make_image_node_coord (the rule of all coordinate generators).
//
// The structure (measured on real Substance Designer outputs, spec section 8; ALL formulas are in the shader text, the tuning constants at its top): the lattice has 2 Tile Weave cells per
// axis; a ribbon is Weave cells long and ONE cell wide; the ribbons form a diagonal basket: the cell (cx, cy) is horizontal when (cx - cy) mod 2 Weave is 0 or above Weave and vertical
// otherwise (Weave 1: the plain weave checker). Two parallel neighbouring ribbons are separated by a grey gap line (Gap X, Gap Y: the thickness of a vertical, a horizontal line in cells, 0..1); where a ribbon END meets the side of a perpendicular one, the ending ribbon dives UNDER: it continues beneath the band and is shaded by a drop shadow
// (depth 0.5 at the edge, falling with the square of the distance, length 0.5 Shape Weave cells), the other ribbon is cut sharply at half a gap from the cell edge; a black square sits on every
// lattice vertex. Both ends of every ribbon are shaded alike.
//
// Anti-aliasing (decision D9): the pixel is the screen space uv derivative as in the Shape Generator. Ribbon edges are filtered by the area covered in one pixel, and when the cells are
// smaller than a pixel the pattern fades to its mean value (closed form per output).
// Tiling: the weave has period 2 Weave cells and the lattice 2 Tile Weave, so the unit square repeats seamlessly for EVERY Tile X and Tile Y.

// The shader (kong). Written in the common subset of kong and C (plain float functions, no ints, no ternary) so that the host test compiles this very
// text. Units: u and v are the (wrapped) texture coordinate, px is the uv size of one pixel.
// weave_eval(which, ...): which 0 = Height, 1 = Mask, 2 = ID.
char *str_weave_generator = "\
float weave_gap_level() { return 0.502; } \
float weave_shade_depth() { return 0.5; } \
float weave_shade_len(float shape, float n) { return 0.5 * shape * n; } \
float weave_fin(float v, float fb) { \
	if (v != v) { return fb; } \
	return v; \
} \
float weave_count(float v, float lo, float hi) { return clamp(floor(weave_fin(v, lo) + 0.5), lo, hi); } \
float weave_mod(float a, float m) { return a - m * floor(a / m); } \
float weave_ivl(float a, float b, float c, float d) { return max(0.0, min(b, d) - max(a, c)); } \
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
float weave_horiz(float cx, float cy, float n) { \
	float d = weave_mod(cx - cy, 2.0 * n); \
	if (d > 0.5 && d < n + 0.5) { return 0.0; } \
	return 1.0; \
} \
float weave_k(float cx, float cy, float n) { \
	float d = weave_mod(cx - cy, 2.0 * n); \
	float k = n - 1.0; \
	if (d > n + 0.5) { k = d - n - 1.0; } \
	if (d > 0.5 && d < n + 0.5) { k = n - d; } \
	return k; \
} \
float weave_id(float cx, float cy, float n) { \
	float h = weave_horiz(cx, cy, n); \
	float k = weave_k(cx, cy, n); \
	return weave_hash(cx - k * h, cy - k * (1.0 - h), 1.0 - h); \
} \
float weave_part(float sel, float cx, float cy, float fx, float fy, float hx, float hy, float n, float tx, float ty, float s) { \
	float horiz = weave_horiz(cx, cy, n); \
	float k = weave_k(cx, cy, n); \
	float first = 1.0 - min(k, 1.0); \
	float last = 1.0 - min(n - 1.0 - k, 1.0); \
	float xlo = 0.5 * tx; \
	float xhi = 1.0 - 0.5 * tx; \
	float ylo = 0.5 * ty; \
	float yhi = 1.0 - 0.5 * ty; \
	float ta = tx; \
	float fa = fx; \
	if (horiz > 0.5) { xlo = 0.0 - first * 0.5 * tx; xhi = 1.0 + last * 0.5 * tx; } \
	if (horiz < 0.5) { ylo = 0.0 - first * 0.5 * ty; yhi = 1.0 + last * 0.5 * ty; ta = ty; fa = fy; } \
	float cover = weave_ivl(fx - hx, fx + hx, xlo, xhi) / (2.0 * hx) * weave_ivl(fy - hy, fy + hy, ylo, yhi) / (2.0 * hy); \
	if (sel < 0.5) { return cover; } \
	float p = k + fa; \
	float len = weave_shade_len(s, n); \
	float es = 0.0; \
	float ee = 0.0; \
	if (len > 0.0001) { es = clamp(1.0 - (p + 0.5 * ta) / len, 0.0, 1.0); ee = clamp(1.0 - (n + 0.5 * ta - p) / len, 0.0, 1.0); } \
	return cover * (1.0 - weave_shade_depth() * (es * es + ee * ee)); \
} \
float weave_shade_mean(float len, float lr) { \
	float x = min(lr / max(len, 0.0001), 1.0); \
	float q = 1.0 - x; \
	return len * (1.0 - q * q * q) / (3.0 * lr); \
} \
float weave_eval(float which, float u, float v, float tx_in, float ty_in, float shape_in, float weave_in, float gx_in, float gy_in, float px_in) { \
	float tx = weave_count(tx_in, 1.0, 20.0); \
	float ty = weave_count(ty_in, 1.0, 20.0); \
	float n = weave_count(weave_in, 1.0, 10.0); \
	float s = clamp(weave_fin(shape_in, 0.5), 0.0, 1.0); \
	float pix = clamp(weave_fin(px_in, 0.0009765625), 0.0000152587890625, 1.0); \
	float x = clamp(weave_fin(u, 0.0), -1000000.0, 1000000.0) * tx * n * 2.0; \
	float y = clamp(weave_fin(v, 0.0), -1000000.0, 1000000.0) * ty * n * 2.0; \
	float cx = floor(x); \
	float cy = floor(y); \
	float fx = x - cx; \
	float fy = y - cy; \
	float cw = 0.5 / (tx * n); \
	float ch = 0.5 / (ty * n); \
	float detail = clamp(min(cw, ch) / pix - 2.0, 0.0, 1.0); \
	float gx = clamp(weave_fin(gx_in, 0.1), 0.0, 1.0); \
	float gy = clamp(weave_fin(gy_in, 0.1), 0.0, 1.0); \
	float hx = 0.5 * pix / cw; \
	float hy = 0.5 * pix / ch; \
	float sxn = 1.0; \
	float syn = 1.0; \
	if (fx < 0.5) { sxn = -1.0; } \
	if (fy < 0.5) { syn = -1.0; } \
	float c0 = weave_part(0.0, cx, cy, fx, fy, hx, hy, n, gx, gy, s); \
	float c1 = weave_part(0.0, cx + sxn, cy, fx - sxn, fy, hx, hy, n, gx, gy, s); \
	float c2 = weave_part(0.0, cx, cy + syn, fx, fy - syn, hx, hy, n, gx, gy, s); \
	float c3 = weave_part(0.0, cx + sxn, cy + syn, fx - sxn, fy - syn, hx, hy, n, gx, gy, s); \
	float wide = 0.0; \
	if (gx > 0.5 || gy > 0.5) { wide = 1.0; } \
	float c4 = 0.0; \
	float c5 = 0.0; \
	float c6 = 0.0; \
	float c7 = 0.0; \
	float c8 = 0.0; \
	if (wide > 0.5) { \
		c4 = weave_part(0.0, cx - sxn, cy, fx + sxn, fy, hx, hy, n, gx, gy, s); \
		c5 = weave_part(0.0, cx, cy - syn, fx, fy + syn, hx, hy, n, gx, gy, s); \
		c6 = weave_part(0.0, cx + sxn, cy - syn, fx - sxn, fy + syn, hx, hy, n, gx, gy, s); \
		c7 = weave_part(0.0, cx - sxn, cy + syn, fx + sxn, fy - syn, hx, hy, n, gx, gy, s); \
		c8 = weave_part(0.0, cx - sxn, cy - syn, fx + sxn, fy + syn, hx, hy, n, gx, gy, s); \
	} \
	float cover = min(c0 + c1 + c2 + c3 + c4 + c5 + c6 + c7 + c8, 1.0); \
	float tm = 0.5 * ((n + gx) * (1.0 - gy) + (n + gy) * (1.0 - gx)) / n; \
	if (which > 1.5) { \
		float best = c0; \
		float id = weave_id(cx, cy, n); \
		if (c1 > best) { best = c1; id = weave_id(cx + sxn, cy, n); } \
		if (c2 > best) { best = c2; id = weave_id(cx, cy + syn, n); } \
		if (c3 > best) { best = c3; id = weave_id(cx + sxn, cy + syn, n); } \
		if (c4 > best) { best = c4; id = weave_id(cx - sxn, cy, n); } \
		if (c5 > best) { best = c5; id = weave_id(cx, cy - syn, n); } \
		if (c6 > best) { best = c6; id = weave_id(cx + sxn, cy - syn, n); } \
		if (c7 > best) { best = c7; id = weave_id(cx - sxn, cy + syn, n); } \
		if (c8 > best) { best = c8; id = weave_id(cx - sxn, cy - syn, n); } \
		return 0.5 + (id - 0.5) * detail; \
	} \
	if (which > 0.5) { return tm + (cover - tm) * detail; } \
	float ox = weave_ivl(fx - hx, fx + hx, 0.0 - 0.5 * gx, 0.5 * gx) / (2.0 * hx) + weave_ivl(fx - hx, fx + hx, 1.0 - 0.5 * gx, 1.0 + 0.5 * gx) / (2.0 * hx); \
	float oy = weave_ivl(fy - hy, fy + hy, 0.0 - 0.5 * gy, 0.5 * gy) / (2.0 * hy) + weave_ivl(fy - hy, fy + hy, 1.0 - 0.5 * gy, 1.0 + 0.5 * gy) / (2.0 * hy); \
	float black = min(ox * oy, 1.0 - cover); \
	float hv = weave_part(1.0, cx, cy, fx, fy, hx, hy, n, gx, gy, s); \
	hv = hv + weave_part(1.0, cx + sxn, cy, fx - sxn, fy, hx, hy, n, gx, gy, s); \
	hv = hv + weave_part(1.0, cx, cy + syn, fx, fy - syn, hx, hy, n, gx, gy, s); \
	hv = hv + weave_part(1.0, cx + sxn, cy + syn, fx - sxn, fy - syn, hx, hy, n, gx, gy, s); \
	if (wide > 0.5) { \
		hv = hv + weave_part(1.0, cx - sxn, cy, fx + sxn, fy, hx, hy, n, gx, gy, s); \
		hv = hv + weave_part(1.0, cx, cy - syn, fx, fy + syn, hx, hy, n, gx, gy, s); \
		hv = hv + weave_part(1.0, cx + sxn, cy - syn, fx - sxn, fy + syn, hx, hy, n, gx, gy, s); \
		hv = hv + weave_part(1.0, cx - sxn, cy + syn, fx + sxn, fy - syn, hx, hy, n, gx, gy, s); \
		hv = hv + weave_part(1.0, cx - sxn, cy - syn, fx + sxn, fy + syn, hx, hy, n, gx, gy, s); \
	} \
	float value = hv + (1.0 - cover - black) * weave_gap_level(); \
	float cov_h = (n + gx) * (1.0 - gy) / n; \
	float cov_v = (n + gy) * (1.0 - gx) / n; \
	float sh_h = 1.0 - 2.0 * weave_shade_depth() * weave_shade_mean(weave_shade_len(s, n), n + gx); \
	float sh_v = 1.0 - 2.0 * weave_shade_depth() * weave_shade_mean(weave_shade_len(s, n), n + gy); \
	float mean = 0.5 * (cov_h * sh_h + cov_v * sh_v) + (1.0 - tm - gx * gy) * weave_gap_level(); \
	return mean + (value - mean) * detail; \
} \
";

// Input socket indices
enum {
	WEAVE_GENERATOR_INPUT_TILE_X = 0,
	WEAVE_GENERATOR_INPUT_TILE_Y = 1,
	WEAVE_GENERATOR_INPUT_SHAPE  = 2,
	WEAVE_GENERATOR_INPUT_WEAVE  = 3,
	WEAVE_GENERATOR_INPUT_GAP_X  = 4,
	WEAVE_GENERATOR_INPUT_GAP_Y  = 5,
	WEAVE_GENERATOR_INPUT_VECTOR = 6, // Last, like the other pass-through generators; not a number input
	WEAVE_GENERATOR_INPUT_COUNT  = 7,
};

// Node button indices (settings, the General section)
enum {
	WEAVE_GENERATOR_BUTTON_WRAP  = 0,
	WEAVE_GENERATOR_BUTTON_COUNT = 1,
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
	char *px = weave_generator_node_pixel(store, uv_unwrapped);
	return string_tmp("weave_eval(%d.0, %s.x, %s.y, %s, %s, %s, %s, %s, %s, %s)", weave_generator_node_output, uv, uv, in[WEAVE_GENERATOR_INPUT_TILE_X], in[WEAVE_GENERATOR_INPUT_TILE_Y],
	                  in[WEAVE_GENERATOR_INPUT_SHAPE], in[WEAVE_GENERATOR_INPUT_WEAVE], in[WEAVE_GENERATOR_INPUT_GAP_X], in[WEAVE_GENERATOR_INPUT_GAP_Y], px);
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
	                        make_image_node_socket(_tr("Tile X"), "VALUE", value_color, f32_array_create_x(1.0), 1.0, 20.0, 1),
	                        make_image_node_socket(_tr("Tile Y"), "VALUE", value_color, f32_array_create_x(1.0), 1.0, 20.0, 1),
	                        make_image_node_socket(_tr("Shape"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Weave"), "VALUE", value_color, f32_array_create_x(2.0), 1.0, 10.0, 1),
	                        make_image_node_socket(_tr("Gap X"), "VALUE", value_color, f32_array_create_x(0.1), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Gap Y"), "VALUE", value_color, f32_array_create_x(0.1), 0.0, 1.0, 100),
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
