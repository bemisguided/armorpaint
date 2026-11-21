#include "../global.h"

// Sender and Receiver nodes: a Sender publishes the link on its input under a Name, any number of Receivers of the same canvas output it.
// The Name lives in the data of the first button of both nodes, so the project format does not change.

#define SENDER_RECEIVER_LIST_ROWS 8
#define SENDER_RECEIVER_GREY 0xffa1a1a1

bool sender_receiver_is_sender(ui_node_t *n) {
	return string_equals(n->type, "SENDER");
}

bool sender_receiver_is_receiver(ui_node_t *n) {
	return string_equals(n->type, "RECEIVER");
}

char *sender_receiver_name(ui_node_t *n) {
	if (n->buttons->length == 0 || n->buttons->buffer[0]->data == NULL) {
		return "";
	}
	return (char *)n->buttons->buffer[0]->data->buffer;
}

void sender_receiver_set_name(ui_node_t *n, char *name) {
	n->buttons->buffer[0]->data = u8_array_create_from_string(name);
}

ui_node_t *sender_receiver_find_sender(ui_node_t_array_t *nodes, char *name, ui_node_t *except) {
	if (name[0] == '\0') {
		return NULL;
	}
	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *n = nodes->buffer[i];
		if (n != except && sender_receiver_is_sender(n) && string_equals(sender_receiver_name(n), name)) {
			return n;
		}
	}
	return NULL;
}

ui_node_t *sender_receiver_sender_of(ui_node_t_array_t *nodes, ui_node_t *receiver) {
	return sender_receiver_find_sender(nodes, sender_receiver_name(receiver), NULL);
}

// Name that is free among the Senders: Name, then Name.001, Name.002, ... (the number of a name that has one is replaced)
char *sender_receiver_unique_name(ui_node_t_array_t *nodes, char *name, ui_node_t *except) {
	if (name[0] == '\0') {
		name = "Sender";
	}
	if (sender_receiver_find_sender(nodes, name, except) == NULL) {
		return string_copy(name);
	}
	i32  len      = (i32)strlen(name);
	i32  base_len = len;
	bool suffix   = len > 4 && name[len - 4] == '.' && name[len - 3] >= '0' && name[len - 3] <= '9' && name[len - 2] >= '0' && name[len - 2] <= '9' &&
	              name[len - 1] >= '0' && name[len - 1] <= '9';
	if (suffix) {
		base_len = len - 4;
	}
	for (i32 i = 1; i < 100000; ++i) {
		char *candidate = string_tmp("%.*s.%03d", base_len, name, i);
		if (sender_receiver_find_sender(nodes, candidate, except) == NULL) {
			return string_copy(candidate);
		}
	}
	return string_copy(name);
}

// A new or pasted Sender takes a free name
void sender_receiver_on_create(ui_node_t_array_t *nodes, ui_node_t *node) {
	if (sender_receiver_is_sender(node)) {
		sender_receiver_set_name(node, sender_receiver_unique_name(nodes, sender_receiver_name(node), node));
	}
}

// Rename of a Sender in the UI: the Receivers of it follow, the wiring never breaks. Returns the name that was set
char *sender_receiver_rename(ui_node_t_array_t *nodes, ui_node_t *sender, char *name) {
	char *old = string_copy(sender_receiver_name(sender));
	if (name[0] == '\0') {
		return old;
	}
	char *unique = sender_receiver_unique_name(nodes, name, sender);
	sender_receiver_set_name(sender, unique);
	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *n = nodes->buffer[i];
		if (sender_receiver_is_receiver(n) && string_equals(sender_receiver_name(n), old)) {
			sender_receiver_set_name(n, unique);
		}
	}
	return unique;
}

// Pasted or duplicated nodes (already in the canvas): Senders take free names, the Receivers pasted with them follow
void sender_receiver_on_paste(ui_node_t_array_t *nodes, ui_node_t **pasted, i32 count) {
	for (i32 i = 0; i < count; ++i) {
		if (!sender_receiver_is_sender(pasted[i])) {
			continue;
		}
		char *old    = string_copy(sender_receiver_name(pasted[i]));
		char *unique = sender_receiver_unique_name(nodes, old, pasted[i]);
		if (string_equals(old, unique)) {
			continue;
		}
		sender_receiver_set_name(pasted[i], unique);
		for (i32 j = 0; j < count; ++j) {
			if (sender_receiver_is_receiver(pasted[j]) && string_equals(sender_receiver_name(pasted[j]), old)) {
				sender_receiver_set_name(pasted[j], unique);
			}
		}
	}
}

ui_node_link_t *sender_receiver_input_link(ui_node_link_t_array_t *links, ui_node_t *node, i32 socket) {
	for (i32 i = 0; i < links->length; ++i) {
		ui_node_link_t *l = links->buffer[i];
		if (l->to_id == node->id && l->to_socket == socket) {
			return l;
		}
	}
	return NULL;
}

static i32 sender_receiver_index(ui_node_t_array_t *nodes, ui_node_t *node) {
	for (i32 i = 0; i < nodes->length; ++i) {
		if (nodes->buffer[i] == node) {
			return i;
		}
	}
	return -1;
}

// Does the node depend on the target, through any input; a Receiver continues at its Sender
static bool sender_receiver_depends(ui_node_t_array_t *nodes, ui_node_link_t_array_t *links, ui_node_t *node, ui_node_t *target, char *visited) {
	if (node == target) {
		return true;
	}
	i32 index = sender_receiver_index(nodes, node);
	if (index < 0 || visited[index]) {
		return false;
	}
	visited[index] = 1;
	for (i32 i = 0; i < links->length; ++i) {
		ui_node_link_t *l = links->buffer[i];
		if (l->to_id == node->id) {
			ui_node_t *from = ui_get_node(nodes, l->from_id);
			if (from != NULL && sender_receiver_depends(nodes, links, from, target, visited)) {
				return true;
			}
		}
	}
	if (sender_receiver_is_receiver(node)) {
		ui_node_t *sender = sender_receiver_sender_of(nodes, node);
		if (sender != NULL && sender_receiver_depends(nodes, links, sender, target, visited)) {
			return true;
		}
	}
	return false;
}

// Would the Receiver feed its own Sender: the Sender input depends on the Receiver
bool sender_receiver_would_loop(ui_node_t_array_t *nodes, ui_node_link_t_array_t *links, ui_node_t *receiver, ui_node_t *sender) {
	char *visited = calloc(nodes->length + 1, 1);
	bool  loop    = sender_receiver_depends(nodes, links, sender, receiver, visited);
	free(visited);
	return loop;
}

// The link a link really stands for: a link out of a Receiver becomes the input link of its Sender (also chains of them).
// NULL when it ends nowhere: no Sender, nothing linked to it, a loop. The shader only ever sees real links
ui_node_link_t *sender_receiver_resolve_link(ui_node_t_array_t *nodes, ui_node_link_t_array_t *links, ui_node_link_t *l) {
	i32 guard = nodes->length + 1;
	while (l != NULL) {
		ui_node_t *from = ui_get_node(nodes, l->from_id);
		if (from == NULL || !sender_receiver_is_receiver(from)) {
			return l;
		}
		ui_node_t *sender = sender_receiver_sender_of(nodes, from);
		if (sender == NULL || guard-- <= 0 || sender_receiver_would_loop(nodes, links, from, sender)) {
			return NULL;
		}
		l = sender_receiver_input_link(links, sender, 0);
	}
	return NULL;
}

// Choose the Sender of a Receiver, "" for None. A pick that would make a loop is refused
bool sender_receiver_pick(ui_node_t_array_t *nodes, ui_node_link_t_array_t *links, ui_node_t *receiver, char *name) {
	if (name[0] != '\0') {
		ui_node_t *sender = sender_receiver_find_sender(nodes, name, NULL);
		if (sender == NULL || sender_receiver_would_loop(nodes, links, receiver, sender)) {
			return false;
		}
	}
	sender_receiver_set_name(receiver, name);
	return true;
}

// Per canvas state, recomputed when the nodes, the links or the socket colors change: socket types, unresolved Receivers
static i32  sender_receiver_signature = 0;
static i32 *sender_receiver_errors    = NULL; // Ids of the Receivers that are red
static i32  sender_receiver_errors_count = 0;
static char **sender_receiver_selected_names = NULL; // Names of the selected Senders
static i32    sender_receiver_selected_count = 0;

static u32 sender_receiver_hash(u32 h, u32 v) {
	return (h ^ v) * 16777619u;
}

static u32 sender_receiver_hash_string(u32 h, char *s) {
	for (; *s != '\0'; ++s) {
		h = sender_receiver_hash(h, (u8)*s);
	}
	return h;
}

static u32 sender_receiver_canvas_signature(ui_node_t_array_t *nodes, ui_node_link_t_array_t *links) {
	u32 h = 2166136261u;
	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *n = nodes->buffer[i];
		h            = sender_receiver_hash(h, (u32)n->id);
		if (sender_receiver_is_sender(n) || sender_receiver_is_receiver(n)) {
			h = sender_receiver_hash_string(h, sender_receiver_name(n));
		}
		for (i32 j = 0; j < n->outputs->length; ++j) {
			h = sender_receiver_hash(h, n->outputs->buffer[j]->color);
		}
	}
	for (i32 i = 0; i < links->length; ++i) {
		ui_node_link_t *l = links->buffer[i];
		h                 = sender_receiver_hash(h, (u32)l->from_id);
		h                 = sender_receiver_hash(h, (u32)l->from_socket);
		h                 = sender_receiver_hash(h, (u32)l->to_id);
		h                 = sender_receiver_hash(h, (u32)l->to_socket);
	}
	return (i32)h;
}

// Type and color of a socket that follows the link it ends at
static void sender_receiver_follow(ui_node_socket_t *socket, ui_node_t_array_t *nodes, ui_node_link_t *resolved) {
	char    *type  = "VALUE";
	uint32_t color = SENDER_RECEIVER_GREY;
	ui_node_t *from = resolved != NULL ? ui_get_node(nodes, resolved->from_id) : NULL;
	if (from != NULL && resolved->from_socket >= 0 && resolved->from_socket < from->outputs->length) {
		ui_node_socket_t *src = from->outputs->buffer[resolved->from_socket];
		if (string_equals(src->type, "RGB") || string_equals(src->type, "RGBA")) {
			type  = "RGBA";
			color = src->color;
		}
		else if (string_equals(src->type, "VECTOR")) {
			type  = "VECTOR";
			color = src->color;
		}
		else if (string_equals(src->type, "VALUE")) {
			color = src->color;
		}
	}
	socket->type  = type;
	socket->color = color;
}

static bool sender_receiver_error_has(i32 id) {
	for (i32 i = 0; i < sender_receiver_errors_count; ++i) {
		if (sender_receiver_errors[i] == id) {
			return true;
		}
	}
	return false;
}

bool sender_receiver_receiver_unresolved(ui_node_t *receiver) {
	return sender_receiver_error_has(receiver->id);
}

void sender_receiver_update(ui_node_t_array_t *nodes, ui_node_link_t_array_t *links) {
	i32 signature = (i32)sender_receiver_canvas_signature(nodes, links);
	if (signature == sender_receiver_signature) {
		return;
	}
	sender_receiver_signature = signature;

	i32  old_count  = sender_receiver_errors_count;
	i32 *old_errors = sender_receiver_errors;
	sender_receiver_errors       = calloc(nodes->length + 1, sizeof(i32));
	sender_receiver_errors_count = 0;

	for (i32 i = 0; i < nodes->length; ++i) {
		ui_node_t *n = nodes->buffer[i];
		if (sender_receiver_is_sender(n)) {
			ui_node_link_t *l = sender_receiver_resolve_link(nodes, links, sender_receiver_input_link(links, n, 0));
			sender_receiver_follow(n->inputs->buffer[0], nodes, l);
		}
		else if (sender_receiver_is_receiver(n)) {
			char      *name   = sender_receiver_name(n);
			ui_node_t *sender = sender_receiver_sender_of(nodes, n);
			ui_node_link_t *l = NULL;
			if (sender != NULL && !sender_receiver_would_loop(nodes, links, n, sender)) {
				l = sender_receiver_resolve_link(nodes, links, sender_receiver_input_link(links, sender, 0));
			}
			sender_receiver_follow(n->outputs->buffer[0], nodes, l);
			if (name[0] != '\0' && (sender == NULL || sender_receiver_would_loop(nodes, links, n, sender))) {
				sender_receiver_errors[sender_receiver_errors_count++] = n->id;
				bool known                                             = false;
				for (i32 j = 0; j < old_count; ++j) {
					known = known || old_errors[j] == n->id;
				}
				if (!known) { // Once per Receiver
					if (sender == NULL) {
						console_log(string("Receiver has no Sender named %s", name));
					}
					else {
						console_error(string("Receiver loop, %s depends on its own Receiver", name));
					}
				}
			}
			else if (name[0] == '\0') {
				// Nothing chosen is red as well, without a message
				sender_receiver_errors[sender_receiver_errors_count++] = n->id;
			}
		}
	}
	free(old_errors);
}

char *sender_receiver_dot_label(ui_node_t *node) {
	if (sender_receiver_is_sender(node)) {
		return sender_receiver_name(node);
	}
	if (sender_receiver_is_receiver(node)) {
		char *name = sender_receiver_name(node);
		return name[0] != '\0' ? name : tr("None");
	}
	return NULL;
}

int sender_receiver_dot_state(ui_node_t *node) {
	if (!sender_receiver_is_receiver(node)) {
		return 0;
	}
	int state = 0;
	if (sender_receiver_receiver_unresolved(node)) {
		state |= UI_DOT_ERROR;
	}
	for (i32 i = 0; i < sender_receiver_selected_count; ++i) {
		if (string_equals(sender_receiver_selected_names[i], sender_receiver_name(node))) {
			state |= UI_DOT_PARTNER;
		}
	}
	return state;
}

static void sender_receiver_draw_line(ui_node_t *a, ui_node_t *b) {
	float ax;
	float ay;
	float bx;
	float by;
	ui_node_dot_center(a, &ax, &ay);
	ui_node_dot_center(b, &bx, &by);
	ui_draw_link_dotted(ax, ay, bx, by, g_theme->HIGHLIGHT_COL);
}

// Called by the node editor after the nodes are drawn and before the dot nodes: state per frame and the dotted lines of the selection
void sender_receiver_overlay(ui_node_canvas_t *canvas) {
	ui_node_t_array_t *nodes = canvas->nodes;
	sender_receiver_update(nodes, canvas->links);

	ui_nodes_t *ui_nodes = ui_nodes_get_nodes();
	free(sender_receiver_selected_names);
	sender_receiver_selected_names = calloc(ui_nodes->nodes_selected_id->length + 1, sizeof(char *));
	sender_receiver_selected_count = 0;
	for (i32 i = 0; i < ui_nodes->nodes_selected_id->length; ++i) {
		ui_node_t *selected = ui_get_node(nodes, ui_nodes->nodes_selected_id->buffer[i]);
		if (selected == NULL) {
			continue;
		}
		if (sender_receiver_is_sender(selected)) {
			sender_receiver_selected_names[sender_receiver_selected_count++] = sender_receiver_name(selected);
			for (i32 j = 0; j < nodes->length; ++j) {
				ui_node_t *r = nodes->buffer[j];
				if (sender_receiver_is_receiver(r) && string_equals(sender_receiver_name(r), sender_receiver_name(selected))) {
					sender_receiver_draw_line(selected, r);
				}
			}
		}
		else if (sender_receiver_is_receiver(selected)) {
			ui_node_t *sender = sender_receiver_sender_of(nodes, selected);
			if (sender != NULL) {
				sender_receiver_draw_line(selected, sender);
			}
		}
	}
}

// Receiver list: a filter field on top, None and the Sender names, a row limit with a scrollbar

static i32   sender_receiver_list_node = -1;
static char *sender_receiver_list_filter;
static i32   sender_receiver_list_sel;
static i32   sender_receiver_list_scroll;
static bool  sender_receiver_list_first;
static bool  sender_receiver_list_at_current;

// Lower case copy in the frame temp memory
static char *sender_receiver_lower(char *s) {
	i32   len = (i32)strlen(s);
	char *r   = string_tmp_alloc(len + 1);
	for (i32 i = 0; i <= len; ++i) {
		r[i] = (char)tolower((unsigned char)s[i]);
	}
	return r;
}

static void sender_receiver_list_commands(ui_t *ui, void *data, void *data2) {
	ui_node_canvas_t *canvas   = ui_nodes_get_canvas(true);
	ui_node_t        *receiver = ui_get_node(canvas->nodes, sender_receiver_list_node);
	if (receiver == NULL) {
		ui_popup_commands = NULL;
		return;
	}

	// Filter field
	if (sender_receiver_list_first) {
		sender_receiver_list_first = false;
		ui_start_text_edit(&sender_receiver_list_filter, UI_ALIGN_LEFT);
	}
	f32 field_x = ui->_x;
	f32 field_y = ui->_y;
	ui_text_input(&sender_receiver_list_filter, "", UI_ALIGN_LEFT, true, true);
	if (sender_receiver_list_filter[0] == '\0') {
		draw_set_color(ui->ops->theme->HOVER_COL);
		draw_string(tr("Filter..."), field_x + ui->ops->theme->TEXT_OFFSET * UI_SCALE(), field_y + (UI_ELEMENT_H() - draw_font_height(ui->ops->font, ui->font_size)) / 2.0);
	}

	// Rows: None while no filter is typed, then the Sender names alphabetical, filtered by substring
	char *filter = sender_receiver_lower(sender_receiver_list_filter);
	i32   total  = 0;
	i32   senders = 0;
	for (i32 i = 0; i < canvas->nodes->length; ++i) {
		senders += sender_receiver_is_sender(canvas->nodes->buffer[i]) ? 1 : 0;
	}
	char **names = calloc(senders + 1, sizeof(char *)); // Sender names, in the list order
	for (i32 i = 0; i < canvas->nodes->length; ++i) {
		ui_node_t *n = canvas->nodes->buffer[i];
		if (sender_receiver_is_sender(n) && strstr(sender_receiver_lower(sender_receiver_name(n)), filter) != NULL) {
			names[total++] = sender_receiver_name(n);
		}
	}
	// Sort by lower case name, the shown name stays as typed
	char **keys = calloc(total + 1, sizeof(char *));
	for (i32 i = 0; i < total; ++i) {
		keys[i] = sender_receiver_lower(names[i]);
	}
	for (i32 i = 1; i < total; ++i) { // Insertion sort, stable and small
		for (i32 j = i; j > 0 && strcmp(keys[j - 1], keys[j]) > 0; --j) {
			char *k = keys[j];
			keys[j]     = keys[j - 1];
			keys[j - 1] = k;
			char *s     = names[j];
			names[j]     = names[j - 1];
			names[j - 1] = s;
		}
	}
	bool with_none = sender_receiver_list_filter[0] == '\0';
	i32  rows      = total + (with_none ? 1 : 0);
	if (sender_receiver_list_at_current) { // First frame: start at the row of the current Sender
		sender_receiver_list_at_current = false;
		for (i32 i = 0; i < total; ++i) {
			if (string_equals(names[i], sender_receiver_name(receiver))) {
				sender_receiver_list_sel = i + (with_none ? 1 : 0);
			}
		}
	}
	i32  shown     = rows < SENDER_RECEIVER_LIST_ROWS ? rows : SENDER_RECEIVER_LIST_ROWS;
	if (rows == 0) {
		shown = 1; // No matches
	}

	// Keys
	if (ui->is_key_pressed) {
		if (ui->key_code == KEY_CODE_DOWN && sender_receiver_list_sel < rows - 1) {
			sender_receiver_list_sel++;
		}
		else if (ui->key_code == KEY_CODE_UP && sender_receiver_list_sel > 0) {
			sender_receiver_list_sel--;
		}
	}
	if (sender_receiver_list_sel > rows - 1) {
		sender_receiver_list_sel = rows - 1;
	}
	if (sender_receiver_list_sel < 0) {
		sender_receiver_list_sel = 0;
	}
	if (sender_receiver_list_sel < sender_receiver_list_scroll) {
		sender_receiver_list_scroll = sender_receiver_list_sel;
	}
	if (sender_receiver_list_sel >= sender_receiver_list_scroll + shown) {
		sender_receiver_list_scroll = sender_receiver_list_sel - shown + 1;
	}
	if (rows > shown) {
		if (ui->input_wheel_delta != 0 && ui_nodes_popup_hovered()) {
			sender_receiver_list_scroll += ui->input_wheel_delta > 0 ? 1 : -1;
		}
	}
	i32 max_scroll = rows > shown ? rows - shown : 0;
	if (sender_receiver_list_scroll > max_scroll) {
		sender_receiver_list_scroll = max_scroll;
	}
	if (sender_receiver_list_scroll < 0) {
		sender_receiver_list_scroll = 0;
	}

	// Rows
	f32  rows_x   = ui->_x;
	f32  rows_y   = ui->_y;
	i32  picked   = -2; // -2 nothing, -1 None, else the index into names
	bool enter    = ui->is_key_pressed && ui->key_code == KEY_CODE_RETURN;
	u32  button_col = ui->ops->theme->BUTTON_COL;
	if (rows == 0) {
		ui_text(tr("No matches"), UI_ALIGN_LEFT, 0);
	}
	for (i32 r = sender_receiver_list_scroll; r < sender_receiver_list_scroll + shown && r < rows; ++r) {
		bool  none = with_none && r == 0;
		i32   idx  = with_none ? r - 1 : r;
		char *text = none ? tr("None") : names[idx];
		ui->ops->theme->BUTTON_COL = r == sender_receiver_list_sel ? ui->ops->theme->HIGHLIGHT_COL : button_col;
		if (ui_button(text, UI_ALIGN_LEFT, "")) {
			picked = none ? -1 : idx;
		}
		if (enter && r == sender_receiver_list_sel) {
			picked = none ? -1 : idx;
		}
	}
	ui->ops->theme->BUTTON_COL = button_col;

	// Scrollbar, only past the row limit
	if (rows > shown) {
		f32 row_h   = UI_ELEMENT_H() + UI_ELEMENT_OFFSET();
		f32 track_h = row_h * shown;
		f32 track_x = rows_x + ui->_w - 6 * UI_SCALE();
		f32 handle_h = track_h * shown / rows;
		if (ui->input_down && ui_nodes_popup_hovered() && ui->input_x > track_x - 2 * UI_SCALE() && ui->input_y >= rows_y && ui->input_y <= rows_y + track_h) {
			f32 t                       = (ui->input_y - rows_y - handle_h / 2.0) / (track_h - handle_h);
			sender_receiver_list_scroll = (i32)(t * max_scroll + 0.5);
			if (sender_receiver_list_scroll < 0) {
				sender_receiver_list_scroll = 0;
			}
			if (sender_receiver_list_scroll > max_scroll) {
				sender_receiver_list_scroll = max_scroll;
			}
		}
		draw_set_color(ui->ops->theme->SEPARATOR_COL);
		draw_filled_rect(track_x, rows_y, 4 * UI_SCALE(), track_h);
		draw_set_color(ui->ops->theme->HOVER_COL);
		draw_filled_rect(track_x, rows_y + (track_h - handle_h) * sender_receiver_list_scroll / max_scroll, 4 * UI_SCALE(), handle_h);
	}

	// Size of the popup follows the rows
	ui_popup_h = (1 + shown) * (ui->ops->theme->ELEMENT_H + ui->ops->theme->ELEMENT_OFFSET);

	if (picked != -2) {
		ui_popup_commands = NULL;
		ui_node_t_array_t *all_nodes = canvas->nodes;
		char              *name      = picked == -1 ? "" : string_copy(names[picked]);
		if (sender_receiver_pick(all_nodes, canvas->links, receiver, name)) {
			ui->changed = true;
		}
		else {
			console_error(string("Receiver loop, %s depends on its own Receiver", name));
		}
	}
	free(names);
	free(keys);
}

static void sender_receiver_open_list(ui_node_t *receiver) {
	ui_t *ui = ui_get_current();
	sender_receiver_list_node   = receiver->id;
	sender_receiver_list_filter = "";
	sender_receiver_list_sel    = 0;
	sender_receiver_list_scroll = 0;
	sender_receiver_list_first  = true;
	sender_receiver_list_at_current = true;

	f32 scale = ui_nodes_get_nodes()->scale_factor;
	f32 w     = 200 * scale;
	f32 h     = 9 * (ui->ops->theme->ELEMENT_H + ui->ops->theme->ELEMENT_OFFSET) * scale;
	f32 x     = ui->_window_x + UI_NODE_X(receiver) + UI_NODE_W(receiver) + 14 * UI_NODES_SCALE();
	f32 y     = ui->_window_y + UI_NODE_Y(receiver) - 4 * UI_NODES_SCALE();
	if (x + w > iron_window_width()) {
		x = ui->_window_x + UI_NODE_X(receiver) - 14 * UI_NODES_SCALE() - w; // Flip to the left of the Receiver
	}
	if (x < 0) {
		x = 0;
	}
	if (y + h > iron_window_height()) {
		y = iron_window_height() - h;
	}
	if (y < 0) {
		y = 0;
	}
	ui_popup((i32)x, (i32)y, (i32)w, 9 * (ui->ops->theme->ELEMENT_H + ui->ops->theme->ELEMENT_OFFSET), &sender_receiver_list_commands, NULL, NULL);
	ui_popup_handle_node_id = receiver->id;
	ui->input_started       = false; // The press that opened the list must not close it
}

void sender_receiver_on_label_double_click(ui_node_t *node) {
	if (sender_receiver_is_sender(node)) {
		ui_nodes_dot_edit_label(node);
	}
	else if (sender_receiver_is_receiver(node)) {
		sender_receiver_open_list(node);
	}
}

void sender_receiver_on_label_edited(ui_node_t *node, char *text) {
	if (!sender_receiver_is_sender(node)) {
		return;
	}
	ui_node_canvas_t *canvas = ui_nodes_get_canvas(true);
	sender_receiver_rename(canvas->nodes, node, text);
	g_ui->changed = true;
	ui_viewnodes_on_node_changed(node);
}

void sender_receiver_on_paste_hook(ui_node_canvas_t *canvas, ui_node_t **pasted, i32 count) {
	sender_receiver_on_paste(canvas->nodes, pasted, count);
}

void sender_receiver_init() {
	ui_nodes_on_dot_label              = sender_receiver_dot_label;
	ui_nodes_on_dot_state              = sender_receiver_dot_state;
	ui_nodes_on_dot_overlay            = sender_receiver_overlay;
	ui_nodes_on_dot_label_double_click = sender_receiver_on_label_double_click;
	ui_nodes_on_dot_label_edited       = sender_receiver_on_label_edited;
	ui_nodes_on_paste                  = sender_receiver_on_paste_hook;
}
