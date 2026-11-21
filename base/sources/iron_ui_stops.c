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
// Marker geometry. Every size derives from marker_h (the drawn handle row, 0.5 line, it already includes UI scale and node
// zoom) and snaps to whole pixels with a 1 px floor, so a small scale keeps crisp rings and layout, drawing and hit test
// (ui_stops_pin_extent) cannot disagree.
static int ui_stops_round(float v) {
	return (int)floorf(v + 0.5f);
}

static int ui_stops_clamp_int(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

// Width of the marker box, odd so it is centred on one pixel column
int ui_stops_pin_w(float marker_h) {
	int w = ui_stops_round(0.8f * marker_h);
	return w % 2 == 0 ? w + 1 : w;
}

// Black outline (outermost) and neutral grey frame (inside it) of the marker box, in pixels, both lie INSIDE the silhouette,
// which is ui_stops_pin_w wide. Like Blender's handle they are equally thick (1 logical px on its 18 px bar: 0.075 of
// marker_h here, so 2 px at UI scale 2 and 1 px at UI scale 1). The box is outline, then the frame on all four sides (the
// top too), then the stop colour fill: the fill gets pin_w - 2 * (outline + frame) across. Where that would leave less than
// 3 px (the width is odd) the frame shrinks first, down to 0, the outline stays; at pin_w 3 the fill is 1 px wide, at pin_w 1
// (marker_h below 2.5) there is no outline.
void ui_stops_pin_rings(float marker_h, int *outline, int *frame) {
	*outline = ui_stops_clamp_int(ui_stops_round(0.075f * marker_h), 1, 8);
	*frame   = *outline;
	int w    = ui_stops_pin_w(marker_h);
	if (*outline > (w - 1) / 2) { // Only at pin_w 1 (marker_h below 2.5): no room for any outline
		*outline = (w - 1) / 2;
	}
	int room = (w - 2 * *outline - 3) / 2; // Frame that still leaves a 3 px fill
	if (*frame > room) {
		*frame = room < 0 ? 0 : room;
	}
}

// How far a marker reaches sideways past its position: half of the silhouette (the roof base and the box outer edge are
// pin_w wide). The consumer insets bar and track by it on both sides, so the markers at 0 and 1 stay inside the control.
// The same for active and inactive markers (the silhouette does not grow).
int ui_stops_pin_extent(float marker_h) {
	return (int)ceilf((float)ui_stops_pin_w(marker_h) * 0.5f);
}

// Stop at the end of the bar (the lowest position for the left end, the highest for the right end, on a tie the higher
// index, the one drawn on top), -1 without stops. A press on the margin next to an end marker selects it.
int ui_stops_end(ui_stops_get_pos_t get_pos, void *data, int count, bool right) {
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

// Marker colours from the theme, so they keep their contrast against the node background in every theme. bg_col is the
// node background (WINDOW_BG_COL), text_col the text colour (TEXT_COL). The outline is black (Blender's too, on dark and
// light themes), the frame and the roofs are text pulled towards the background: the frame about mid grey (128 on the
// default and the light theme), the inactive roof darker than it and the active roof brighter, in a dark theme (a light
// theme inverts the roofs with the text colour). The frame colour never changes with the selection.
void ui_stops_pin_colors(uint32_t bg_col, uint32_t text_col, ui_stops_colors_t *colors) {
	colors->outline     = 0xff000000;
	colors->frame       = ui_stops_mix(text_col, bg_col, 0.55f);
	colors->roof        = ui_stops_mix(text_col, bg_col, 0.72f);
	colors->roof_active = ui_stops_mix(text_col, bg_col, 0.10f);
}
// ui_stops logic end

// The house, built like Blender's color ramp handle: a 45 degree roof in neutral grey (darker when inactive, brighter when
// active, the select indicator) over a box. Both have a black outline. The box is the black outline, the neutral grey frame
// on all four sides (its top band lies directly under the roof, the frame colour does not change with the selection) and
// the stop colour fill innermost. The silhouette is w wide (the roof base and the outer edge of the box are the same width)
// and roof + box_h tall. The roof outline runs along the two slopes and, like the box outline, ends on the roof base: the
// grey roof sits directly on the frame.
// cx: pixel centre of the position, y: the bar bottom, w: ui_stops_pin_w(h). locked: the dimmed variant of a held stop.
static void ui_stops_draw_house(float cx, float y, float w, float h, uint32_t fill, bool active, bool locked) {
	ui_theme_t       *theme = ui_get_current()->ops->theme;
	ui_stops_colors_t colors;
	ui_stops_pin_colors((uint32_t)theme->WINDOW_BG_COL, (uint32_t)theme->TEXT_COL, &colors);
	if (locked) { // Dimmed towards the background: no active highlight, the box colour kept faintly
		uint32_t bg    = (uint32_t)theme->WINDOW_BG_COL;
		colors.outline = ui_stops_mix(colors.outline, bg, 0.5f);
		colors.frame   = ui_stops_mix(colors.frame, bg, 0.6f);
		colors.roof    = ui_stops_mix(colors.roof, bg, 0.6f);
		fill           = ui_stops_mix(fill | 0xff000000, bg, 0.65f);
		active         = false;
	}
	int outline, frame;
	ui_stops_pin_rings(h, &outline, &frame);
	float half  = w * 0.5f;
	float roof  = floorf(half + 0.5f); // Height of the 45 degree roof
	float top   = y + roof;            // Top of the box
	float box_h = floorf(roof * 1.4f + 0.5f);
	float d     = (float)outline;
	float g     = (float)frame;
	float left  = cx - half;
	float inset = 1.4142f * d; // Vertical room of the roof outline at the apex (it is d wide at the base)
	if (inset > roof - 1.0f) { // Keep a roof of at least 1 px at the smallest sizes
		inset = roof > 1.0f ? roof - 1.0f : 0.0f;
	}

	draw_set_color(colors.outline);
	draw_filled_rect(left, top, w, box_h);
	draw_set_color(colors.frame);
	draw_filled_rect(left + d, top, w - d * 2.0f, box_h - d);
	draw_set_color(fill | 0xff000000);
	draw_filled_rect(left + d + g, top + g, w - (d + g) * 2.0f, box_h - d - g * 2.0f);
	draw_set_color(colors.outline);
	draw_filled_triangle(cx, y, left, top, cx + half, top);
	draw_set_color(active ? colors.roof_active : colors.roof);
	draw_filled_triangle(cx, y + inset, left + d, top, cx + half - d, top);
	draw_set_color(0xffffffff);
}

void ui_stops_draw_pin(float cx, float y, float w, float h, uint32_t fill, bool active) {
	ui_stops_draw_house(cx, y, w, h, fill, active, false);
}

// The house of a stop the consumer holds fixed (a linked value): dimmed, never active
void ui_stops_draw_pin_locked(float cx, float y, float w, float h, uint32_t fill) {
	ui_stops_draw_house(cx, y, w, h, fill, false, true);
}

// Position line across the bar at a stop: 1 px wide at ordinary sizes, dashes alternating light and dark (readable on any
// bar colour), dimmer when inactive. These colours sit on the bar content, not on the node background: fixed, not themed.
static void ui_stops_draw_line(float cx, float y, float bar_h, float marker_h, bool active) {
	int      line_w = ui_stops_clamp_int(ui_stops_round(0.035f * marker_h), 1, 1 << 20);
	int      dash   = ui_stops_clamp_int(ui_stops_round(0.14f * marker_h), 2, 4);
	float    x0     = floorf(cx) - (float)((line_w - 1) / 2);
	uint32_t light  = active ? 0xffffffff : 0xffb2b2b2;
	uint32_t dark   = active ? 0xff000000 : 0xff666666;
	int      n      = 0;
	for (float off = 0.0f; off < bar_h; off += (float)dash, ++n) {
		float len = bar_h - off < (float)dash ? bar_h - off : (float)dash;
		draw_set_color(n % 2 == 0 ? light : dark);
		draw_filled_rect(x0, y + off, (float)line_w, len);
	}
	draw_set_color(0xffffffff);
}

// Pixel column of a position, snapped to the pixel grid and kept inside the track
static float ui_stops_snap_x(float x, float w, float pos) {
	float col  = floorf(x + ui_stops_clamp(pos) * w);
	float last = ceilf(x + w) - 1.0f;
	return (col > last ? last : col) + 0.5f;
}

// One frame of the control. x, y, w: the track in window coordinates (the consumer insets its bar by
// ui_stops_pin_extent(marker_h) on both sides, so the markers at 0 and 1 stay inside), bar_h the bar height, the markers
// are drawn in the marker_h tall row below it; a press counts within the hit_h tall row (at least marker_h, a small drawn
// handle keeps a comfortable click zone) and horizontally within the track plus the pin extent. A press on a handle
// selects it and starts a drag (a double click deletes it), a press on the bar adds a stop there and drags it, a press on
// the margin beside the track selects the end stop (it never adds one).
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
	float pin_w  = (float)ui_stops_pin_w(marker_h);
	float extent = (float)ui_stops_pin_extent(marker_h);
	float hit_w  = hit_h * 0.7f;
	float mx    = ui->input_x - ui->_window_x;
	float my    = ui->input_y - ui->_window_y;

	bool can_start = ui->enabled && ui->input_enabled && ui->input_started && s->drag == 0 &&
	                 (ui->current_window == NULL || ui_input_in_rect(ui->_window_x, ui->_window_y, ui->_window_w, ui->_window_h));
	if (can_start && mx >= x - extent && mx <= x + w + extent && my >= y && my < y + bar_h + hit_h) {
		flags |= UI_STOPS_CONSUMED;
		bool in_bar = my < y + bar_h;
		bool margin = mx < x || mx > x + w;
		int  hit    = in_bar ? -1 : ui_stops_hit(ops->get_pos, data, n, mx, x, w, hit_w * 0.5f + 1.0f, s->active);
		if (margin && hit < 0) {
			hit = ui_stops_end(ops->get_pos, data, n, mx > x + w);
		}
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
		else if (in_bar && !margin && ui_stops_can_add(n, ops->max_count)) {
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
	for (int i = 0; i < n; ++i) { // The active stop last, on top
		if (i != s->active) {
			float cx = ui_stops_snap_x(x, w, ops->get_pos(data, i));
			ui_stops_draw_line(cx, y, bar_h, marker_h, false);
			ops->draw(data, i, cx, y + bar_h, pin_w, marker_h, false);
		}
	}
	if (n > 0) {
		float cx = ui_stops_snap_x(x, w, ops->get_pos(data, s->active));
		ui_stops_draw_line(cx, y, bar_h, marker_h, true);
		ops->draw(data, s->active, cx, y + bar_h, pin_w, marker_h, true);
	}
	return flags;
}
