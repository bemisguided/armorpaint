#include "../global.h"

// Transform (Filter menu): moves, rotates, scales and flips an image in image space. A pass-through node with capture
// (render/make_image_node.c, image_node_type_t.pass_through_capture): it bakes nothing. Its Image input is evaluated in image space and
// kept as a render target (32 bit float, colour kept) of Capture Size, and the material shader samples that texture at the transformed
// coordinate; the placement, Filter, Tiling and every number are in the shader, so a slider never captures again, and a linked number
// is per pixel. An Image linked straight to the Image output of a baked node reads that node's result (no copy).
// It never touches the uv, Tile UV places an image on the mesh.
//
// Everything is about the Pivot: flip, then scale, then rotation, then Offset. Output pixel q reads the source at
// p = pivot + inverse(R * S * F) * (q - pivot - offset), R = [cos a, -sin a; sin a, cos a] on the image space (y down).
// Rotation follows the rotation convention of all image nodes (armorpaint-admin specs/image-nodes/ROTATION_CONVENTION.md):
// degrees, a positive angle turns the image clockwise as seen in the editor view (+90 sends the top left corner to the
// top right), about the Pivot, after the scale and before the Offset.
// Cubic is Catmull-Rom (16 fetches, written here: make_image_node_sample_input_at only repeats and its Cubic is Tile UV's B-spline), not
// Tile UV's B-spline: it passes through the texels, so the default settings give back the input texel for texel. Linear is four fetches
// and Nearest one, all at texel centres (a linear filter of a 32 bit float target is done by hand). No mipmaps: a source shrunk by more
// than about 2x aliases. The texels outside the image follow Tiling (Tile, Mirror, Clamp, None: black), applied to the texel indices.
//
// A linked Scale of 0 or below is clamped to the slider minimum 0.01 (no mirror). For Tile and Mirror without Rotation the Offset is
// reduced to the period of the tiling (a multiple of Scale times the period) before the float subtraction, so a huge Offset (1e6)
// behaves like its fraction; with a Rotation the float precision of the shader applies (the cpu bake did it in f64).
//
// Rotation and Scale are used as set for every Tiling mode. The output of a Tile Transform is periodic with period 1
// only when the rotation maps the lattice onto itself (multiples of 90 degrees with an integer 1 / Scale), otherwise
// it shows a seam when tiled (known limit). None is black: the Image output carries colour only (the capture holds alpha 1). The maths is in
// armorpaint-admin specs/image-nodes/IMAGE_NODES.md, section Transform.

#define TRANSFORM_SCALE_MIN 0.01 // Minimum of the Scale sliders

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

// Own sections of the node, a contiguous range of inputs each (Image belongs to the General section), see make_image_node_sections_capture
enum {
	TRANSFORM_SECTION_LAYOUT_FIRST = TRANSFORM_INPUT_OFFSET_X,
	TRANSFORM_SECTION_LAYOUT_COUNT = TRANSFORM_INPUT_SCALE_Y - TRANSFORM_INPUT_OFFSET_X + 1,
	TRANSFORM_SECTION_PIVOT_FIRST  = TRANSFORM_INPUT_PIVOT_X,
	TRANSFORM_SECTION_PIVOT_COUNT  = TRANSFORM_INPUT_PIVOT_Y - TRANSFORM_INPUT_PIVOT_X + 1,
};

// Node button indices (settings)
enum {
	TRANSFORM_BUTTON_CAPTURE_SIZE = 0,
	TRANSFORM_BUTTON_TILING       = 1,
	TRANSFORM_BUTTON_FILTER       = 2,
	TRANSFORM_BUTTON_FLIP_X       = 3,
	TRANSFORM_BUTTON_FLIP_Y       = 4,
};

// Filter setting values (the order of the combo); the shader takes IMAGE_FILTER_*, mapped in transform_node_sample.
// The Tiling setting values are IMAGE_TILING_* (types.h) in the same order as its combo.
enum {
	TRANSFORM_FILTER_LINEAR,
	TRANSFORM_FILTER_CUBIC,
	TRANSFORM_FILTER_NEAREST,
};

// Width and height of the capture of the Image input: Capture Size 512, 1024, 2048 or 4096
static i32 transform_node_size(ui_node_t *node) {
	return make_image_node_size(node, TRANSFORM_BUTTON_CAPTURE_SIZE);
}

// Captured per pixel: the Image input (the capture target of the shared pass)
static char *transform_node_capture(ui_node_t *node) {
	char *image = parser_material_parse_vector_input(node->inputs->buffer[TRANSFORM_INPUT_IMAGE]);
	return string_tmp("float4(%s, 1.0)", image);
}

static image_node_type_t transform_node_image_type = {
    .type                 = "TEX_TRANSFORM",
    .size                 = transform_node_size,
    .capture              = transform_node_capture,
    .capture_format       = IMAGE_CAPTURE_RGBA32F,
    .pass_through_capture = true,
    .image_input          = TRANSFORM_INPUT_IMAGE,
};

static void transform_node_write(char *statement) {
	parser_material_write(parser_material_kong, statement);
}

// Wraps the texel indices of the variable idx (type vec: float, float2 or float4, the taps of one axis, n texels wide) after the tiling mode. None also writes the
// vector mask the weights are multiplied with (1 inside the image) and returns its name, else NULL. Statements only, no branches.
static char *transform_node_write_wrap(char *idx, char *vec, char *n, i32 tiling) {
	switch (tiling) {
	case IMAGE_TILING_TILE:
		transform_node_write(string_tmp("%s = %s - %s * floor(%s / %s);", idx, idx, n, idx, n));
		break;
	case IMAGE_TILING_MIRROR:
		transform_node_write(string_tmp("%s = %s - 2.0 * %s * floor(%s / (2.0 * %s));", idx, idx, n, idx, n, n));
		transform_node_write(string_tmp("%s = min(%s, 2.0 * %s - 1.0 - %s);", idx, idx, n, idx)); // The mirrored half
		break;
	case IMAGE_TILING_CLAMP:
		transform_node_write(string_tmp("%s = clamp(%s, 0.0, %s - 1.0);", idx, idx, n));
		break;
	default: {
		char *mask = string_tmp("%s_mask", idx);
		transform_node_write(string_tmp("%s %s = step(0.0, %s) * step(%s, %s - 1.0);", vec, mask, idx, idx, n));
		transform_node_write(string_tmp("%s = clamp(%s, 0.0, %s - 1.0);", idx, idx, n));
		return mask;
	}
	}
	return NULL;
}

// The float4 variable of the node's image input at the transformed coordinate, once per node. The texture is the capture of the Image input
// (or the result of the baked node it is linked to), read at texel centres with sample_lod and the one linear sampler, which then never
// blends two texels itself. Black until the first capture.
static char *transform_node_sample(ui_node_t *node) {
	char *var      = string_tmp("%s_input", parser_material_store_var_name(node));
	char *tex_name = make_image_node_bind_input(node);
	if (string_array_index_of(parser_material_parsed, var) >= 0) {
		return var;
	}
	any_array_push(parser_material_parsed, var);
	if (tex_name == NULL) {
		transform_node_write(string_tmp("float4 %s = float4(0.0, 0.0, 0.0, 1.0);", var));
		return var;
	}
	if (!parser_material_image_space) {
		node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
	}
	i32 setting = (i32)make_image_node_button(node, TRANSFORM_BUTTON_FILTER);
	i32 filter  = setting == TRANSFORM_FILTER_LINEAR ? IMAGE_FILTER_LINEAR : setting == TRANSFORM_FILTER_CUBIC ? IMAGE_FILTER_CUBIC : IMAGE_FILTER_NEAREST;
	i32 tiling  = (i32)make_image_node_button(node, TRANSFORM_BUTTON_TILING);
	bool flip_x = make_image_node_button(node, TRANSFORM_BUTTON_FLIP_X) != 0.0f;
	bool flip_y = make_image_node_button(node, TRANSFORM_BUTTON_FLIP_Y) != 0.0f;
	bool wraps  = tiling == IMAGE_TILING_TILE || tiling == IMAGE_TILING_MIRROR;
	char *period = tiling == IMAGE_TILING_MIRROR ? "2.0" : "1.0";

	char *ox  = parser_material_parse_value_input(node->inputs->buffer[TRANSFORM_INPUT_OFFSET_X], false);
	char *oy  = parser_material_parse_value_input(node->inputs->buffer[TRANSFORM_INPUT_OFFSET_Y], false);
	char *rot = parser_material_parse_value_input(node->inputs->buffer[TRANSFORM_INPUT_ROTATION], false);
	char *sx  = parser_material_parse_value_input(node->inputs->buffer[TRANSFORM_INPUT_SCALE_X], false);
	char *sy  = parser_material_parse_value_input(node->inputs->buffer[TRANSFORM_INPUT_SCALE_Y], false);
	char *px  = parser_material_parse_value_input(node->inputs->buffer[TRANSFORM_INPUT_PIVOT_X], false);
	char *py  = parser_material_parse_value_input(node->inputs->buffer[TRANSFORM_INPUT_PIVOT_Y], false);

	// Source position p = pivot + inverse(R S F) (q - pivot - offset); a Scale below the minimum (a linked 0, negative or not a number) is the minimum
	transform_node_write(string_tmp("float %s_a = %s * (3.14159265 / 180.0);", var, rot));
	transform_node_write(string_tmp("float %s_ca = cos(%s_a);", var, var));
	transform_node_write(string_tmp("float %s_sa = sin(%s_a);", var, var));
	transform_node_write(string_tmp("float %s_sx = %f;", var, TRANSFORM_SCALE_MIN));
	transform_node_write(string_tmp("float %s_sy = %f;", var, TRANSFORM_SCALE_MIN));
	transform_node_write(string_tmp("if (%s > %f) { %s_sx = %s; }", sx, TRANSFORM_SCALE_MIN, var, sx));
	transform_node_write(string_tmp("if (%s > %f) { %s_sy = %s; }", sy, TRANSFORM_SCALE_MIN, var, sy));
	transform_node_write(string_tmp("float2 %s_c = float2(%s, %s);", var, px, py));
	transform_node_write(string_tmp("float2 %s_o = float2(%s, %s);", var, ox, oy));
	if (wraps) {
		// Without rotation a shift of the Offset by a multiple of (Scale x period) moves the source by whole periods: keep its fraction
		transform_node_write(string_tmp("float2 %s_e = float2(%s_sx, %s_sy) * %s;", var, var, var, period));
		transform_node_write(string_tmp("if (%s_a == 0.0) { %s_o = %s_o - %s_e * floor(%s_o / %s_e); }", var, var, var, var, var, var));
	}
	transform_node_write(string_tmp("float2 %s_d = tex_coord - %s_c - %s_o;", var, var, var));
	transform_node_write(string_tmp("float2 %s_p = float2(%s_d.x * %s_ca + %s_d.y * %s_sa, -%s_d.x * %s_sa + %s_d.y * %s_ca) / float2(%s_sx, %s_sy);", var, var, var, var,
	                                var, var, var, var, var, var, var));
	if (flip_x) {
		transform_node_write(string_tmp("%s_p = float2(-%s_p.x, %s_p.y);", var, var, var));
	}
	if (flip_y) {
		transform_node_write(string_tmp("%s_p = float2(%s_p.x, -%s_p.y);", var, var, var));
	}
	transform_node_write(string_tmp("%s_p = %s_c + %s_p;", var, var, var));
	if (wraps) {
		// The source position reduced to its period (the same texels the tiling of the indices picks), so a far position keeps its precision
		transform_node_write(string_tmp("%s_p = %s_p - %s * floor(%s_p / %s);", var, var, period, var, period));
	}
	transform_node_write(string_tmp("float2 %s_n = constants.%s_size;", var, tex_name));

	// Taps per axis: Nearest 1, Linear 2 (texels fl, fl + 1), Cubic 4 (fl - 1 .. fl + 2); float, float2 or float4 vectors of the taps of one axis
	i32   taps = filter == IMAGE_FILTER_NEAREST ? 1 : filter == IMAGE_FILTER_LINEAR ? 2 : 4;
	char *vec  = taps == 1 ? "float" : taps == 2 ? "float2" : "float4";
	char *comp[4] = {"x", "y", "z", "w"};
	char *axis[2] = {"x", "y"};
	char *masks[2] = {NULL, NULL};
	for (i32 a = 0; a < 2; ++a) {
		char *c  = axis[a];
		char *ix = string_tmp("%s_i%s", var, c);
		char *wx = string_tmp("%s_w%s", var, c);
		if (filter == IMAGE_FILTER_NEAREST) {
			transform_node_write(string_tmp("float %s = floor(%s_p.%s * %s_n.%s);", ix, var, c, var, c));
		}
		else {
			char *t = string_tmp("%s_t%s", var, c);
			transform_node_write(string_tmp("float %s_q%s = %s_p.%s * %s_n.%s - 0.5;", var, c, var, c, var, c));
			transform_node_write(string_tmp("float %s_f%s = floor(%s_q%s);", var, c, var, c));
			transform_node_write(string_tmp("float %s = %s_q%s - %s_f%s;", t, var, c, var, c)); // Float error must not blur a read on a texel centre
			transform_node_write(string_tmp("if (%s < 0.0001) { %s = 0.0; }", t, t));
			transform_node_write(string_tmp("if (%s > 0.9999) { %s = 1.0; }", t, t));
			if (filter == IMAGE_FILTER_LINEAR) {
				transform_node_write(string_tmp("float2 %s = float2(%s_f%s, %s_f%s + 1.0);", ix, var, c, var, c));
				transform_node_write(string_tmp("float2 %s = float2(1.0 - %s, %s);", wx, t, t));
			}
			else {
				// Catmull-Rom weights of the four texels around the fraction t
				transform_node_write(string_tmp("float %s2 = %s * %s;", t, t, t));
				transform_node_write(string_tmp("float %s3 = %s2 * %s;", t, t, t));
				transform_node_write(string_tmp("float4 %s = float4(%s_f%s - 1.0, %s_f%s, %s_f%s + 1.0, %s_f%s + 2.0);", ix, var, c, var, c, var, c, var, c));
				transform_node_write(string_tmp("float4 %s = float4(0.5 * (-%s3 + 2.0 * %s2 - %s), 0.5 * (3.0 * %s3 - 5.0 * %s2 + 2.0), "
				                                "0.5 * (-3.0 * %s3 + 4.0 * %s2 + %s), 0.5 * (%s3 - %s2));",
				                                wx, t, t, t, t, t, t, t, t, t, t));
			}
		}
		masks[a] = transform_node_write_wrap(ix, vec, string_tmp("%s_n.%s", var, c), tiling);
		if (masks[a] != NULL && filter != IMAGE_FILTER_NEAREST) {
			transform_node_write(string_tmp("%s = %s * %s;", wx, wx, masks[a])); // None: a texel outside has no weight
		}
	}
	char *fetch_fmt = "sample_lod(%s, sampler_linear, float2(%s + 0.5, %s + 0.5) / %s_n, 0.0)";
	if (filter == IMAGE_FILTER_NEAREST) {
		char *mask = "";
		if (masks[0] != NULL) {
			mask = string_tmp(" * %s * %s", masks[0], masks[1]);
		}
		transform_node_write(string_tmp("float4 %s = sample_lod(%s, sampler_linear, float2(%s_ix + 0.5, %s_iy + 0.5) / %s_n, 0.0)%s;", var, tex_name, var, var, var, mask));
		return var;
	}
	for (i32 j = 0; j < taps; ++j) {
		char *row = NULL;
		for (i32 i = 0; i < taps; ++i) {
			char *f = string_tmp(fetch_fmt, tex_name, string_tmp("%s_ix.%s", var, comp[i]), string_tmp("%s_iy.%s", var, comp[j]), var);
			f       = string_tmp("%s * %s_wx.%s", f, var, comp[i]);
			row     = row == NULL ? f : string_tmp("%s + %s", row, f);
		}
		transform_node_write(string_tmp("float4 %s_r%d = %s;", var, j, row));
	}
	char *sum = NULL;
	for (i32 j = 0; j < taps; ++j) {
		char *r = string_tmp("%s_r%d * %s_wy.%s", var, j, var, comp[j]);
		sum     = sum == NULL ? r : string_tmp("%s + %s", sum, r);
	}
	transform_node_write(string_tmp("float4 %s = %s;", var, sum));
	return var;
}

char *transform_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.rgb", transform_node_sample(node));
}

char *transform_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", transform_node_sample(node));
}

void transform_node_init() {
	u32 value_color = NODES_MATERIAL_VALUE_COLOR;

	ui_node_t *transform_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Transform"),
	                .type   = "TEX_TRANSFORM",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_PASS_THROUGH_COLOR, // Bakes nothing: a shader on the mesh over a capture of the Image
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_RGBA_COLOR, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Offset X"), "VALUE", value_color, f32_array_create_x(0.0), -1.0, 1.0, 100),
	                        make_image_node_socket(_tr("Offset Y"), "VALUE", value_color, f32_array_create_x(0.0), -1.0, 1.0, 100),
	                        make_image_node_socket(_tr("Rotation"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 360.0, 10),
	                        make_image_node_socket(_tr("Scale X"), "VALUE", value_color, f32_array_create_x(1.0), TRANSFORM_SCALE_MIN, 8.0, 100),
	                        make_image_node_socket(_tr("Scale Y"), "VALUE", value_color, f32_array_create_x(1.0), TRANSFORM_SCALE_MIN, 8.0, 100),
	                        make_image_node_socket(_tr("Pivot X"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Pivot Y"), "VALUE", value_color, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                    },
	                    8),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "RGBA", NODES_MATERIAL_RGBA_COLOR, f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_capture_size_setting(),
	                        make_image_node_setting(_tr("Tiling"), "ENUM", IMAGE_TILING_TILE, "Tile\nMirror\nClamp\nNone"),
	                        make_image_node_setting(_tr("Filter"), "ENUM", TRANSFORM_FILTER_CUBIC, "Linear\nCubic\nNearest"),
	                        make_image_node_setting(_tr("Flip X"), "BOOL", 0, NULL),
	                        make_image_node_setting(_tr("Flip Y"), "BOOL", 0, NULL),
	                    },
	                    5),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, transform_node_def);
	any_map_set(parser_material_node_values, "TEX_TRANSFORM", transform_node_value);
	any_map_set(parser_material_node_vectors, "TEX_TRANSFORM", transform_node_vector);
	ui_node_section_t sections[] = {
	    {.title = _tr("Layout"), .kind = UI_NODE_ITEM_INPUT, .first = TRANSFORM_SECTION_LAYOUT_FIRST, .count = TRANSFORM_SECTION_LAYOUT_COUNT, .closed_by_default = false},
	    {.title = _tr("Pivot"), .kind = UI_NODE_ITEM_INPUT, .first = TRANSFORM_SECTION_PIVOT_FIRST, .count = TRANSFORM_SECTION_PIVOT_COUNT, .closed_by_default = true},
	};
	make_image_node_sections_capture(transform_node_def, sections, 2, TRANSFORM_SECTION_LAYOUT_FIRST); // General: the settings and Image; Texture: Capture Size
	make_image_node_register(&transform_node_image_type);
}
