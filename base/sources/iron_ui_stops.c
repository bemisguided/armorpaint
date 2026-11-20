#include "iron_draw.h"
#include "iron_system.h"
#include "iron_ui.h"
#include <math.h>
#include <stdbool.h>

// ui_stops logic begin: pure functions, no drawing and no input state
float ui_stops_clamp(float pos) {
	if (!(pos > 0.0f)) {
		return 0.0f; // Also NaN
	}
	return pos > 1.0f ? 1.0f : pos;
}

// Stop under the mouse, -1 for none. Stops within radius (pixels along the bar) are candidates, the nearest ones (within
// 1 px of the nearest) are tied. If the active stop is one of several tied stops the next tied stop is returned, so a
// click on the same spot cycles through overlapping handles, else the tied stop with the highest index (drawn on top).
int ui_stops_hit(ui_stops_get_pos_t get_pos, void *data, int count, float mouse_x, float bar_x, float bar_w, float radius, int active) {
	float nearest = radius + 1.0f;
	for (int i = 0; i < count; ++i) {
		float d = fabsf(bar_x + ui_stops_clamp(get_pos(data, i)) * bar_w - mouse_x);
		if (d <= radius && d < nearest) {
			nearest = d;
		}
	}
	if (nearest > radius) {
		return -1;
	}
	int  tied_count   = 0;
	int  last_tied    = -1;
	int  next_tied    = -1; // First tied stop after active
	int  first_tied   = -1;
	bool active_tied  = false;
	for (int i = 0; i < count; ++i) {
		float d = fabsf(bar_x + ui_stops_clamp(get_pos(data, i)) * bar_w - mouse_x);
		if (d > radius || d > nearest + 1.0f) {
			continue;
		}
		tied_count++;
		last_tied = i;
		if (first_tied < 0) {
			first_tied = i;
		}
		if (i == active) {
			active_tied = true;
		}
		else if (i > active && next_tied < 0) {
			next_tied = i;
		}
	}
	if (active_tied) {
		if (tied_count == 1) {
			return active;
		}
		return next_tied >= 0 ? next_tied : first_tied;
	}
	return last_tied;
}

// Middle of the largest gap between neighbouring stops (in position order), 0.5 for fewer than two stops
float ui_stops_gap_pos(ui_stops_get_pos_t get_pos, void *data, int count) {
	if (count < 2) {
		return 0.5f;
	}
	if (count > UI_STOPS_MAX) {
		count = UI_STOPS_MAX;
	}
	float p[UI_STOPS_MAX];
	for (int i = 0; i < count; ++i) {
		float v = ui_stops_clamp(get_pos(data, i));
		int   j = i - 1;
		while (j >= 0 && p[j] > v) {
			p[j + 1] = p[j];
			j--;
		}
		p[j + 1] = v;
	}
	float best = -1.0f;
	float mid  = 0.5f;
	for (int i = 0; i + 1 < count; ++i) {
		float gap = p[i + 1] - p[i];
		if (gap > best) {
			best = gap;
			mid  = p[i] + gap * 0.5f;
		}
	}
	return mid;
}

// Active index after stop removed was deleted, count_after stops are left
int ui_stops_after_remove(int active, int removed, int count_after) {
	if (active > removed) {
		active--;
	}
	if (active >= count_after) {
		active = count_after - 1;
	}
	return active < 0 ? 0 : active;
}

bool ui_stops_can_add(int count, int max_count) {
	return count < max_count;
}

bool ui_stops_can_remove(int count, int min_count) {
	return count > min_count;
}

bool ui_stops_is_double_click(double now, double last_time, int last_hit, int hit) {
	return hit >= 0 && hit == last_hit && now - last_time < 0.3;
}
// ui_stops logic end

void ui_stops_draw_pin(float cx, float y, float w, float h, uint32_t fill, bool active) {
	float o    = (active ? 2.0f : 1.0f) * UI_SCALE();
	float half = w * 0.5f;
	float neck = y + h * 0.35f;
	draw_set_color(active ? 0xffffffff : 0xff000000);
	draw_filled_triangle(cx, y - o * 1.5f, cx - half - o, neck, cx + half + o, neck);
	draw_filled_rect(cx - half - o, neck, w + o * 2.0f, y + h - neck + o);
	draw_set_color(fill | 0xff000000);
	draw_filled_triangle(cx, y, cx - half, neck, cx + half, neck);
	draw_filled_rect(cx - half, neck, w, y + h - neck);
	draw_set_color(0xffffffff);
}

// One frame of the control. x, y, w: the bar in window coordinates, bar_h its height, the handles are drawn in the
// marker_h tall row below it; a press counts within the hit_h tall row (at least marker_h, a small drawn handle keeps
// a comfortable click zone). A press on a handle selects it and starts a drag (a double click deletes it), a press
// on the bar adds a stop there and drags it.
int ui_stops_update(ui_stops_t *s, const ui_stops_ops_t *ops, void *data, float x, float y, float w, float bar_h, float marker_h, float hit_h) {
	ui_t *ui    = ui_get_current();
	int   flags = 0;
	int   n     = ops->count(data);
	if (n <= 0) {
		s->drag = 0;
		return 0;
	}
	if (s->active >= n) {
		s->active = n - 1;
	}
	if (s->active < 0) {
		s->active = 0;
	}
	if (hit_h < marker_h) {
		hit_h = marker_h;
	}
	float pin_w = marker_h * 0.7f;
	float hit_w = hit_h * 0.7f;
	float mx    = ui->input_x - ui->_window_x;
	float my    = ui->input_y - ui->_window_y;

	bool can_start = ui->enabled && ui->input_enabled && ui->input_started && s->drag == 0 &&
	                 (ui->current_window == NULL || ui_input_in_rect(ui->_window_x, ui->_window_y, ui->_window_w, ui->_window_h));
	if (can_start && mx >= x - hit_w && mx <= x + w + hit_w && my >= y && my < y + bar_h + hit_h) {
		flags |= UI_STOPS_CONSUMED;
		bool in_bar = my < y + bar_h;
		int  hit    = in_bar ? -1 : ui_stops_hit(ops->get_pos, data, n, mx, x, w, hit_w * 0.5f + 1.0f, s->active);
		if (hit >= 0) {
			if (ui_stops_is_double_click(iron_time(), s->last_time, s->last_hit, hit) && ui_stops_can_remove(n, ops->min_count)) {
				ops->remove(data, hit);
				s->active    = ui_stops_after_remove(s->active, hit, ops->count(data));
				s->last_hit  = -1;
				flags       |= UI_STOPS_CHANGED | UI_STOPS_SELECTED;
				ui->changed  = true;
			}
			else {
				if (s->active != hit) {
					flags |= UI_STOPS_SELECTED;
				}
				s->active    = hit;
				s->drag      = 1;
				s->moved     = false;
				s->grab      = ops->get_pos(data, hit) - (mx - x) / w;
				s->last_hit  = hit;
				s->last_time = iron_time();
			}
		}
		else if (in_bar && ui_stops_can_add(n, ops->max_count)) {
			int added = ops->add(data, ui_stops_clamp((mx - x) / w));
			if (added >= 0) {
				s->active   = added;
				s->drag     = 1;
				s->moved    = true;
				s->grab     = 0.0f;
				s->last_hit = -1;
				flags      |= UI_STOPS_CHANGED | UI_STOPS_SELECTED;
				ui->changed = true;
			}
		}
		ui->input_started = false;
	}

	if (s->drag != 0) {
		if (ui->input_down) {
			float pos = ui_stops_clamp((mx - x) / w + s->grab);
			if (pos != ops->get_pos(data, s->active)) {
				ops->set_pos(data, s->active, pos);
				s->moved = true;
			}
			if (s->moved) {
				ui->changed = true; // Keeps the drag one undo step
				flags      |= UI_STOPS_CHANGED;
			}
		}
		else {
			s->drag = 0;
		}
	}

	n = ops->count(data);
	for (int i = 0; i < n; ++i) {
		if (i != s->active) {
			ops->draw(data, i, x + ui_stops_clamp(ops->get_pos(data, i)) * w, y + bar_h, pin_w, marker_h, false);
		}
	}
	if (n > 0) {
		ops->draw(data, s->active, x + ui_stops_clamp(ops->get_pos(data, s->active)) * w, y + bar_h, pin_w, marker_h, true);
	}
	return flags;
}
