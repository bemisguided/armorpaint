#include "../global.h"

// Flood Fill: connected islands of a mask with a random grey, a bounding box gradient and the size per island.
// Generator node: the graph feeding the Mask is evaluated over the node's own square, [0, 1] of tex_coord, independent
// of the mesh (render/make_flood_fill.c), and labelled on the cpu into one RGBA64 texture per node:
// R = ID, G = Position x, B = Position y, A = Area (floored to 1/1024 inside kept islands, so Mask = A > 0).
// The texture is sampled like an Image Texture: at the Vector input or tex_coord, repeating.

#define FLOOD_FILL_AREA_FLOOR 1024.0

// Input socket indices
enum {
	FLOOD_FILL_INPUT_MASK   = 0,
	FLOOD_FILL_INPUT_VECTOR = 1,
};

// Node button indices
enum {
	FLOOD_FILL_BUTTON_THRESHOLD    = 0,
	FLOOD_FILL_BUTTON_INVERT       = 1,
	FLOOD_FILL_BUTTON_CONNECTIVITY = 2,
	FLOOD_FILL_BUTTON_WRAP         = 3,
	FLOOD_FILL_BUTTON_MIN_SIZE     = 4,
	FLOOD_FILL_BUTTON_SEED         = 5,
	FLOOD_FILL_BUTTON_SIZE         = 6,
};

// Output socket indices
enum {
	FLOOD_FILL_OUTPUT_ID       = 0,
	FLOOD_FILL_OUTPUT_POSITION = 1,
	FLOOD_FILL_OUTPUT_AREA     = 2,
	FLOOD_FILL_OUTPUT_MASK     = 3,
};

static f32 flood_fill_node_button(ui_node_t *node, i32 i) {
	return node->buttons->buffer[i]->default_value->buffer[0];
}

f32 flood_fill_node_threshold(ui_node_t *node) {
	return flood_fill_node_button(node, FLOOD_FILL_BUTTON_THRESHOLD);
}

bool flood_fill_node_invert(ui_node_t *node) {
	return flood_fill_node_button(node, FLOOD_FILL_BUTTON_INVERT) != 0.0;
}

// Width and height of the generated square: Size 512, 1024, 2048 or 4096
i32 flood_fill_node_size(ui_node_t *node) {
	i32 i = (i32)flood_fill_node_button(node, FLOOD_FILL_BUTTON_SIZE);
	return 512 << (i < 0 ? 0 : i > 3 ? 3 : i);
}

// Labelling parameters of the node for a width x height mask
flood_fill_params_t flood_fill_node_params(ui_node_t *node, i32 width, i32 height) {
	f32 min_size = flood_fill_node_button(node, FLOOD_FILL_BUTTON_MIN_SIZE);
	f32 seed     = flood_fill_node_button(node, FLOOD_FILL_BUTTON_SEED);
	return (flood_fill_params_t){
	    .width         = width,
	    .height        = height,
	    .connectivity8 = flood_fill_node_button(node, FLOOD_FILL_BUTTON_CONNECTIVITY) != 0.0,
	    .wrap          = flood_fill_node_button(node, FLOOD_FILL_BUTTON_WRAP) != 0.0,
	    .seed          = (u32)math_floor(seed > 0.0 ? seed : 0.0),
	    .min_size      = (i32)math_floor(min_size > 0.0 ? min_size : 0.0),
	};
}

// Writes float4 <var> = the texel of the baked texture under uv (a float2 expression), repeating.
// The uv is wrapped and snapped to the texel centre, so the one linear sampler never blends two island ids.
static void flood_fill_node_write_sample(char *var, char *tex_name, char *uv) {
	parser_material_write(parser_material_kong, string_tmp("float2 %s_uv = %s;", var, uv));
	parser_material_write(parser_material_kong, string_tmp("%s_uv = (floor((%s_uv - floor(%s_uv)) * constants.%s_size) + 0.5) / constants.%s_size;", var, var,
	                                                       var, tex_name, tex_name));
	parser_material_write(parser_material_kong, string_tmp("float4 %s = sample_lod(%s, sampler_linear, %s_uv, 0.0);", var, tex_name, var));
}

// Samples the baked texture once per node, shared by all outputs
static char *flood_fill_node_write(ui_node_t *node) {
	char *store = parser_material_store_var_name(node);
	char *key   = string_tmp("%s_flood", store);
	if (string_array_index_of(parser_material_parsed, key) >= 0) {
		return store;
	}
	any_array_push(parser_material_parsed, key);

	char *tex_name = string_tmp("texflood_%s", make_material_node_preview_id(node, NULL));
	node_shader_add_texture(parser_material_kong, tex_name, string_tmp("_%s", tex_name));
	node_shader_add_constant(parser_material_kong, string_tmp("float2 %s_size", tex_name), string_tmp("_size(_%s)", tex_name));
	char *ff = string_tmp("%s_ff", store);

	ui_node_socket_t *vector = node->inputs->buffer[FLOOD_FILL_INPUT_VECTOR];
	if (parser_material_get_input_link(vector) != NULL) {
		char *uv = parser_material_parse_vector_input(vector);
		flood_fill_node_write_sample(ff, tex_name, string_tmp("%s.xy", uv));
	}
	else if (parser_material_triplanar) { // Like Image Texture: one sample per projection, blended
		node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
		flood_fill_node_write_sample(string_tmp("%s0", ff), tex_name, "tex_coord");
		flood_fill_node_write_sample(string_tmp("%s1", ff), tex_name, "tex_coord1");
		flood_fill_node_write_sample(string_tmp("%s2", ff), tex_name, "tex_coord2");
		parser_material_write(parser_material_kong,
		                      string_tmp("float4 %s = %s0 * tex_coord_blend.x + %s1 * tex_coord_blend.y + %s2 * tex_coord_blend.z;", ff, ff, ff, ff));
	}
	else {
		node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
		flood_fill_node_write_sample(ff, tex_name, "tex_coord");
	}
	return store;
}

char *flood_fill_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	if (parser_material_flood_passthrough == node) { // Capturing the input for the bake
		return parser_material_parse_value_input(node->inputs->buffer[FLOOD_FILL_INPUT_MASK], false);
	}
	char *store = flood_fill_node_write(node);
	i32   i     = array_index_of(node->outputs, socket);
	if (i == FLOOD_FILL_OUTPUT_POSITION) {
		return string_tmp("%s_ff.g", store);
	}
	if (i == FLOOD_FILL_OUTPUT_AREA) {
		return string_tmp("%s_ff.a", store);
	}
	if (i == FLOOD_FILL_OUTPUT_MASK) {
		return string_tmp("clamp(%s_ff.a * %s, 0.0, 1.0)", store, parser_material_vec1(FLOOD_FILL_AREA_FLOOR));
	}
	return string_tmp("%s_ff.r", store);
}

char *flood_fill_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	if (parser_material_flood_passthrough == node) {
		return parser_material_to_vec3(parser_material_parse_value_input(node->inputs->buffer[FLOOD_FILL_INPUT_MASK], false));
	}
	if (array_index_of(node->outputs, socket) == FLOOD_FILL_OUTPUT_POSITION) {
		char *store = flood_fill_node_write(node);
		return string_tmp("float3(%s_ff.g, %s_ff.b, 0.0)", store, store);
	}
	return parser_material_to_vec3(flood_fill_node_value(node, socket));
}

// Bake row: the Bake button, highlighted as Baking while a bake is pending. Either bakes now.
static void flood_fill_node_bake_button(i32 node_id) {
	ui_node_t *node = ui_get_node(ui_nodes_get_canvas(true)->nodes, node_id);
	if (node == NULL) {
		return;
	}
	ui_push_id((ui_id_t)ui_nodes_get_nodes());
	ui_push_id((ui_id_t)node_id);
	bool pending     = make_flood_fill_is_pending(node);
	i32  _BUTTON_COL = g_theme->BUTTON_COL;
	if (pending) {
		g_theme->BUTTON_COL = g_theme->HIGHLIGHT_COL;
	}
	if (ui_icon_button(pending ? tr("Baking") : tr("Bake"), ICON_PLAY, UI_ALIGN_CENTER)) {
		make_flood_fill_bake_now(node);
	}
	g_theme->BUTTON_COL = _BUTTON_COL;
	ui_pop_id();
	ui_pop_id();
}

static ui_node_socket_t *flood_fill_node_socket(char *name, char *type, u32 color, f32_array_t *default_value) {
	return ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                     .node_id       = 0,
	                                     .name          = name,
	                                     .type          = type,
	                                     .color         = color,
	                                     .default_value = default_value,
	                                     .min           = 0.0,
	                                     .max           = 1.0,
	                                     .precision     = 100,
	                                     .display       = 0});
}

static ui_node_button_t *flood_fill_node_button_def(char *name, char *type, f32 default_value, char *data, f32 max, f32 precision, f32 height) {
	return ALLOC_INIT(ui_node_button_t, {.name          = name,
	                                     .type          = type,
	                                     .output        = -1,
	                                     .default_value = f32_array_create_x(default_value),
	                                     .data          = data != NULL ? u8_array_create_from_string(data) : NULL,
	                                     .min           = 0.0,
	                                     .max           = max,
	                                     .precision     = precision,
	                                     .height        = height});
}

void flood_fill_node_init() {
	u32 value_color  = 0xffa1a1a1;
	u32 vector_color = 0xff6363c7;

	ui_node_t *flood_fill_node_def =
	    ALLOC_INIT(ui_node_t, {.id     = 0,
	                           .name   = _tr("Flood Fill"),
	                           .type   = "TEX_FLOOD_FILL",
	                           .x      = 0,
	                           .y      = 0,
	                           .color  = NODES_MATERIAL_GENERATORS_COLOR,
	                           .inputs = any_array_create_from_raw(
	                               (void *[]){
	                                   flood_fill_node_socket(_tr("Mask"), "VALUE", value_color, f32_array_create_x(0.0)),
	                                   flood_fill_node_socket(_tr("Vector"), "VECTOR", vector_color, f32_array_create_xyz(0.0, 0.0, 0.0)),
	                               },
	                               2),
	                           .outputs = any_array_create_from_raw(
	                               (void *[]){
	                                   flood_fill_node_socket(_tr("ID"), "VALUE", value_color, f32_array_create_x(0.0)),
	                                   flood_fill_node_socket(_tr("Position"), "VECTOR", vector_color, f32_array_create_xyz(0.0, 0.0, 0.0)),
	                                   flood_fill_node_socket(_tr("Area"), "VALUE", value_color, f32_array_create_x(0.0)),
	                                   flood_fill_node_socket(_tr("Mask"), "VALUE", value_color, f32_array_create_x(0.0)),
	                               },
	                               4),
	                           .buttons = any_array_create_from_raw(
	                               (void *[]){
	                                   flood_fill_node_button_def(_tr("Threshold"), "VALUE", 0.5, NULL, 1.0, 100, 0),
	                                   flood_fill_node_button_def(_tr("Invert"), "BOOL", 0, NULL, 1.0, 100, 0),
	                                   flood_fill_node_button_def(_tr("Connectivity"), "ENUM", 0, "4\n8", 1.0, 100, 0),
	                                   flood_fill_node_button_def(_tr("Wrap Edges"), "BOOL", 0, NULL, 1.0, 100, 0),
	                                   flood_fill_node_button_def(_tr("Min Size"), "VALUE", 0, NULL, 4096.0, 1, 0),
	                                   flood_fill_node_button_def(_tr("Seed"), "VALUE", 0, NULL, 1000.0, 1, 0),
	                                   flood_fill_node_button_def(_tr("Size"), "ENUM", 1, "512\n1024\n2048\n4096", 1.0, 100, 0),
	                                   flood_fill_node_button_def("flood_fill_node_bake_button", "CUSTOM", 0, NULL, 1.0, 100, 1),
	                               },
	                               8),
	                           .width = 0,
	                           .flags = 0});

	any_array_push(nodes_material_generators, flood_fill_node_def);
	any_map_set(parser_material_node_values, "TEX_FLOOD_FILL", flood_fill_node_value);
	any_map_set(parser_material_node_vectors, "TEX_FLOOD_FILL", flood_fill_node_vector);
	any_map_set(ui_nodes_custom_buttons, "flood_fill_node_bake_button", flood_fill_node_bake_button);
}
