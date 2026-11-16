
#include "../global.h"

// Texture Slot: the baked texture of a bake type (Bake Properties on textures) for the object and tile being painted.
// The button stores the persistent bake type id (BTP_*), not the position in the combo.
// One sampler per bake type (texslot_<btp>), bound to the hidden atlas of that type (_texslot_<btp>, uniforms.c).

static i32 texture_slot_node_type(ui_node_t *node) {
	return (i32)node->buttons->buffer[0]->default_value->buffer[0];
}

static void texture_slot_node_button(i32 node_id) {
	ui_node_t *node = ui_get_node(ui_nodes_get_canvas(true)->nodes, node_id);
	ui_push_id((ui_id_t)ui_nodes_get_nodes());
	ui_push_id((ui_id_t)node_id);

	f32_array_t    *values = node->buttons->buffer[0]->default_value;
	i32             btp    = (i32)values->buffer[0];
	i32             count  = asset_bake_type_count();
	string_array_t *labels = any_array_create_from_raw((void *[]){}, 0);
	i32             sel    = -1;
	for (i32 i = 0; i < count; ++i) {
		i32 id = asset_bake_type_at(i);
		any_array_push(labels, asset_bake_type_label(id));
		if (id == btp) {
			sel = i;
		}
	}
	if (sel < 0) { // Unknown id of a newer version, kept until another type is picked
		any_array_push(labels, asset_bake_type_label(btp));
		sel = count;
	}
	ui_set_next_id((ui_id_t)&values->buffer[0]);
	ui_combo(&sel, labels, tr("Bake Type"), false, UI_ALIGN_LEFT, true);
	values->buffer[0] = sel < count ? asset_bake_type_at(sel) : btp;

	// Informational hint, dim, wrapped to the node width. The height follows on the next draw.
	i32 lines = 0;
	if (util_uv_overlap_covered(g_context->layer)) {
		char *text  = tr("Several covered objects share UV space.");
		i32   len   = string_length(text);
		i32   start = 0;
		i32   col   = g_theme->TEXT_COL;
		g_theme->TEXT_COL = base_darker(col, 0x00202020);
		while (start < len) {
			i32 end        = start;
			i32 last_space = -1;
			while (end < len && draw_sub_string_width(g_font, g_ui->font_size, text, start, end + 1) <= g_ui->_w) {
				if (text[end] == ' ') {
					last_space = end;
				}
				end++;
			}
			if (end < len && last_space > start) {
				end = last_space;
			}
			end = end == start ? start + 1 : end; // Always advance
			ui_text(substring(text, start, end), UI_ALIGN_LEFT, 0x00000000);
			lines++;
			start = end < len && text[end] == ' ' ? end + 1 : end;
		}
		g_theme->TEXT_COL = col;
	}
	node->buttons->buffer[0]->height = 1.5 + lines;
	ui_pop_id();
	ui_pop_id();
}

// Samples the atlas once per node into <store>, same uv as the image texture node (the atlas uv in udim projects)
static char *texture_slot_node_sample(ui_node_t *node) {
	i32   btp      = texture_slot_node_type(node);
	char *tex_name = string_tmp("texslot_%d", btp);
	texture_slot_want(btp);
	node_shader_context_add_elem(parser_material_kong->context, "tex", "short2norm");
	node_shader_add_texture(parser_material_kong, tex_name, string_tmp("_texslot_%d", btp));

	char *store   = parser_material_store_var_name(node);
	char *uv_name = parser_material_tex_coord;
	if (parser_material_tex_coord_atlas != NULL) {
		parser_material_write(parser_material_kong, string_tmp("float2 %s_atlas = %s;", store, parser_material_tex_coord_atlas));
		uv_name = string_tmp("%s_atlas", store);
	}
	if (parser_material_is_frag) {
		parser_material_write(parser_material_kong, string_tmp("float4 %s = sample(%s, sampler_linear, %s.xy);", store, tex_name, uv_name));
	}
	else {
		parser_material_write(parser_material_kong, string_tmp("float4 %s = sample_lod(%s, sampler_linear, %s.xy, 0.0);", store, tex_name, uv_name));
	}
	// Color maps are stored as sRGB, same as the sRGB color space of the image node
	if (asset_bake_default_encoding(btp) == AE_COLOR && parser_material_transform_color_space && !parser_material_parsing_basecolor) {
		parser_material_write(parser_material_kong, string_tmp("%s.rgb = pow(%s.rgb, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));", store, store));
	}
	return store;
}

char *texture_slot_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	// Already fetched by the Value output
	if (string_array_index_of(parser_material_parsed, parser_material_res_var_name(node, node->outputs->buffer[1])) >= 0) {
		return string_tmp("%s.rgb", parser_material_store_var_name(node));
	}
	return string_tmp("%s.rgb", texture_slot_node_sample(node));
}

char *texture_slot_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	// Already fetched by the Color output
	if (string_array_index_of(parser_material_parsed, parser_material_res_var_name(node, node->outputs->buffer[0])) >= 0) {
		return string_tmp("%s.r", parser_material_store_var_name(node));
	}
	return string_tmp("%s.r", texture_slot_node_sample(node));
}

void texture_slot_node_init() {
	ui_node_t *texture_slot_node_def = ALLOC_INIT(ui_node_t, {.id      = 0,
	                                                          .name    = _tr("Texture Slot"),
	                                                          .type    = "TEX_SLOT",
	                                                          .x       = 0,
	                                                          .y       = 0,
	                                                          .color   = 0xff4982a0,
	                                                          .inputs  = any_array_create_from_raw((void *[]){}, 0),
	                                                          .outputs = any_array_create_from_raw(
	                                                              (void *[]){
	                                                                  ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                                                                                .node_id       = 0,
	                                                                                                .name          = _tr("Color"),
	                                                                                                .type          = "RGBA",
	                                                                                                .color         = 0xffc7c729,
	                                                                                                .default_value = f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0),
	                                                                                                .min           = 0.0,
	                                                                                                .max           = 1.0,
	                                                                                                .precision     = 100,
	                                                                                                .display       = 0}),
	                                                                  ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                                                                                .node_id       = 0,
	                                                                                                .name          = _tr("Value"),
	                                                                                                .type          = "VALUE",
	                                                                                                .color         = 0xffa1a1a1,
	                                                                                                .default_value = f32_array_create_x(1.0),
	                                                                                                .min           = 0.0,
	                                                                                                .max           = 1.0,
	                                                                                                .precision     = 100,
	                                                                                                .display       = 0}),
	                                                              },
	                                                              2),
	                                                          .buttons = any_array_create_from_raw(
	                                                              (void *[]){
	                                                                  ALLOC_INIT(ui_node_button_t, {.name          = "texture_slot_node_button",
	                                                                                                .type          = "CUSTOM",
	                                                                                                .output        = -1,
	                                                                                                .default_value = f32_array_create_x(BTP_AO),
	                                                                                                .data          = NULL,
	                                                                                                .min           = 0.0,
	                                                                                                .max           = 1.0,
	                                                                                                .precision     = 100,
	                                                                                                .height        = 1.5}),
	                                                              },
	                                                              1),
	                                                          .width = 0,
	                                                          .flags = 0});

	any_array_push(nodes_material_texture, texture_slot_node_def);
	any_map_set(parser_material_node_vectors, "TEX_SLOT", texture_slot_node_vector);
	any_map_set(parser_material_node_values, "TEX_SLOT", texture_slot_node_value);
	any_map_set(ui_nodes_custom_buttons, "texture_slot_node_button", texture_slot_node_button);
}
