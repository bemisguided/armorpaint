
#include "../global.h"

static void color_ramp_sort(i32 *sorted, i32 len, f32_array_t *elems) {
	for (i32 i = 1; i < len; i++) {
		i32 key     = sorted[i];
		f32 key_pos = elems->buffer[key * 5 + 4];
		i32 j       = i - 1;
		while (j >= 0 && elems->buffer[sorted[j] * 5 + 4] > key_pos) {
			sorted[j + 1] = sorted[j];
			j--;
		}
		sorted[j + 1] = key;
	}
}

static void color_ramp_eval_cpu(f32_array_t *vals, i32 *sorted, i32 len, i32 interp, f32 t, f32 *out_r, f32 *out_g, f32 *out_b, f32 *out_a) {
	if (len == 1) {
		i32 s0 = sorted[0];
		*out_r = vals->buffer[s0 * 5 + 0];
		*out_g = vals->buffer[s0 * 5 + 1];
		*out_b = vals->buffer[s0 * 5 + 2];
		*out_a = vals->buffer[s0 * 5 + 3];
		return;
	}
	// Find index: count stops whose position is less than t
	i32 idx = 0;
	for (i32 i = 1; i < len; i++) {
		if (t > vals->buffer[sorted[i] * 5 + 4]) {
			idx = i;
		}
	}
	if (interp == 1) { // Constant
		i32 s  = sorted[idx];
		*out_r = vals->buffer[s * 5 + 0];
		*out_g = vals->buffer[s * 5 + 1];
		*out_b = vals->buffer[s * 5 + 2];
		*out_a = vals->buffer[s * 5 + 3];
	}
	else { // Linear
		if (idx >= len - 1) {
			i32 s  = sorted[len - 1];
			*out_r = vals->buffer[s * 5 + 0];
			*out_g = vals->buffer[s * 5 + 1];
			*out_b = vals->buffer[s * 5 + 2];
			*out_a = vals->buffer[s * 5 + 3];
		}
		else {
			i32 s0    = sorted[idx];
			i32 s1    = sorted[idx + 1];
			f32 pos0  = vals->buffer[s0 * 5 + 4];
			f32 pos1  = vals->buffer[s1 * 5 + 4];
			f32 range = pos1 - pos0;
			f32 f     = range > 0.00001f ? (t - pos0) / range : 0.0f;
			if (f < 0.0f)
				f = 0.0f;
			if (f > 1.0f)
				f = 1.0f;
			*out_r = vals->buffer[s0 * 5 + 0] * (1.0f - f) + vals->buffer[s1 * 5 + 0] * f;
			*out_g = vals->buffer[s0 * 5 + 1] * (1.0f - f) + vals->buffer[s1 * 5 + 1] * f;
			*out_b = vals->buffer[s0 * 5 + 2] * (1.0f - f) + vals->buffer[s1 * 5 + 2] * f;
			*out_a = vals->buffer[s0 * 5 + 3] * (1.0f - f) + vals->buffer[s1 * 5 + 3] * f;
		}
	}
}

static char *color_ramp_eval(ui_node_t *node) {
	char *store_var = parser_material_store_var_name(node);
	if (string_array_index_of(parser_material_parsed, store_var) >= 0) {
		return store_var;
	}
	any_array_push(parser_material_parsed, store_var);

	char        *fac    = parser_material_parse_value_input(node->inputs->buffer[0], false);
	i32          interp = node->buttons->buffer[0]->data->buffer[0]; // 0=linear, 1=constant
	f32_array_t *elems  = node->buttons->buffer[0]->default_value;
	i32          len    = (i32)(elems->length / 5);

	if (len <= 0) {
		parser_material_write(parser_material_kong, string_tmp("float4 %s = float4(0.0, 0.0, 0.0, 1.0);", store_var));
		return store_var;
	}

	// Sort stop indices by position
	i32 sorted[len];
	for (i32 i = 0; i < len; i++)
		sorted[i] = i;
	color_ramp_sort(sorted, len, elems);

	char *base    = parser_material_node_name(node, NULL);
	char *fac_var = string_tmp("%s_fac", base);
	parser_material_write(parser_material_kong, string_tmp("float %s = %s;", fac_var, fac));

	// Declare one float4 variable per stop: {base}_c0, {base}_c1, ...
	for (i32 i = 0; i < len; i++) {
		i32   s = sorted[i];
		char *r = f32_to_string_with_zeros(elems->buffer[s * 5 + 0]);
		char *g = f32_to_string_with_zeros(elems->buffer[s * 5 + 1]);
		char *b = f32_to_string_with_zeros(elems->buffer[s * 5 + 2]);
		char *a = f32_to_string_with_zeros(elems->buffer[s * 5 + 3]);
		parser_material_write(parser_material_kong, string_tmp("float4 %s_c%d = float4(%s, %s, %s, %s);", base, i, r, g, b, a));
	}

	if (len == 1) {
		parser_material_write(parser_material_kong, string_tmp("float4 %s = %s_c0;", store_var, base));
		return store_var;
	}

	if (interp == 1) { // Constant
		parser_material_write(parser_material_kong, string_tmp("float4 %s = %s_c0;", store_var, base));
		for (i32 i = 1; i < len; i++) {
			char *pi = f32_to_string_with_zeros(elems->buffer[sorted[i] * 5 + 4]);
			parser_material_write(parser_material_kong, string_tmp("if (%s > %s) { %s = %s_c%d; }", fac_var, pi, store_var, base, i));
		}
	}
	else { // Linear
		f32   p0  = elems->buffer[sorted[0] * 5 + 4];
		f32   p1  = elems->buffer[sorted[1] * 5 + 4];
		char *b01 = string_tmp("clamp((%s - %s) / max(%s, 0.00001), 0.0, 1.0)", fac_var, f32_to_string_with_zeros(p0), f32_to_string_with_zeros(p1 - p0));
		parser_material_write(parser_material_kong, string_tmp("float4 %s = lerp(%s_c0, %s_c1, %s);", store_var, base, base, b01));
		for (i32 i = 1; i < len - 1; i++) {
			f32   pi  = elems->buffer[sorted[i] * 5 + 4];
			f32   pi1 = elems->buffer[sorted[i + 1] * 5 + 4];
			char *blend =
			    string_tmp("clamp((%s - %s) / max(%s, 0.00001), 0.0, 1.0)", fac_var, f32_to_string_with_zeros(pi), f32_to_string_with_zeros(pi1 - pi));
			char *pi_s = f32_to_string_with_zeros(pi);
			parser_material_write(parser_material_kong,
			                      string_tmp("if (%s > %s) { %s = lerp(%s_c%d, %s_c%d, %s); }", fac_var, pi_s, store_var, base, i, base, i + 1, blend));
		}
	}

	return store_var;
}

char *color_ramp_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	char *store_var = color_ramp_eval(node);
	return string_tmp("%s.xyz", store_var);
}

char *color_ramp_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	char *store_var = color_ramp_eval(node);
	return string_tmp("%s.w", store_var);
}

// Stops control of the node: the ramp data and its interpolation, positions are buffer[i * 5 + 4]
typedef struct color_ramp_stops {
	f32_array_t *vals;
	i32          interp;
} color_ramp_stops_t;

#define COLOR_RAMP_MIN_STOPS 1
#define COLOR_RAMP_MAX_STOPS 32

static i32 color_ramp_stops_count(void *data) {
	return (i32)(((color_ramp_stops_t *)data)->vals->length / 5);
}

static f32 color_ramp_stops_get_pos(void *data, i32 i) {
	return ((color_ramp_stops_t *)data)->vals->buffer[i * 5 + 4];
}

static void color_ramp_stops_set_pos(void *data, i32 i, f32 pos) {
	((color_ramp_stops_t *)data)->vals->buffer[i * 5 + 4] = pos;
}

// Adds a stop at pos with the colour and alpha the ramp has there, returns its index or -1
static i32 color_ramp_stops_add(void *data, f32 pos) {
	color_ramp_stops_t *ramp = data;
	i32                 len  = (i32)(ramp->vals->length / 5);
	if (len >= COLOR_RAMP_MAX_STOPS) {
		return -1;
	}
	f32 r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
	if (len > 0) {
		i32 sorted[len];
		for (i32 i = 0; i < len; i++)
			sorted[i] = i;
		color_ramp_sort(sorted, len, ramp->vals);
		color_ramp_eval_cpu(ramp->vals, sorted, len, ramp->interp, pos, &r, &g, &b, &a);
	}
	f32_array_push(ramp->vals, r);
	f32_array_push(ramp->vals, g);
	f32_array_push(ramp->vals, b);
	f32_array_push(ramp->vals, a);
	f32_array_push(ramp->vals, pos);
	return len;
}

static void color_ramp_stops_remove(void *data, i32 i) {
	f32_array_t *vals = ((color_ramp_stops_t *)data)->vals;
	for (i32 j = i * 5; j + 5 < vals->length; j++) {
		vals->buffer[j] = vals->buffer[j + 5];
	}
	vals->length -= 5;
}

static void color_ramp_stops_draw(void *data, i32 i, f32 cx, f32 y, f32 w, f32 h, bool active) {
	f32_array_t *vals = ((color_ramp_stops_t *)data)->vals;
	ui_stops_draw_pin(cx, y, w, h, color_from_floats(vals->buffer[i * 5 + 0], vals->buffer[i * 5 + 1], vals->buffer[i * 5 + 2], 1.0), active);
}

void nodes_material_color_ramp_button(i32 node_id) {
	ui_nodes_t              *nodes  = ui_nodes_get_nodes();
	ui_node_t               *node   = ui_get_node(ui_nodes_get_canvas(true)->nodes, node_id);
	ui_node_button_t        *but    = node->buttons->buffer[0];
	ui_nodes_editor_state_t *state  = ui_nodes_editor_state(node);
	f32                      nx     = g_ui->_x;
	f32                      ny     = g_ui->_y;
	f32_array_t             *vals   = but->default_value; // [r, g, b, a, pos, r, g, b, a, pos, ..]
	f32                      sw     = g_ui->_w / (float)UI_NODES_SCALE();
	if (vals->length < 5) { // The handles need a stop to work with
		f32 black[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
		for (i32 i = 0; i < 5; i++)
			f32_array_push(vals, black[i]);
	}
	i32 len    = (i32)(vals->length / 5);
	i32 interp = but->data->buffer[0];

	// Sort stops by position for preview
	i32 sorted[len];
	for (i32 i = 0; i < len; i++)
		sorted[i] = i;
	color_ramp_sort(sorted, len, vals);

	// Gradient preview
	f32 bar_h    = UI_LINE_H() - 2 * UI_NODES_SCALE();
	f32 marker_h = UI_LINE_H() * 0.5f; // Drawn handle row
	f32 hit_h    = UI_LINE_H() * 0.7f; // Click zone of the handles, larger than the drawn handle
	f32 gap_h    = UI_LINE_H() * 0.3f; // Space between the handles and the controls below
	i32 slices   = 64;
	for (i32 s = 0; s < slices; s++) {
		f32 t = (f32)s / (f32)slices;
		f32 r, g, b, a;
		color_ramp_eval_cpu(vals, sorted, len, interp, t, &r, &g, &b, &a);
		i32 col     = color_from_floats(r, g, b, 1.0);
		f32 slice_w = sw / (f32)slices + 1.0f / UI_NODES_SCALE();
		ui_fill(t * sw, 0, slice_w, bar_h / UI_SCALE(), col);
	}

	// Handles below the bar: select, drag, add on the bar, double click deletes
	color_ramp_stops_t  ramp = {.vals = vals, .interp = interp};
	ui_stops_ops_t      ops  = {.count     = color_ramp_stops_count,
	                            .get_pos   = color_ramp_stops_get_pos,
	                            .set_pos   = color_ramp_stops_set_pos,
	                            .add       = color_ramp_stops_add,
	                            .remove    = color_ramp_stops_remove,
	                            .draw      = color_ramp_stops_draw,
	                            .min_count = COLOR_RAMP_MIN_STOPS,
	                            .max_count = COLOR_RAMP_MAX_STOPS};
	i32                 hit  = ui_stops_update(&state->stops, &ops, &ramp, g_ui->_x, g_ui->_y, g_ui->_w, bar_h, marker_h, hit_h);
	if (hit & UI_STOPS_CONSUMED) {
		nodes->_input_started = false;
		g_context->select_time = 0; // Two quick presses on handles are not a node double click (2D view)
	}
	g_ui->_y += UI_LINE_H() + marker_h + gap_h;

	// Edit controls
	f32_array_t *row = f32_array_create_from_raw_tmp(
	    (f32[]){
	        1 / 4.0,
	        1 / 4.0,
	        2 / 4.0,
	    },
	    3);
	ui_row(row);
	bool add_stop = ui_button("+", UI_ALIGN_CENTER, "");
	if (g_ui->is_hovered) {
		ui_tooltip(tr("Add stop"));
	}
	if (add_stop) {
		i32 added = color_ramp_stops_add(&ramp, ui_stops_gap_pos(color_ramp_stops_get_pos, &ramp, color_ramp_stops_count(&ramp)));
		if (added >= 0) {
			state->stops.active = added;
		}
	}
	bool delete_stop = ui_button("-", UI_ALIGN_CENTER, "");
	if (g_ui->is_hovered) {
		ui_tooltip(tr("Delete active stop"));
	}
	if (delete_stop && ui_stops_can_remove(color_ramp_stops_count(&ramp), COLOR_RAMP_MIN_STOPS)) {
		i32 active = state->stops.active;
		color_ramp_stops_remove(&ramp, active);
		state->stops.active = ui_stops_after_remove(active, active, color_ramp_stops_count(&ramp));
	}
	state->interp                     = but->data->buffer[0];
	string_array_t *interpolate_combo = any_array_create_from_raw_tmp(
	    (void *[]){
	        tr("Linear"),
	        tr("Constant"),
	    },
	    2);
	but->data->buffer[0] = ui_combo(&state->interp, interpolate_combo, tr("Interpolate"), false, UI_ALIGN_LEFT, true);

	// Active stop: position and colour
	f32_array_t *row2 = f32_array_create_from_raw_tmp(
	    (f32[]){
	        3 / 4.0,
	        1 / 4.0,
	    },
	    2);
	ui_row(row2);
	i32 i = state->stops.active;
	if (i >= color_ramp_stops_count(&ramp) || i < 0) {
		state->stops.active = i = color_ramp_stops_count(&ramp) - 1;
	}
	state->x                = vals->buffer[i * 5 + 4];
	vals->buffer[i * 5 + 4] = ui_slider(&state->x, "Pos", 0, 1, true, 100, true, UI_ALIGN_LEFT, true);
	if (vals->buffer[i * 5 + 4] > 1.0) {
		vals->buffer[i * 5 + 4] = 1.0;
	}
	else if (vals->buffer[i * 5 + 4] < 0.0) {
		vals->buffer[i * 5 + 4] = 0.0;
	}
	u32 color = color_from_floats(vals->buffer[i * 5 + 0], vals->buffer[i * 5 + 1], vals->buffer[i * 5 + 2], 1.0);
	if (ui_text("", UI_ALIGN_RIGHT, color) == UI_STATE_STARTED) {
		f32 rx                = nx + g_ui->_w - ui_p(37);
		f32 ry                = ny - ui_p(5);
		nodes->_input_started = g_ui->input_started = false;
		ui_nodes_rgba_popup(&state->color_state, vals->buffer + i * 5, math_floor(rx), math_floor(ry + UI_ELEMENT_H()));
	}
}

void color_ramp_node_init() {

	ui_node_t *color_ramp_node_def =
	    ALLOC_INIT(ui_node_t, {.id     = 0,
	                           .name   = _tr("Color Ramp"),
	                           .type   = "VALTORGB",
	                           .x      = 0,
	                           .y      = 0,
	                           .color  = 0xff62676d,
	                           .inputs = any_array_create_from_raw(
	                               (void *[]){
	                                   ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                                                 .node_id       = 0,
	                                                                 .name          = _tr("Factor"),
	                                                                 .type          = "VALUE",
	                                                                 .color         = 0xffa1a1a1,
	                                                                 .default_value = f32_array_create_x(0.5),
	                                                                 .min           = 0.0,
	                                                                 .max           = 1.0,
	                                                                 .precision     = 100,
	                                                                 .display       = 0}),
	                               },
	                               1),
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
	                                                                 .name          = _tr("Alpha"),
	                                                                 .type          = "VALUE",
	                                                                 .color         = 0xffa1a1a1,
	                                                                 .default_value = f32_array_create_x(0.0),
	                                                                 .min           = 0.0,
	                                                                 .max           = 1.0,
	                                                                 .precision     = 100,
	                                                                 .display       = 0}),
	                               },
	                               2),
	                           .buttons = any_array_create_from_raw(
	                               (void *[]){
	                                   ALLOC_INIT(ui_node_button_t,
	                                              {.name          = "nodes_material_color_ramp_button",
	                                               .type          = "CUSTOM",
	                                               .output        = 0,
	                                               .default_value = f32_array_create_from_raw((f32[]){0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0}, 10),
	                                               .data          = u8_array_create(1),
	                                               .min           = 0.0,
	                                               .max           = 1.0,
	                                               .precision     = 100,
	                                               .height        = 4.1}),
	                               },
	                               1),
	                           .width = 0,
	                           .flags = 0});

	any_array_push(nodes_material_color, color_ramp_node_def);
	any_map_set(parser_material_node_vectors, "VALTORGB", color_ramp_node_vector);
	any_map_set(parser_material_node_values, "VALTORGB", color_ramp_node_value);
	any_map_set(ui_nodes_custom_buttons, "nodes_material_color_ramp_button", nodes_material_color_ramp_button);
}
