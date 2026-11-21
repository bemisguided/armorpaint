#include "../global.h"

// Levels (Filter menu): one remap of the values of the Image input, the same function on R, G and B. A pass-through shader node like Map Range
// and Gamma: the remap is computed per pixel in the material shader (str_levels below), no baked result, no Size. The Image input and every
// value input (Black, White, Gamma, Out Black, Out White, Position, Width, Contrast) can be linked and are evaluated per pixel. The alpha of a
// colour is not carried by shader colour sockets (as for every colour node), so it passes through as it is.
// Two modes of the same core (levels_core), spec specs/image-nodes/IMAGE_NODES.md (Levels):
//   Levels: out = out_low + (out_high - out_low) * pow(clamp((x - black) / (white - black), 0, 1), 1 / gamma)
//           Black and White are the input points, Gamma the midtone (gamma 2 brightens: the input at black + (white - black) * 0.5^gamma gives 0.5),
//           Out Black and Out White the output range (out black above out white reverses the image).
//   Range:  a window of the values around Position (the Histogram Select of Substance Designer): d = |x - position|, half = Width / 2,
//           ramp = (1 - Contrast) * half, out = 1 - clamp((d - (half - ramp)) / ramp, 0, 1): 1 inside the window, falling linearly to 0 over the
//           ramp that ends at the window edge (position +- half). Contrast 1 gives a hard edge, 0 a triangle. Invert gives 1 - out.
//           It is the Levels core with black = half - ramp, white = half, gamma 1, out low 1, out high 0 (swapped by Invert), applied to d.
// A range of zero width (white - black below 1e-6: White not above Black, Width 0, Contrast 1) is a hard step at black (x above black
// gives 1). All values are clamped to their slider range in the shader (Black, White, Position, Out Black, Out White, Contrast 0..1, Width 0..1,
// Gamma 0.01..10, a NaN takes the default); the image itself is not clamped before the remap, the result lies in 0..1.
//
// The node shows a strip (button levels_node_strip_button) with the histogram of the Image input behind it and handles on the
// value axis: Levels black, midtone and white, Range window low edge, position and high edge. The handles edit the sockets
// (a linked socket's handle is greyed and not draggable). The histogram is a summary of a small side capture of the Image input only
// (the shared pass, image_node_type_t.summary_only: 256 x 256 float over the unit uv square in image space, refreshed when the graph changes,
// debounced, only for this strip and Auto Levels, never part of the output): 256 bins over all three channels (every pixel counts three
// samples, so a colour image shows the overall level distribution and a grey image its grey histogram), drawn as sqrt(bin / largest bin) so
// a few dominant bins do not flatten the rest. Auto Levels (button) sets Black and White to the 0.1 % and 99.9 % percentiles of those
// samples (Range: Position to their mean); Gamma and the Out values stay as they are, a flat image is left alone, a linked socket is never set.

// Input socket indices
enum {
	LEVELS_INPUT_IMAGE     = 0,
	LEVELS_INPUT_BLACK     = 1,
	LEVELS_INPUT_WHITE     = 2,
	LEVELS_INPUT_GAMMA     = 3,
	LEVELS_INPUT_OUT_BLACK = 4,
	LEVELS_INPUT_OUT_WHITE = 5,
	LEVELS_INPUT_POSITION  = 6,
	LEVELS_INPUT_WIDTH     = 7,
	LEVELS_INPUT_CONTRAST  = 8,
};

// Node button indices (settings)
enum {
	LEVELS_BUTTON_MODE   = 0,
	LEVELS_BUTTON_INVERT = 1,
	LEVELS_BUTTON_AUTO   = 2,
	LEVELS_BUTTON_STRIP  = 3,
};

// Mode setting
enum {
	LEVELS_MODE_LEVELS = 0,
	LEVELS_MODE_RANGE  = 1,
};

#define LEVELS_DEFAULT_GAMMA    1.0
#define LEVELS_DEFAULT_POSITION 0.5
#define LEVELS_DEFAULT_WIDTH    0.2
#define LEVELS_DEFAULT_CONTRAST 0.5
#define LEVELS_GAMMA_MIN        0.01
#define LEVELS_GAMMA_MAX        10.0
#define LEVELS_HANDLE_GAP       0.001 // Closest black and white handle
#define LEVELS_HISTOGRAM_BINS   256
#define LEVELS_FINE_BINS        1024 // Resolution of the percentiles
#define LEVELS_AUTO_CLIP        0.001 // Auto Levels clips this fraction of the samples on each side
#define LEVELS_CAPTURE_SIZE     256   // Width and height of the side capture of the Image input (65536 pixels)

// Histogram of the captured Image kept by the shared pass. Indices of the floats (test natives read them): bins 0..255, min 256, max 257, mean 258, low 259, high 260
typedef struct levels_summary {
	f32 bins[LEVELS_HISTOGRAM_BINS]; // Fraction of the samples per bin of the value 0..1, sums to 1
	f32 min;
	f32 max;
	f32 mean;
	f32 low;  // Percentile LEVELS_AUTO_CLIP
	f32 high; // Percentile 1 - LEVELS_AUTO_CLIP
} levels_summary_t;

// levels logic begin: pure functions, no node and no gpu (the host test compiles this part unchanged)
static f32 levels_node_clamp01(f32 x) {
	if (!(x > 0.0f)) {
		return 0.0f; // Also NaN
	}
	return x > 1.0f ? 1.0f : x;
}

static f32 levels_node_clamp_range(f32 x, f32 lo, f32 hi, f32 fallback) {
	if (!(x == x)) {
		return fallback;
	}
	return x < lo ? lo : x > hi ? hi : x;
}

// Position of the midtone handle on the value axis: the input that gives 0.5, black + (white - black) * 0.5^gamma
static f32 levels_node_mid_pos(f32 black, f32 white, f32 gamma) {
	f32 g = levels_node_clamp_range(gamma, (f32)LEVELS_GAMMA_MIN, (f32)LEVELS_GAMMA_MAX, 1.0f);
	return black + (white - black) * powf(0.5f, g);
}

// Gamma for a midtone handle at pos (inverse of levels_node_mid_pos), kept in the gamma range; a handle outside the range
// of black and white or a collapsed range gives the extremes / 1
static f32 levels_node_gamma_from_mid(f32 black, f32 white, f32 pos) {
	f32 range = white - black;
	if (!(range > (f32)LEVELS_HANDLE_GAP)) {
		return 1.0f;
	}
	f32 lo = powf(0.5f, (f32)LEVELS_GAMMA_MAX); // The fractions that the gamma limits give
	f32 hi = powf(0.5f, (f32)LEVELS_GAMMA_MIN);
	f32 m  = levels_node_clamp_range((pos - black) / range, lo, hi, 0.5f);
	f32 g  = logf(m) / logf(0.5f);
	return levels_node_clamp_range(g, (f32)LEVELS_GAMMA_MIN, (f32)LEVELS_GAMMA_MAX, 1.0f);
}

// Histogram of n floats (channels of the image): bins, min, max, mean and the percentiles low and high, over the samples clamped to 0..1 (NaN is 0)
static void levels_node_histogram(const f32 *samples, size_t n, levels_summary_t *out) {
	u32 fine[LEVELS_FINE_BINS];
	memset(fine, 0, sizeof(fine));
	f32    min = 1.0f, max = 0.0f;
	f64    sum = 0.0;
	for (size_t i = 0; i < n; ++i) {
		f32 v = levels_node_clamp01(samples[i]);
		i32 b = (i32)(v * (f32)LEVELS_FINE_BINS);
		fine[b >= LEVELS_FINE_BINS ? LEVELS_FINE_BINS - 1 : b]++;
		min = v < min ? v : min;
		max = v > max ? v : max;
		sum += v;
	}
	memset(out, 0, sizeof(levels_summary_t));
	if (n == 0) {
		return;
	}
	i32 per_bin = LEVELS_FINE_BINS / LEVELS_HISTOGRAM_BINS;
	for (i32 b = 0; b < LEVELS_FINE_BINS; ++b) {
		out->bins[b / per_bin] += (f32)((f64)fine[b] / (f64)n);
	}
	out->min  = min;
	out->max  = max;
	out->mean = (f32)(sum / (f64)n);
	f64 low_target  = LEVELS_AUTO_CLIP * (f64)n;
	f64 high_target = (1.0 - LEVELS_AUTO_CLIP) * (f64)n;
	f64 cum         = 0.0;
	bool low_done   = false;
	out->low        = min;
	out->high       = max;
	for (i32 b = 0; b < LEVELS_FINE_BINS; ++b) {
		f64 next = cum + (f64)fine[b];
		if (!low_done && next >= low_target && fine[b] > 0) {
			out->low = ((f32)b + (f32)((low_target - cum) / (f64)fine[b])) / (f32)LEVELS_FINE_BINS;
			low_done = true;
		}
		if (next >= high_target && fine[b] > 0) {
			out->high = ((f32)b + (f32)((high_target - cum) / (f64)fine[b])) / (f32)LEVELS_FINE_BINS;
			break;
		}
		cum = next;
	}
	out->low  = out->low < min ? min : out->low > max ? max : out->low;
	out->high = out->high < min ? min : out->high > max ? max : out->high;
}
// levels logic end

// The remap in the material shader (kong). Written in the common subset of kong and C so that the host test compiles this very text:
// plain float functions, no vectors except the three channel wrapper at the end. levels_core is the old CPU remap function.
char *str_levels = "\
float levels_fin(float v, float lo, float hi, float fb) { \
	if (v != v) { return fb; } \
	return clamp(v, lo, hi); \
} \
float levels_core(float x, float black, float white, float inv_gamma, float out_low, float out_high) { \
	float v = x; \
	if (v != v) { v = 0.0; } \
	float span = white - black; \
	float t = 0.0; \
	if (span < 0.000001) { \
		if (v > black) { t = 1.0; } \
	} \
	else { \
		t = clamp((v - black) / span, 0.0, 1.0); \
	} \
	if (inv_gamma != 1.0 && t > 0.0 && t < 1.0) { t = pow(t, inv_gamma); } \
	return out_low + (out_high - out_low) * t; \
} \
float levels_levels(float x, float black, float white, float gamma, float out_black, float out_white) { \
	return levels_core(x, levels_fin(black, 0.0, 1.0, 0.0), levels_fin(white, 0.0, 1.0, 1.0), 1.0 / levels_fin(gamma, 0.01, 10.0, 1.0), levels_fin(out_black, 0.0, 1.0, 0.0), levels_fin(out_white, 0.0, 1.0, 1.0)); \
} \
float levels_range(float x, float position, float width, float contrast, float invert) { \
	float v = x; \
	if (v != v) { v = 0.0; } \
	float center = levels_fin(position, 0.0, 1.0, 0.5); \
	float half_w = 0.5 * levels_fin(width, 0.0, 1.0, 0.0); \
	float ramp = (1.0 - levels_fin(contrast, 0.0, 1.0, 1.0)) * half_w; \
	float out_low = 1.0; \
	float out_high = 0.0; \
	if (invert > 0.5) { out_low = 0.0; out_high = 1.0; } \
	return levels_core(abs(v - center), half_w - ramp, half_w, 1.0, out_low, out_high); \
} \
float3 levels_levels3(float3 c, float black, float white, float gamma, float out_black, float out_white) { \
	return float3(levels_levels(c.x, black, white, gamma, out_black, out_white), levels_levels(c.y, black, white, gamma, out_black, out_white), levels_levels(c.z, black, white, gamma, out_black, out_white)); \
} \
float3 levels_range3(float3 c, float position, float width, float contrast, float invert) { \
	return float3(levels_range(c.x, position, width, contrast, invert), levels_range(c.y, position, width, contrast, invert), levels_range(c.z, position, width, contrast, invert)); \
} \
";

static i32 levels_node_mode(ui_node_t *node) {
	return (i32)make_image_node_button(node, LEVELS_BUTTON_MODE);
}

// Which rows of the node are not shown: Levels has no Invert and no Range inputs, Range no Levels inputs. A linked socket is never hidden by the caller (ui_nodes_on_item_hidden).
static bool levels_node_hidden(i32 mode, ui_node_item_t kind, i32 index) {
	bool range = mode == LEVELS_MODE_RANGE;
	if (kind == UI_NODE_ITEM_INPUT) {
		if (index >= LEVELS_INPUT_BLACK && index <= LEVELS_INPUT_OUT_WHITE) {
			return range;
		}
		return index >= LEVELS_INPUT_POSITION && !range;
	}
	if (kind == UI_NODE_ITEM_BUTTON) {
		return index == LEVELS_BUTTON_INVERT && !range;
	}
	return false;
}

static bool levels_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	return levels_node_hidden(levels_node_mode(node), kind, index);
}

// Side capture of the Image input only, for the histogram (the remap itself is the shader)
static i32 levels_node_capture_size(ui_node_t *node) {
	return LEVELS_CAPTURE_SIZE;
}

static char *levels_node_capture(ui_node_t *node) {
	char *image = parser_material_parse_vector_input(node->inputs->buffer[LEVELS_INPUT_IMAGE]);
	return string_tmp("float4(%s, 1.0)", image);
}

// The three colour channels of every captured pixel are the samples of the histogram
static void levels_node_summarize(u8 *bytes, i32 size, void *params, void *summary) {
	f32   *pixels = (f32 *)bytes;
	size_t n      = (size_t)size * size;
	f32   *plane  = malloc(n * 3 * sizeof(f32));
	if (plane == NULL) {
		return; // Keeps the previous summary
	}
	for (size_t i = 0; i < n; ++i) {
		plane[i * 3]     = pixels[i * 4];
		plane[i * 3 + 1] = pixels[i * 4 + 1];
		plane[i * 3 + 2] = pixels[i * 4 + 2];
	}
	levels_node_histogram(plane, n * 3, summary);
	free(plane);
}

static image_node_type_t levels_node_image_type = {
    .type          = "LEVELS",
    .summary_only  = true, // No result texture: the shader below is the output
    .size          = levels_node_capture_size,
    .capture       = levels_node_capture,
    .capture_float = true,
    .summary_size  = sizeof(levels_summary_t),
    .summarize     = levels_node_summarize,
};

char *levels_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	node_shader_add_function(parser_material_kong, str_levels);
	char *image = parser_material_parse_vector_input(node->inputs->buffer[LEVELS_INPUT_IMAGE]);
	if (levels_node_mode(node) == LEVELS_MODE_RANGE) {
		char *position = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_POSITION], false);
		char *width    = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_WIDTH], false);
		char *contrast = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_CONTRAST], false);
		bool  invert   = make_image_node_button(node, LEVELS_BUTTON_INVERT) != 0.0;
		return string_tmp("levels_range3(%s, %s, %s, %s, %s)", image, position, width, contrast, invert ? "1.0" : "0.0");
	}
	char *black     = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_BLACK], false);
	char *white     = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_WHITE], false);
	char *gamma     = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_GAMMA], false);
	char *out_black = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_OUT_BLACK], false);
	char *out_white = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_OUT_WHITE], false);
	return string_tmp("levels_levels3(%s, %s, %s, %s, %s, %s)", image, black, white, gamma, out_black, out_white);
}

// True when input i of the node has a link (that socket is driven by the graph, its value is not edited here)
static bool levels_node_linked(ui_node_canvas_t *canvas, i32 node_id, i32 i) {
	for (i32 k = 0; k < canvas->links->length; ++k) {
		ui_node_link_t *l = canvas->links->buffer[k];
		if (l->to_id == node_id && l->to_socket == i) {
			return true;
		}
	}
	return false;
}

static f32 levels_node_input(ui_node_t *node, i32 i) {
	return node->inputs->buffer[i]->default_value->buffer[0];
}

static void levels_node_set_input(ui_node_t *node, i32 i, f32 v) {
	node->inputs->buffer[i]->default_value->buffer[0] = v;
}

static levels_summary_t *levels_node_summary_of(ui_node_t *node) {
	if (!levels_node_linked(ui_nodes_get_canvas(true), node->id, LEVELS_INPUT_IMAGE)) {
		return NULL; // An unlinked Image is plain white: no histogram
	}
	return make_image_node_summary(node);
}

// Auto Levels button: Black and White from the percentiles of the baked Image, in Range mode the Position from its mean. A linked socket keeps its link and is not set.
static void levels_node_auto_button(i32 node_id) {
	ui_node_t *node = ui_get_node(ui_nodes_get_canvas(true)->nodes, node_id);
	if (node == NULL) {
		return;
	}
	levels_summary_t *s       = levels_node_summary_of(node);
	ui_node_canvas_t *canvas  = ui_nodes_get_canvas(true);
	bool              enabled = g_ui->enabled;
	g_ui->enabled             = enabled && s != NULL;
	bool pressed              = ui_button(tr("Auto Levels"), UI_ALIGN_CENTER, "");
	if (g_ui->is_hovered) {
		ui_tooltip(tr("Set the points from the input image"));
	}
	g_ui->enabled = enabled;
	if (!pressed || s == NULL) {
		return;
	}
	if (levels_node_mode(node) == LEVELS_MODE_RANGE) {
		if (!levels_node_linked(canvas, node->id, LEVELS_INPUT_POSITION)) {
			levels_node_set_input(node, LEVELS_INPUT_POSITION, s->mean);
		}
	}
	else if (s->high - s->low > (f32)LEVELS_HANDLE_GAP) { // A flat image has no range to fit
		if (!levels_node_linked(canvas, node->id, LEVELS_INPUT_BLACK)) {
			levels_node_set_input(node, LEVELS_INPUT_BLACK, s->low);
		}
		if (!levels_node_linked(canvas, node->id, LEVELS_INPUT_WHITE)) {
			levels_node_set_input(node, LEVELS_INPUT_WHITE, s->high);
		}
	}
	g_ui->changed = true;
}

// Strip handles: Levels 0 black, 1 midtone, 2 white; Range 0 window low edge, 1 position, 2 window high edge (positions on the value axis 0..1)
typedef struct levels_strip {
	ui_node_t        *node;
	ui_node_canvas_t *canvas;
	bool              range;
} levels_strip_t;

static i32 levels_strip_count(void *data) {
	return 3;
}

static f32 levels_strip_get_pos(void *data, i32 i) {
	levels_strip_t *st = data;
	ui_node_t      *n  = st->node;
	if (st->range) {
		f32 p = levels_node_clamp_range(levels_node_input(n, LEVELS_INPUT_POSITION), 0.0f, 1.0f, 0.5f);
		f32 h = 0.5f * levels_node_clamp_range(levels_node_input(n, LEVELS_INPUT_WIDTH), 0.0f, 1.0f, 0.0f);
		return i == 0 ? levels_node_clamp01(p - h) : i == 1 ? p : levels_node_clamp01(p + h);
	}
	f32 b = levels_node_clamp_range(levels_node_input(n, LEVELS_INPUT_BLACK), 0.0f, 1.0f, 0.0f);
	f32 w = levels_node_clamp_range(levels_node_input(n, LEVELS_INPUT_WHITE), 0.0f, 1.0f, 1.0f);
	return i == 0 ? b : i == 1 ? levels_node_mid_pos(b, w, levels_node_input(n, LEVELS_INPUT_GAMMA)) : w;
}

static void levels_strip_set_pos(void *data, i32 i, f32 pos) {
	levels_strip_t *st = data;
	ui_node_t      *n  = st->node;
	if (st->range) {
		f32 p = levels_node_clamp_range(levels_node_input(n, LEVELS_INPUT_POSITION), 0.0f, 1.0f, 0.5f);
		if (i == 1) {
			levels_node_set_input(n, LEVELS_INPUT_POSITION, levels_node_clamp01(pos));
		}
		else { // An edge moves symmetrically: the width is twice the distance to the position
			f32 w = 2.0f * (i == 0 ? p - pos : pos - p);
			levels_node_set_input(n, LEVELS_INPUT_WIDTH, levels_node_clamp01(w));
		}
		return;
	}
	f32 b = levels_node_clamp_range(levels_node_input(n, LEVELS_INPUT_BLACK), 0.0f, 1.0f, 0.0f);
	f32 w = levels_node_clamp_range(levels_node_input(n, LEVELS_INPUT_WHITE), 0.0f, 1.0f, 1.0f);
	if (i == 0) { // The handles never cross: black stays LEVELS_HANDLE_GAP below white
		f32 top = w - (f32)LEVELS_HANDLE_GAP;
		levels_node_set_input(n, LEVELS_INPUT_BLACK, pos > top ? (top < 0.0f ? 0.0f : top) : levels_node_clamp01(pos));
	}
	else if (i == 2) {
		f32 bottom = b + (f32)LEVELS_HANDLE_GAP;
		levels_node_set_input(n, LEVELS_INPUT_WHITE, pos < bottom ? (bottom > 1.0f ? 1.0f : bottom) : levels_node_clamp01(pos));
	}
	else {
		levels_node_set_input(n, LEVELS_INPUT_GAMMA, levels_node_gamma_from_mid(b, w, pos));
	}
}

static i32 levels_strip_socket_of(levels_strip_t *st, i32 i) {
	if (st->range) {
		return i == 1 ? LEVELS_INPUT_POSITION : LEVELS_INPUT_WIDTH;
	}
	return i == 0 ? LEVELS_INPUT_BLACK : i == 1 ? LEVELS_INPUT_GAMMA : LEVELS_INPUT_WHITE;
}

static bool levels_strip_is_locked(void *data, i32 i) {
	levels_strip_t *st = data;
	return levels_node_linked(st->canvas, st->node->id, levels_strip_socket_of(st, i));
}

static void levels_strip_draw(void *data, i32 i, f32 cx, f32 y, f32 w, f32 h, bool active) {
	levels_strip_t *st = data;
	if (levels_strip_is_locked(data, i)) {
		ui_stops_draw_pin(cx, y, w, h, 0xff4a4a4a, false); // Linked: greyed, not draggable
		return;
	}
	u32 fill = st->range ? (i == 1 ? 0xff9a9a9a : 0xffd8d8d8) : (i == 0 ? 0xff202020 : i == 1 ? 0xff808080 : 0xffffffff);
	ui_stops_draw_pin(cx, y, w, h, fill, active);
}

#define LEVELS_STRIP_BAR_LINES    1.6f // The histogram and value gradient
#define LEVELS_STRIP_MARKER_LINES 0.5f // The row of handles
#define LEVELS_STRIP_GAP_LINES    0.2f
#define LEVELS_STRIP_HEIGHT       (LEVELS_STRIP_BAR_LINES + LEVELS_STRIP_MARKER_LINES + LEVELS_STRIP_GAP_LINES) // Lines of the node button

static void levels_node_strip_button(i32 node_id) {
	ui_nodes_t       *nodes  = ui_nodes_get_nodes();
	ui_node_canvas_t *canvas = ui_nodes_get_canvas(true);
	ui_node_t        *node   = ui_get_node(canvas->nodes, node_id);
	if (node == NULL) {
		return;
	}
	ui_nodes_editor_state_t *state = ui_nodes_editor_state(node);
	levels_strip_t           strip = {.node = node, .canvas = canvas, .range = levels_node_mode(node) == LEVELS_MODE_RANGE};
	f32 sw       = g_ui->_w / (f32)UI_NODES_SCALE();
	f32 bar_h    = UI_LINE_H() * LEVELS_STRIP_BAR_LINES - 2 * UI_NODES_SCALE();
	f32 grad_h   = UI_LINE_H() * 0.25f; // Value gradient along the bottom of the bar
	f32 hist_h   = bar_h - grad_h;
	f32 marker_h = UI_LINE_H() * LEVELS_STRIP_MARKER_LINES;
	f32 hit_h    = UI_LINE_H() * 0.7f;
	f32 gap_h    = UI_LINE_H() * LEVELS_STRIP_GAP_LINES;

	ui_fill(0, 0, sw, hist_h / UI_SCALE(), 0xff1c1c1c);
	levels_summary_t *summary = levels_node_summary_of(node);
	if (summary != NULL) {
		f32 largest = 0.0f;
		for (i32 b = 0; b < LEVELS_HISTOGRAM_BINS; ++b) {
			largest = summary->bins[b] > largest ? summary->bins[b] : largest;
		}
		for (i32 b = 0; b < LEVELS_HISTOGRAM_BINS && largest > 0.0f; ++b) {
			f32 h = sqrtf(summary->bins[b] / largest) * hist_h; // sqrt: a few dominant bins do not flatten the rest
			if (h > 0.0f) {
				f32 x0 = sw * (f32)b / LEVELS_HISTOGRAM_BINS;
				f32 x1 = sw * (f32)(b + 1) / LEVELS_HISTOGRAM_BINS;
				ui_fill(x0, (hist_h - h) / UI_SCALE(), x1 - x0 + 1.0f / UI_NODES_SCALE(), h / UI_SCALE(), 0xff8c8c8c);
			}
		}
	}
	if (strip.range) { // The window the mask selects
		f32 lo = levels_strip_get_pos(&strip, 0);
		f32 hi = levels_strip_get_pos(&strip, 2);
		ui_fill(lo * sw, 0, (hi - lo) * sw, hist_h / UI_SCALE(), 0x40ffffff);
	}
	i32 slices = 32;
	for (i32 s = 0; s < slices; ++s) {
		f32 t = (f32)s / (f32)slices;
		u32 c = color_from_floats(t, t, t, 1.0);
		ui_fill(t * sw, hist_h / UI_SCALE(), sw / (f32)slices + 1.0f / UI_NODES_SCALE(), grad_h / UI_SCALE(), c);
	}

	ui_stops_ops_t ops = {.count     = levels_strip_count,
	                      .get_pos   = levels_strip_get_pos,
	                      .set_pos   = levels_strip_set_pos,
	                      .add       = NULL,
	                      .remove    = NULL,
	                      .draw      = levels_strip_draw,
	                      .is_locked = levels_strip_is_locked,
	                      .min_count = 3,
	                      .max_count = 3};
	i32 hit = ui_stops_update(&state->stops, &ops, &strip, g_ui->_x, g_ui->_y, g_ui->_w, bar_h, marker_h, hit_h);
	if (hit & UI_STOPS_CONSUMED) {
		nodes->_input_started  = false;
		g_context->select_time = 0; // Two quick presses on handles are not a node double click (2D view)
	}
	g_ui->_y += UI_LINE_H() * LEVELS_STRIP_BAR_LINES + marker_h + gap_h;
}

// A node of the first (baked) Levels build, type TEX_LEVELS with Size and a state row: it has a layout this code cannot read and the type is
// not defined any more, so it loads as an unknown node with a black output and these two buttons draw nothing. Delete it and add a new Levels node.
static void levels_node_legacy_button(i32 node_id) {
}

// The sections of the node: General (Mode, Invert, Auto Levels, the strip and Image) and Parameters (the other inputs). No Texture section:
// the node has no baked texture.
static void levels_node_sections(ui_node_t *def) {
	ui_node_section_t *list = calloc(3, sizeof(ui_node_section_t)); // The list ends with the zeroed entry
	list[0]                 = (ui_node_section_t){.title             = _tr("General"),
	                                              .kind              = UI_NODE_ITEM_BUTTON,
	                                              .first             = 0,
	                                              .count             = def->buttons->length,
	                                              .closed_by_default = false,
	                                              .inputs_first      = 0,
	                                              .inputs_count      = 1};
	list[1]                 = (ui_node_section_t){.title = _tr("Parameters"), .kind = UI_NODE_ITEM_INPUT, .first = 1, .count = def->inputs->length - 1, .closed_by_default = false};
	any_map_set(ui_nodes_sections, def->type, list);
}

void levels_node_init() {
	u32 color_socket = 0xffc7c729; // The usual yellow of an RGBA socket
	u32 value_color  = 0xffa1a1a1;

	ui_node_t *levels_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Levels"),
	                .type   = "LEVELS",
	                .x      = 0,
	                .y      = 0,
	                .color  = 0xff62676d, // The grey header of Map Range and the Color nodes: a pass-through node, not an orange image node
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "RGBA", color_socket, f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Black"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("White"), "VALUE", value_color, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Gamma"), "VALUE", value_color, f32_array_create_x(LEVELS_DEFAULT_GAMMA), LEVELS_GAMMA_MIN, LEVELS_GAMMA_MAX, 100),
	                        make_image_node_socket(_tr("Out Black"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Out White"), "VALUE", value_color, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Position"), "VALUE", value_color, f32_array_create_x(LEVELS_DEFAULT_POSITION), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Width"), "VALUE", value_color, f32_array_create_x(LEVELS_DEFAULT_WIDTH), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Contrast"), "VALUE", value_color, f32_array_create_x(LEVELS_DEFAULT_CONTRAST), 0.0, 1.0, 100),
	                    },
	                    9),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "RGBA", color_socket, f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Mode"), "ENUM", LEVELS_MODE_LEVELS, "Levels\nRange"),
	                        make_image_node_setting(_tr("Invert"), "BOOL", 0, NULL),
	                        ALLOC_INIT(ui_node_button_t, {.name          = "levels_auto_button",
	                                                      .type          = "CUSTOM",
	                                                      .output        = -1,
	                                                      .default_value = f32_array_create_x(0),
	                                                      .data          = NULL,
	                                                      .min           = 0.0,
	                                                      .max           = 1.0,
	                                                      .precision     = 100,
	                                                      .height        = 1}),
	                        ALLOC_INIT(ui_node_button_t, {.name          = "levels_strip_button",
	                                                      .type          = "CUSTOM",
	                                                      .output        = -1,
	                                                      .default_value = f32_array_create_x(0),
	                                                      .data          = NULL,
	                                                      .min           = 0.0,
	                                                      .max           = 1.0,
	                                                      .precision     = 100,
	                                                      .height        = LEVELS_STRIP_HEIGHT}),
	                    },
	                    4),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, levels_node_def);
	any_map_set(parser_material_node_vectors, "LEVELS", levels_node_vector);
	any_map_set(ui_nodes_hidden_rules, "LEVELS", levels_node_item_hidden);
	any_map_set(ui_nodes_custom_buttons, "levels_auto_button", levels_node_auto_button);
	any_map_set(ui_nodes_custom_buttons, "levels_strip_button", levels_node_strip_button);
	any_map_set(ui_nodes_custom_buttons, "levels_node_auto_button", levels_node_legacy_button); // The first build (see levels_node_legacy_button)
	any_map_set(ui_nodes_custom_buttons, "levels_node_strip_button", levels_node_legacy_button);
	levels_node_sections(levels_node_def);
	make_image_node_register(&levels_node_image_type);
}
