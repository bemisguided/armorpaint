#include "iron_draw.h"
#include "iron_system.h"
#include "iron_ui.h"
#include <math.h>
#include <stdbool.h>

#define UI_STOPS_DOUBLE_CLICK 0.2 // Seconds, as the double clicks of the node editor and the mesh tab

static float ui_stops_clamp(float pos) {
	if (!(pos > 0.0f)) {
		return 0.0f; // Also NaN
	}
	return pos > 1.0f ? 1.0f : pos;
}

// Stop under the mouse, -1 for none. Stops within radius (pixels along the bar) are candidates, the nearest ones (within
// 1 px of the nearest) are tied. If the active stop is one of several tied stops the next tied stop is returned, so a
// click on the same spot cycles through overlapping handles, else the tied stop with the highest index (drawn on top).
static int ui_stops_hit(ui_stops_get_pos_t get_pos, void *data, int count, float mouse_x, float bar_x, float bar_w, float radius, int active) {
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
	int  tied_count  = 0;
	int  last_tied   = -1;
	int  next_tied   = -1; // First tied stop after active
	int  first_tied  = -1;
	bool active_tied = false;
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

static bool ui_stops_is_double_click(double now, double last_time, int last_hit, int hit) {
	return hit >= 0 && hit == last_hit && now - last_time < UI_STOPS_DOUBLE_CLICK;
}

// Marker geometry: every size derives from marker_h (the handle row, it includes UI scale and node zoom) and snaps to
// whole pixels, so the drawing and the hit test (ui_stops_pin_extent) agree.
static int ui_stops_round(float v) {
	return (int)floorf(v + 0.5f);
}

static int ui_stops_clamp_int(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

// Width of the marker box, odd so it is centred on one pixel column
static int ui_stops_pin_w(float marker_h) {
	int w = ui_stops_round(0.8f * marker_h);
	return w % 2 == 0 ? w + 1 : w;
}

// Black outline and grey frame of the marker box in pixels, both inside the silhouette (ui_stops_pin_w wide). The frame
// shrinks first when the fill would get less than 3 px (1 px at pin_w 3, no outline at pin_w 1).
static void ui_stops_pin_rings(float marker_h, int *outline, int *frame) {
	*outline = ui_stops_clamp_int(ui_stops_round(0.075f * marker_h), 1, 8);
	*frame   = *outline;
	int w    = ui_stops_pin_w(marker_h);
	if (*outline > (w - 1) / 2) {
		*outline = (w - 1) / 2;
	}
	int room = (w - 2 * *outline - 3) / 2;
	if (*frame > room) {
		*frame = room < 0 ? 0 : room;
	}
}

// How far a marker reaches sideways past its position (half the silhouette). The consumer insets bar and track by it on
// both sides, so the markers at 0 and 1 stay inside the control.
int ui_stops_pin_extent(float marker_h) {
	return (int)ceilf((float)ui_stops_pin_w(marker_h) * 0.5f);
}

// Stop at the end of the bar (the lowest position for the left end, the highest for the right end, on a tie the higher
// index, the one drawn on top), -1 without stops. A press on the margin next to an end marker selects it.
static int ui_stops_end(ui_stops_get_pos_t get_pos, void *data, int count, bool right) {
	int   best     = -1;
	float best_pos = 0.0f;
	for (int i = 0; i < count; ++i) {
		float p = ui_stops_clamp(get_pos(data, i));
		if (best < 0 || (right ? p >= best_pos : p <= best_pos)) {
			best     = i;
			best_pos = p;
		}
	}
	return best;
}

static uint32_t ui_stops_mix(uint32_t a, uint32_t b, float t) {
	uint32_t out = 0xff000000;
	for (int shift = 0; shift < 24; shift += 8) {
		float ca = (float)((a >> shift) & 0xff);
		float cb = (float)((b >> shift) & 0xff);
		out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << shift;
	}
	return out;
}

typedef struct ui_stops_colors {
	uint32_t outline;
	uint32_t frame;
	uint32_t roof;
	uint32_t roof_active;
} ui_stops_colors_t;

// Marker colours from the theme (bg_col: node background, text_col: text colour), so they keep their contrast in every
// theme: black outline, a frame between text and background colour, the inactive roof as the frame, the active roof bright.
static void ui_stops_pin_colors(uint32_t bg_col, uint32_t text_col, ui_stops_colors_t *colors) {
	colors->outline     = 0xff000000;
	colors->frame       = ui_stops_mix(text_col, bg_col, 0.55f);
	colors->roof        = colors->frame;
	colors->roof_active = ui_stops_mix(text_col, bg_col, 0.10f);
}

// A marker is a house like Blender's color ramp handle: a roof over a box with the stop colour. Geometry and colours are
// the same for all markers of a control.
typedef struct ui_stops_house {
	float             w; // Silhouette width, odd
	float             half;
	float             roof; // Height of the 45 degree roof
	float             box_h;
	float             d; // Outline
	float             g; // Frame
	float             inset; // Vertical room of the roof outline at the apex
	ui_stops_colors_t colors;
} ui_stops_house_t;

static void ui_stops_house_init(ui_stops_house_t *h, float marker_h) {
	ui_theme_t *theme = ui_get_current()->ops->theme;
	ui_stops_pin_colors((uint32_t)theme->WINDOW_BG_COL, (uint32_t)theme->TEXT_COL, &h->colors);
	int outline, frame;
	ui_stops_pin_rings(marker_h, &outline, &frame);
	h->w     = (float)ui_stops_pin_w(marker_h);
	h->half  = h->w * 0.5f;
	h->roof  = floorf(h->half + 0.5f);
	h->box_h = floorf(h->roof * 1.4f + 0.5f);
	h->d     = (float)outline;
	h->g     = (float)frame;
	h->inset = 1.4142f * h->d;
	if (h->inset > h->roof - 1.0f) { // Keep a roof of at least 1 px at the smallest sizes
		h->inset = h->roof > 1.0f ? h->roof - 1.0f : 0.0f;
	}
}

// The box: black outline, grey frame on all four sides, then the stop colour. cx: pixel centre of the position, y: bar bottom.
static void ui_stops_draw_box(const ui_stops_house_t *h, float cx, float y, uint32_t fill) {
	float left = cx - h->half;
	float top  = y + h->roof;
	draw_set_color(h->colors.outline);
	draw_filled_rect(left, top, h->w, h->box_h);
	draw_set_color(h->colors.frame);
	draw_filled_rect(left + h->d, top, h->w - h->d * 2.0f, h->box_h - h->d);
	draw_set_color(fill | 0xff000000);
	draw_filled_rect(left + h->d + h->g, top + h->g, h->w - (h->d + h->g) * 2.0f, h->box_h - h->d - h->g * 2.0f);
}

// The roof: black outline over a grey roof, the select grey when active. It lies in the rows above the box.
static void ui_stops_draw_roof(const ui_stops_house_t *h, float cx, float y, bool active) {
	float left = cx - h->half;
	float top  = y + h->roof;
	draw_set_color(h->colors.outline);
	draw_filled_triangle(cx, y, left, top, cx + h->half, top);
	draw_set_color(active ? h->colors.roof_active : h->colors.roof);
	draw_filled_triangle(cx, y + h->inset, left + h->d, top, cx + h->half - h->d, top);
}

// Position line across the bar at a stop: 1 px wide at ordinary sizes, dashes alternating light and dark (readable on any
// bar colour), dimmer when inactive. These colours sit on the bar content, not on the node background: fixed, not themed.
static void ui_stops_draw_line(float cx, float y, float bar_h, float marker_h, bool active) {
	int      line_w = ui_stops_clamp_int(ui_stops_round(0.035f * marker_h), 1, 8);
	int      dash   = ui_stops_clamp_int(ui_stops_round(0.14f * marker_h), 2, 4);
	float    x0     = floorf(cx) - (float)((line_w - 1) / 2);
	uint32_t light  = active ? 0xffffffff : 0xffb2b2b2;
	uint32_t dark   = active ? 0xff000000 : 0xff666666;
	draw_set_color(dark);
	draw_filled_rect(x0, y, (float)line_w, bar_h);
	draw_set_color(light);
	for (float off = 0.0f; off < bar_h; off += 2.0f * (float)dash) {
		float len = bar_h - off < (float)dash ? bar_h - off : (float)dash;
		draw_filled_rect(x0, y + off, (float)line_w, len);
	}
}

// Pixel column of a position, snapped to the pixel grid and kept inside the track
static float ui_stops_snap_x(float x, float w, float pos) {
	float col  = floorf(x + ui_stops_clamp(pos) * w);
	float last = ceilf(x + w) - 1.0f;
	return (col > last ? last : col) + 0.5f;
}

// Drawing order: every stop except the active one, then the active one (on top)
static int ui_stops_draw_order(int k, int n, int active) {
	if (k == n - 1) {
		return active;
	}
	return k < active ? k : k + 1;
}

// Position lines, then all boxes, then all roofs: the roofs are the only triangles, so the draw batch switches the pipeline
// once for the whole control. The roof rows lie above the box rows and the lines above both, so nothing overlaps between
// the passes and the picture is that of drawing one marker after the other.
static void ui_stops_draw(const ui_stops_t *s, const ui_stops_ops_t *ops, void *data, const ui_stops_layout_t *l) {
	int n = ops->count(data);
	if (n <= 0) {
		return;
	}
	ui_stops_house_t h;
	ui_stops_house_init(&h, l->marker_h);
	float line_y = floorf(l->y); // Whole pixels: the triangles are not truncated like the rects
	float pin_y  = floorf(l->y + l->bar_h);
	for (int k = 0; k < n; ++k) {
		int i = ui_stops_draw_order(k, n, s->active);
		ui_stops_draw_line(ui_stops_snap_x(l->x, l->w, ops->get_pos(data, i)), line_y, l->bar_h, l->marker_h, i == s->active);
	}
	ui_stops_house_t dim = h; // Fork only: the house of a stop the consumer holds fixed (a linked value) is dimmed towards the background
	uint32_t         bg  = (uint32_t)ui_get_current()->ops->theme->WINDOW_BG_COL;
	dim.colors.outline   = ui_stops_mix(h.colors.outline, bg, 0.5f);
	dim.colors.frame     = ui_stops_mix(h.colors.frame, bg, 0.6f);
	dim.colors.roof      = ui_stops_mix(h.colors.roof, bg, 0.6f);
	for (int k = 0; k < n; ++k) {
		int      i      = ui_stops_draw_order(k, n, s->active);
		bool     locked = ops->is_locked != NULL && ops->is_locked(data, i);
		uint32_t fill   = ops->fill(data, i);
		ui_stops_draw_box(locked ? &dim : &h, ui_stops_snap_x(l->x, l->w, ops->get_pos(data, i)), pin_y, locked ? ui_stops_mix(fill | 0xff000000, bg, 0.65f) : fill);
	}
	for (int k = 0; k < n; ++k) {
		int  i      = ui_stops_draw_order(k, n, s->active);
		bool locked = ops->is_locked != NULL && ops->is_locked(data, i);
		ui_stops_draw_roof(locked ? &dim : &h, ui_stops_snap_x(l->x, l->w, ops->get_pos(data, i)), pin_y, i == s->active && !locked);
	}
	draw_set_color(0xffffffff);
}

// One frame of the control. The layout is the track (the consumer insets its bar by ui_stops_pin_extent(marker_h) on both
// sides, so the markers at 0 and 1 stay inside), the bar_h tall bar, the marker_h tall row of markers below it and the hit_h
// tall click zone (at least marker_h); a press counts within the track plus the pin extent. A press on a handle selects it
// and starts a drag (a double click deletes it), a press on the bar adds a stop there and drags it, a press on the margin
// beside the track selects the end stop (it never adds one).
int ui_stops_update(ui_stops_t *s, const ui_stops_ops_t *ops, void *data, const ui_stops_layout_t *layout) {
	ui_t *ui    = ui_get_current();
	int   flags = 0;
	int   n     = ops->count(data);
	if (n <= 0) {
		s->drag = false;
		return 0;
	}
	if (s->active >= n) {
		s->active = n - 1;
	}
	if (s->active < 0) {
		s->active = 0;
	}
	float x      = layout->x;
	float y      = layout->y;
	float w      = layout->w;
	float bar_h  = layout->bar_h;
	float hit_h  = layout->hit_h < layout->marker_h ? layout->marker_h : layout->hit_h;
	float extent = (float)ui_stops_pin_extent(layout->marker_h);
	float hit_w  = hit_h * 0.7f;
	float mx     = ui->input_x - ui->_window_x;
	float my     = ui->input_y - ui->_window_y;

	bool can_start = ui->enabled && ui->input_enabled && ui->input_started && !s->drag &&
	                 (ui->current_window == NULL || ui_input_in_rect(ui->_window_x, ui->_window_y, ui->_window_w, ui->_window_h));
	if (can_start && mx >= x - extent && mx <= x + w + extent && my >= y && my < y + bar_h + hit_h) {
		flags |= UI_STOPS_CONSUMED;
		bool in_bar = my < y + bar_h;
		bool margin = mx < x || mx > x + w;
		int  hit    = in_bar ? -1 : ui_stops_hit(ops->get_pos, data, n, mx, x, w, hit_w * 0.5f + 1.0f, s->active);
		if (margin && hit < 0) {
			hit = ui_stops_end(ops->get_pos, data, n, mx > x + w);
		}
		bool locked = hit >= 0 && ops->is_locked != NULL && ops->is_locked(data, hit); // Fork only
		if (locked) {
			if (s->active != hit) {
				flags |= UI_STOPS_SELECTED;
			}
			s->active   = hit;
			s->last_hit = -1;
		}
		else if (hit >= 0) {
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
				s->drag      = true;
				s->moved     = false;
				s->grab      = ui_stops_clamp(ops->get_pos(data, hit)) - (mx - x) / w;
				s->last_hit  = hit;
				s->last_time = iron_time();
			}
		}
		else if (in_bar && !margin && ui_stops_can_add(n, ops->max_count)) {
			int added = ops->add(data, ui_stops_clamp((mx - x) / w));
			if (added >= 0) {
				s->active   = added;
				s->drag     = true;
				s->moved    = true;
				s->grab     = 0.0f;
				s->last_hit = -1;
				flags      |= UI_STOPS_CHANGED | UI_STOPS_SELECTED;
				ui->changed = true;
			}
		}
		ui->input_started = false;
	}

	if (s->drag) {
		if (ui->input_down) {
			float pos = ui_stops_clamp((mx - x) / w + s->grab);
			if (fabsf(pos - ui_stops_clamp(ops->get_pos(data, s->active))) > 1e-6f) { // A press without movement changes nothing
				ops->set_pos(data, s->active, pos);
				s->moved = true;
			}
			if (s->moved) {
				ui->changed = true; // Keeps the drag one undo step
				flags      |= UI_STOPS_CHANGED;
			}
		}
		else {
			s->drag = false;
			if (s->moved) {
				s->last_hit = -1; // A drag is not the first click of a double click
			}
		}
	}

	ui_stops_draw(s, ops, data, layout);
	return flags;
}
