#include "../global.h"

// Sender Node: publishes the link on its input under a Name, drawn as a dot with the Name next to it (see sender_receiver.c)

void sender_node_init() {

	ui_node_t *sender_node_def = ALLOC_INIT(ui_node_t, {.id     = 0,
	                                                    .name   = _tr("Sender Node"),
	                                                    .type   = "SENDER",
	                                                    .x      = 0,
	                                                    .y      = 0,
	                                                    .color  = 0xffa1a1a1,
	                                                    .inputs = any_array_create_from_raw(
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
	                                                    .outputs = any_array_create_from_raw((void *[]){}, 0),
	                                                    .buttons = any_array_create_from_raw(
	                                                        (void *[]){
	                                                            ALLOC_INIT(ui_node_button_t, {.name          = "Name",
	                                                                                          .type          = "TEXT",
	                                                                                          .output        = -1,
	                                                                                          .default_value = f32_array_create_x(0),
	                                                                                          .data          = u8_array_create_from_string("Sender"),
	                                                                                          .min           = 0.0,
	                                                                                          .max           = 1.0,
	                                                                                          .precision     = 100,
	                                                                                          .height        = 0}),
	                                                        },
	                                                        1),
	                                                    .width = 0,
	                                                    .flags = UI_NODE_FLAG_DOT});

	any_array_push(nodes_material_utilities, sender_node_def);
}
