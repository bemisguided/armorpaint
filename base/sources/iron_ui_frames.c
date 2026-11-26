// Node frames: a labelled, coloured box that groups nodes in the node editor.
//
// A frame is an ordinary node of the type FRAME without sockets. Nothing is added to the saved node struct: the label is the node name, the colour is
// the node colour, x, y and width are the frame rectangle. The height and the member node ids live in the data of one hidden button of the frame, as
// the text "<height> <id> <id> ..." (a project saved by a build without frames shows an empty node and keeps the data).
// The rectangle is derived from the members (ui_frame_fit), so x, y and width are always equal to the computed values.
// Frames do not nest: a frame is never a member of a frame (a file that says so is ignored and healed), a node is a member of at most one frame.

#include "iron_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UI_FRAME_BUTTON_TYPE "FRAME"

const uint32_t ui_frame_palette[UI_FRAME_PALETTE_COUNT] = {
    0xff4982a0, // Blue
    0xffb34f5a, // Red
    0xff62676d, // Grey
    0xff448c6d, // Green
    0xff522c99, // Purple
    0xffc78a29, // Orange
    0xff3a9a96, // Teal
};

static char *ui_frame_strdup(const char *s) {
	size_t n = strlen(s) + 1;
	char  *r = malloc(n);
	memcpy(r, s, n);
	return r;
}

bool ui_node_is_frame(ui_node_t *n) {
	return n != NULL && n->type != NULL && strcmp(n->type, UI_FRAME_TYPE) == 0;
}

static ui_node_t *ui_frame_find(ui_node_canvas_t *c, int id) {
	for (int i = 0; i < c->nodes->length; ++i) {
		if (c->nodes->buffer[i]->id == id) {
			return c->nodes->buffer[i];
		}
	}
	return NULL;
}

static ui_node_button_t *ui_frame_button(ui_node_t *f) {
	if (f->buttons == NULL) {
		return NULL;
	}
	for (int i = 0; i < f->buttons->length; ++i) {
		ui_node_button_t *b = f->buttons->buffer[i];
		if (b->type != NULL && strcmp(b->type, UI_FRAME_BUTTON_TYPE) == 0) {
			return b;
		}
	}
	return NULL;
}

// Reads the hidden text, ids are written to ids (at most max), returns the member count and the height through height (0 when missing)
static int ui_frame_read(ui_node_t *f, int *ids, int max, float *height) {
	if (height != NULL) {
		*height = 0.0f;
	}
	ui_node_button_t *b = ui_frame_button(f);
	if (b == NULL || b->data == NULL || b->data->buffer == NULL) {
		return 0;
	}
	const char *s     = (const char *)b->data->buffer;
	int         len   = b->data->length;
	int         pos   = 0;
	int         count = 0;
	bool        first = true;
	while (pos < len) {
		while (pos < len && (s[pos] == ' ' || s[pos] == '\n')) {
			pos++;
		}
		if (pos >= len || s[pos] == '\0') {
			break;
		}
		char tok[64];
		int  n = 0;
		while (pos < len && s[pos] != ' ' && s[pos] != '\n' && s[pos] != '\0') {
			if (n < 63) {
				tok[n++] = s[pos];
			}
			pos++;
		}
		tok[n] = '\0';
		if (first) {
			first = false;
			if (height != NULL) {
				float h = (float)atof(tok);
				*height = (isfinite(h) && h > 0.0f && h < UI_FRAME_MAX_SIZE) ? h : 0.0f; // From a file: nan, inf and absurd heights read as missing
			}
		}
		else if (ids != NULL && count < max) {
			ids[count++] = atoi(tok);
		}
		else if (ids == NULL) {
			count++;
		}
	}
	return count;
}

int ui_frame_get_members(ui_node_t *f, int *ids, int max) {
	return ui_frame_read(f, ids, max, NULL);
}

float ui_frame_get_height(ui_node_t *f) {
	float h = 0.0f;
	ui_frame_read(f, NULL, 0, &h);
	return h;
}

void ui_frame_set_data(ui_node_t *f, const int *ids, int count, float height) {
	ui_node_button_t *b = ui_frame_button(f);
	if (b == NULL) {
		b                = calloc(1, sizeof(ui_node_button_t));
		b->name          = ui_frame_strdup("frame");
		b->type          = ui_frame_strdup(UI_FRAME_BUTTON_TYPE);
		b->output        = -1;
		b->default_value = f32_array_create_x(0.0f);
		if (f->buttons == NULL) {
			f->buttons = (ui_node_button_array_t *)any_array_create_from_raw((void *[]){}, 0);
		}
		any_array_push((any_array_t *)f->buttons, b);
	}
	size_t cap = 32 + (size_t)count * 12;
	char  *buf = malloc(cap);
	int    len = snprintf(buf, cap, "%.9g", height);
	for (int i = 0; i < count; ++i) {
		len += snprintf(buf + len, cap - len, " %d", ids[i]);
	}
	b->data = u8_array_create_from_raw((uint8_t *)buf, (uint32_t)len + 1); // Includes the terminator, the copy encodes as a JSON string
	free(buf);
}

bool ui_frame_has_member(ui_node_t *f, int id) {
	int ids[UI_FRAME_MAX_MEMBERS];
	int n = ui_frame_read(f, ids, UI_FRAME_MAX_MEMBERS, NULL);
	for (int i = 0; i < n; ++i) {
		if (ids[i] == id) {
			return true;
		}
	}
	return false;
}

void ui_frame_add_member(ui_node_t *f, int id) {
	int   ids[UI_FRAME_MAX_MEMBERS];
	float h = 0.0f;
	int   n = ui_frame_read(f, ids, UI_FRAME_MAX_MEMBERS, &h);
	for (int i = 0; i < n; ++i) {
		if (ids[i] == id) {
			return;
		}
	}
	if (n >= UI_FRAME_MAX_MEMBERS) {
		return;
	}
	ids[n++] = id;
	ui_frame_set_data(f, ids, n, h);
}

void ui_frame_remove_member(ui_node_t *f, int id) {
	int   ids[UI_FRAME_MAX_MEMBERS];
	float h = 0.0f;
	int   n = ui_frame_read(f, ids, UI_FRAME_MAX_MEMBERS, &h);
	int   k = 0;
	for (int i = 0; i < n; ++i) {
		if (ids[i] != id) {
			ids[k++] = ids[i];
		}
	}
	if (k != n) {
		ui_frame_set_data(f, ids, k, h);
	}
}

void ui_frame_remap_members(ui_node_t *f, const int *old_ids, const int *new_ids, int count) {
	int   ids[UI_FRAME_MAX_MEMBERS];
	float h = 0.0f;
	int   n = ui_frame_read(f, ids, UI_FRAME_MAX_MEMBERS, &h);
	int   k = 0;
	for (int i = 0; i < n; ++i) {
		for (int j = 0; j < count; ++j) {
			if (old_ids[j] == ids[i]) {
				ids[k++] = new_ids[j]; // Members that were not copied with the frame are dropped
				break;
			}
		}
	}
	ui_frame_set_data(f, ids, k, h);
}

ui_node_t *ui_frame_new(int id, float x, float y) {
	ui_node_t *f = calloc(1, sizeof(ui_node_t));
	f->id        = id;
	f->name      = ui_frame_strdup("Frame");
	f->type      = ui_frame_strdup(UI_FRAME_TYPE);
	f->x         = x;
	f->y         = y;
	f->color     = ui_frame_palette[UI_FRAME_DEFAULT_COLOR];
	f->inputs    = (ui_node_socket_array_t *)any_array_create_from_raw((void *[]){}, 0);
	f->outputs   = (ui_node_socket_array_t *)any_array_create_from_raw((void *[]){}, 0);
	f->buttons   = (ui_node_button_array_t *)any_array_create_from_raw((void *[]){}, 0);
	f->width     = UI_FRAME_DEFAULT_W;
	f->flags     = UI_NODE_FLAG_NONE;
	ui_frame_set_data(f, NULL, 0, UI_FRAME_DEFAULT_H);
	return f;
}

ui_node_t *ui_frame_of(ui_node_canvas_t *c, int id) {
	if (ui_node_is_frame(ui_frame_find(c, id))) {
		return NULL; // A frame is in no frame
	}
	for (int i = 0; i < c->nodes->length; ++i) {
		ui_node_t *f = c->nodes->buffer[i];
		if (ui_node_is_frame(f) && f->id != id && ui_frame_has_member(f, id)) {
			return f;
		}
	}
	return NULL;
}

static bool ui_frame_id_in(const int *ids, int count, int id) {
	for (int i = 0; i < count; ++i) {
		if (ids[i] == id) {
			return true;
		}
	}
	return false;
}

void ui_frame_leave(ui_node_canvas_t *c, int id) {
	for (int i = 0; i < c->nodes->length; ++i) {
		ui_node_t *f = c->nodes->buffer[i];
		if (ui_node_is_frame(f)) {
			ui_frame_remove_member(f, id);
		}
	}
}

int ui_frame_free_nodes(ui_node_canvas_t *c, const int *ids, int count, int *out) {
	int n = 0;
	for (int i = 0; i < count; ++i) {
		if (ui_frame_find(c, ids[i]) != NULL && !ui_node_is_frame(ui_frame_find(c, ids[i])) && ui_frame_of(c, ids[i]) == NULL && !ui_frame_id_in(out, n, ids[i])) {
			out[n++] = ids[i];
		}
	}
	return n;
}

void ui_frame_join(ui_node_canvas_t *c, ui_node_t *frame, const int *ids, int count) {
	for (int i = 0; i < count; ++i) {
		int id = ids[i];
		if (id == frame->id || ui_frame_find(c, id) == NULL || ui_node_is_frame(ui_frame_find(c, id))) {
			continue; // Frames do not nest
		}
		ui_node_t *parent = ui_frame_of(c, id);
		if (parent != NULL && ui_frame_id_in(ids, count, parent->id)) {
			continue; // It travels with its frame, which is part of the same group
		}
		if (!ui_frame_has_member(frame, id) && ui_frame_get_members(frame, NULL, 0) >= UI_FRAME_MAX_MEMBERS) {
			continue; // Full: the node stays where it is
		}
		ui_frame_leave(c, id);
		ui_frame_add_member(frame, id);
	}
}

int ui_frame_drag_set(ui_node_canvas_t *c, const int *selected, int count, int *out, int max) {
	int n = 0;
	for (int i = 0; i < count && n < max; ++i) {
		if (!ui_frame_id_in(out, n, selected[i])) {
			out[n++] = selected[i];
		}
	}
	// The members of the selected frames follow (only the frame's own, a frame has no frame inside)
	int selected_n = n;
	for (int i = 0; i < selected_n; ++i) {
		ui_node_t *f = ui_frame_find(c, out[i]);
		if (!ui_node_is_frame(f)) {
			continue;
		}
		int ids[UI_FRAME_MAX_MEMBERS];
		int m = ui_frame_read(f, ids, UI_FRAME_MAX_MEMBERS, NULL);
		for (int j = 0; j < m && n < max; ++j) {
			if (ui_frame_find(c, ids[j]) != NULL && !ui_node_is_frame(ui_frame_find(c, ids[j])) && !ui_frame_id_in(out, n, ids[j])) {
				out[n++] = ids[j];
			}
		}
	}
	return n;
}

ui_node_t *ui_frame_drop_target(ui_node_canvas_t *c, float x, float y, const int *drag_ids, int count) {
	ui_node_t *best      = NULL;
	float      best_area = 0.0f;
	for (int i = 0; i < c->nodes->length; ++i) {
		ui_node_t *f = c->nodes->buffer[i];
		if (!ui_node_is_frame(f)) {
			continue;
		}
		float w = f->width, h = ui_frame_get_height(f);
		if (x < f->x || x > f->x + w || y < f->y || y > f->y + h) {
			continue;
		}
		if (ui_frame_id_in(drag_ids, count, f->id)) {
			continue; // A dragged frame is never a target (and never taken by one)
		}
		if (best == NULL || w * h < best_area) {
			best      = f;
			best_area = w * h;
		}
	}
	if (best == NULL) {
		return NULL;
	}
	// Only nodes join, and only those that do not travel with a dragged frame: nothing to do when each of them is in the frame already
	for (int j = 0; j < count; ++j) {
		ui_node_t *n = ui_frame_find(c, drag_ids[j]);
		if (n == NULL || ui_node_is_frame(n) || ui_frame_has_member(best, drag_ids[j])) {
			continue;
		}
		ui_node_t *home = ui_frame_of(c, drag_ids[j]);
		if (home != NULL && ui_frame_id_in(drag_ids, count, home->id)) {
			continue;
		}
		return best;
	}
	return NULL;
}

static float ui_frame_node_w(ui_node_t *n) {
	return n->width != 0 ? n->width : 140.0f;
}

static bool ui_frame_differs(float a, float b) {
	return !(fabsf(a - b) <= 0.01f); // nan differs
}

void ui_frame_fit(ui_node_canvas_t *c, ui_frame_node_h_t node_h, ui_frame_node_h_t node_top) {
	{
		for (int i = 0; i < c->nodes->length; ++i) {
			ui_node_t *f = c->nodes->buffer[i];
			if (!ui_node_is_frame(f)) {
				continue;
			}
			int   ids[UI_FRAME_MAX_MEMBERS];
			float stored_h = 0.0f;
			int   count    = ui_frame_read(f, ids, UI_FRAME_MAX_MEMBERS, &stored_h);
			int   valid    = 0;
			float minx = 0, miny = 0, maxx = 0, maxy = 0;
			for (int j = 0; j < count; ++j) {
				ui_node_t *m = ids[j] != f->id ? ui_frame_find(c, ids[j]) : NULL;
				if (m == NULL || ui_node_is_frame(m)) { // Missing nodes are pruned, a frame is never a member (a file can say so: healed here)
					continue;
				}
				ids[valid++] = ids[j];
				float mw     = ui_frame_node_w(m);
				float mh     = node_h(c, m);
				float mtop   = m->y - (node_top == NULL ? 0.0f : node_top(c, m));
				if (valid == 1) {
					minx = m->x;
					miny = mtop;
					maxx = m->x + mw;
					maxy = m->y + mh;
				}
				else {
					minx = fminf(minx, m->x);
					miny = fminf(miny, mtop);
					maxx = fmaxf(maxx, m->x + mw);
					maxy = fmaxf(maxy, m->y + mh);
				}
			}
			float h = stored_h > 0.0f ? stored_h : UI_FRAME_DEFAULT_H;
			if (valid > 0) {
				float nx = minx - UI_FRAME_MARGIN;
				float ny = miny - UI_FRAME_MARGIN - UI_FRAME_HEADER;
				float nw = maxx - minx + UI_FRAME_MARGIN * 2.0f;
				h        = maxy - miny + UI_FRAME_MARGIN * 2.0f + UI_FRAME_HEADER;
				f->x     = nx;
				f->y     = ny;
				f->width = nw;
			}
			else if (!(f->width > 0.0f && f->width < UI_FRAME_MAX_SIZE)) {
				f->width = UI_FRAME_DEFAULT_W;
			}
			if (valid != count || ui_frame_differs(h, stored_h)) {
				ui_frame_set_data(f, ids, valid, h); // Also drops members that are gone
			}
		}
	}
}

ui_frame_hit_t ui_frame_hit(float x, float y, float w, float h, float px, float py, float header_h, float edge) {
	float half = edge * 0.5f;
	if (px < x - half || px > x + w + half || py < y - half || py > y + h + half) {
		return UI_FRAME_HIT_NONE;
	}
	if (px >= x && px <= x + w && py >= y && py <= y + header_h) {
		return UI_FRAME_HIT_HEADER;
	}
	if (px < x + half || px > x + w - half || py < y + half || py > y + h - half) {
		return UI_FRAME_HIT_EDGE;
	}
	return UI_FRAME_HIT_NONE; // The body is click through
}

int ui_frame_draw_order(ui_node_canvas_t *c, int *order, int max) {
	int n = 0;
	for (int i = 0; i < c->nodes->length && n < max && n < UI_FRAME_MAX_MEMBERS; ++i) {
		if (ui_node_is_frame(c->nodes->buffer[i])) {
			order[n++] = i; // The array order: a later frame is drawn over an earlier one
		}
	}
	return n;
}

float ui_frame_label_size(float base, float zoom, float frame_w, float frame_h, float text_w) {
	if (zoom <= 0.0f) {
		zoom = 1.0f;
	}
	// On screen the label is max(base * zoom, min_screen): in canvas units max(base, min_screen / zoom), in stops of a ratio
	float want = fmaxf(base, base * UI_FRAME_LABEL_MIN_SCREEN / zoom);
	float step = logf(UI_FRAME_LABEL_STOP);
	int   k    = (int)ceilf(logf(want / base) / step - 0.0001f);
	// Cap: the width within the frame and a share of the frame height, snapped down to a stop
	float cap = 1.0e30f;
	if (text_w > 0.0f) {
		cap = fminf(cap, base * (frame_w - UI_FRAME_LABEL_PAD * 2.0f) / text_w);
	}
	cap = fminf(cap, frame_h * UI_FRAME_LABEL_MAX_H);
	if (cap < base * powf(UI_FRAME_LABEL_STOP, (float)k)) {
		int kc = (int)floorf(logf(fmaxf(cap, 0.0001f) / base) / step + 0.0001f);
		k      = kc < UI_FRAME_LABEL_MIN_STOP ? UI_FRAME_LABEL_MIN_STOP : kc;
	}
	return base * powf(UI_FRAME_LABEL_STOP, (float)k);
}
