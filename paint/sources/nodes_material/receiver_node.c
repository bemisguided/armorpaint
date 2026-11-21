#include "../global.h"

// Receiver Node: outputs what the Sender of its Name receives. The parser resolves it to the real link (parser_material_get_input_link),
// the callbacks below only serve a Receiver that is parsed itself, as the start node of a preview.

static char *receiver_node_resolve(ui_node_t *node, bool vector) {
	ui_node_link_t  self = {.id = 99998, .from_id = node->id, .from_socket = 0, .to_id = -1, .to_socket = -1};
	ui_node_link_t *l    = sender_receiver_resolve_link(parser_material_nodes, parser_material_links, &self);
	if (l == NULL) {
		return vector ? "float3(0.0, 0.0, 0.0)" : "0.0";
	}
	ui_node_t *from   = parser_material_get_node(l->from_id);
	char      *res    = parser_material_write_result(l);
	char      *type   = from->outputs->buffer[l->from_socket]->type;
	bool       is_vec = string_equals(type, "RGB") || string_equals(type, "RGBA") || string_equals(type, "VECTOR");
	if (vector) {
		return is_vec ? res : parser_material_to_vec3(res);
	}
	return is_vec ? string_tmp("%s.x", res) : res;
}

char *receiver_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return receiver_node_resolve(node, true);
}

char *receiver_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return receiver_node_resolve(node, false);
}

void receiver_node_init() {

	ui_node_t *receiver_node_def = ALLOC_INIT(ui_node_t, {.id      = 0,
	                                                      .name    = _tr("Receiver Node"),
	                                                      .type    = "RECEIVER",
	                                                      .x       = 0,
	                                                      .y       = 0,
	                                                      .color   = 0xffa1a1a1,
	                                                      .inputs  = any_array_create_from_raw((void *[]){}, 0),
	                                                      .outputs = any_array_create_from_raw(
	                                                          (void *[]){
	                                                              ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                                                                            .node_id       = 0,
	                                                                                            .name          = _tr("Value"),
	                                                                                            .type          = "VALUE",
	                                                                                            .color         = 0xffa1a1a1,
	                                                                                            .default_value = f32_array_create_xyz(0.0, 0.0, 0.0),
	                                                                                            .min           = 0.0,
	                                                                                            .max           = 1.0,
	                                                                                            .precision     = 100,
	                                                                                            .display       = 0}),
	                                                          },
	                                                          1),
	                                                      .buttons = any_array_create_from_raw(
	                                                          (void *[]){
	                                                              ALLOC_INIT(ui_node_button_t, {.name          = "Name",
	                                                                                            .type          = "TEXT",
	                                                                                            .output        = -1,
	                                                                                            .default_value = f32_array_create_x(0),
	                                                                                            .data          = u8_array_create_from_string(""),
	                                                                                            .min           = 0.0,
	                                                                                            .max           = 1.0,
	                                                                                            .precision     = 100,
	                                                                                            .height        = 0}),
	                                                          },
	                                                          1),
	                                                      .width = 0,
	                                                      .flags = UI_NODE_FLAG_DOT});

	any_array_push(nodes_material_utilities, receiver_node_def);
	any_map_set(parser_material_node_vectors, "RECEIVER", receiver_node_vector);
	any_map_set(parser_material_node_values, "RECEIVER", receiver_node_value);
}
