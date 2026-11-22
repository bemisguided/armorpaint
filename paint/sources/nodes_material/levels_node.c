#include "../global.h"

// Levels (Filter menu): one remap of the values of the Image input, the same function on R, G and B. A pass-through shader node like Map Range
// and Gamma: the remap is computed per pixel in the material shader (str_levels below), no baked result, no Size. The Image input and every
// value input (In Low, In High, In Mid, Out Low, Out High, Position, Width, Contrast) can be linked and are evaluated per pixel. The alpha of a
// colour is not carried by shader colour sockets (as for every colour node), so it passes through as it is.
// The names and the model are those of the Levels node of Substance Designer (Blender's Map Range is the same family), no Gamma socket.
// Two modes of the same core (levels_core), spec specs/image-nodes/IMAGE_NODES.md (Levels):
//   Levels: t = (x - in_low) / (in_high - in_low), clamped to 0..1 when Intermediary Clamp is on, then the midtone curve and the output range:
//           out = out_low + (out_high - out_low) * pow(t, inv_gamma), the curve only for 0 < t < 1 (linear outside, so no NaN from a negative base).
//           In Mid is the ABSOLUTE input value that maps to mid grey (0.5 of t): inv_gamma = log(0.5) / log((in_mid - in_low) / (in_high - in_low)),
//           the fraction kept inside the limits of the old gamma 0.01..10 (0.5^10 .. 0.5^0.01), so In Mid stays strictly between In Low and
//           In High; a NaN or a degenerate mid falls back to the midpoint (inv_gamma 1, a plain linear remap). Out Low above Out High reverses the image.
//   Intermediary Clamp (setting, default on): on, t is clamped to 0..1 before the output range (the result stays between Out Low and Out High);
//           off, t is NOT clamped: values outside In Low .. In High extrapolate linearly and the result may leave Out Low .. Out High and 0 .. 1
//           (a plain linear remap, Blender's Map Range without Clamp). The shader clamps the input to +-1e6 first so that nothing becomes NaN
//           (inf input, 0 * inf). Range mode always clamps (a mask in 0..1): the setting is hidden there.
//   Range:  a window of the values around Position (the Histogram Select of Substance Designer): d = |x - position|, half = Width / 2,
//           ramp = (1 - Contrast) * half, out = 1 - clamp((d - (half - ramp)) / ramp, 0, 1): 1 inside the window, falling linearly to 0 over the
//           ramp that ends at the window edge (position +- half). Contrast 1 gives a hard edge, 0 a triangle. Invert gives 1 - out.
//           It is the Levels remap with low = half - ramp, high = half, gamma 1, out low 1, out high 0 (swapped by Invert), applied to d.
// A range of zero width (In High - In Low below 1e-6: In High not above In Low, Width 0, Contrast 1) is a hard step at In Low (x above it
// gives 1). All values are clamped to their slider range in the shader (0..1, a NaN takes the default); the image itself is not clamped
// before the remap, with Intermediary Clamp on the result lies in 0..1.
//
// The node shows a strip (button levels_node_strip_button). Levels mode: the three INPUT handles In Low, In Mid, In High sit on top of the
// histogram (the house flipped, roof down, its tip on the frame of the histogram box), the histogram of the Image input in a box that stands
// out from the node, below it ONE gradient bar, the OUTPUT range, and the two OUTPUT handles Out Low and Out High below the bar (house, roof up).
// Range mode: the histogram box with the window, the gradient bar and the window low edge, position and high edge below it (unchanged).
// The handles edit the sockets (a linked socket's handle is greyed and not draggable); In Low <= In Mid <= In High (the handles stop a gap
// apart), Out Low and Out High may cross. The track is inset by the handle extent (ui_stops_pin_extent) so no handle reaches out of the node.
// The histogram is a summary of a small side capture of the Image input only
// (the shared pass, image_node_type_t.summary_only: 256 x 256 float over the unit uv square in image space, refreshed when the graph changes,
// debounced, only for this strip and Auto Levels, never part of the output): 256 bins over all three channels (every pixel counts three
// samples, so a colour image shows the overall level distribution and a grey image its grey histogram), drawn as sqrt(bin / largest bin) so
// a few dominant bins do not flatten the rest. Auto Levels (button) sets In Low and In High to the 0.1 % and 99.9 % percentiles of those
// samples (Range: Position to their mean); In Mid and the Out values stay as they are, a flat image is left alone, a linked socket is never set.

// Input socket indices
enum {
	LEVELS_INPUT_IMAGE     = 0,
	LEVELS_INPUT_IN_LOW    = 1,
	LEVELS_INPUT_IN_HIGH   = 2,
	LEVELS_INPUT_IN_MID    = 3,
	LEVELS_INPUT_OUT_LOW   = 4,
	LEVELS_INPUT_OUT_HIGH  = 5,
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
	LEVELS_BUTTON_CLAMP  = 4, // Intermediary Clamp, last so the indices above stay as they were; drawn below the sliders (trailing member of Parameters)
};

// Mode setting
enum {
	LEVELS_MODE_LEVELS = 0,
	LEVELS_MODE_RANGE  = 1,
};

#define LEVELS_DEFAULT_IN_MID   0.5
#define LEVELS_DEFAULT_POSITION 0.5
#define LEVELS_DEFAULT_WIDTH    0.2
#define LEVELS_DEFAULT_CONTRAST 0.5
#define LEVELS_GAMMA_MIN        0.01
#define LEVELS_GAMMA_MAX        10.0
#define LEVELS_HANDLE_GAP       0.001 // Closest In Low, In Mid and In High handles
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

// Fraction of the input range (In Low .. In High) at which In Mid sits, as the shader uses it: kept inside the limits of the old gamma 0.01..10,
// 0.5 for a NaN or a degenerate range
static f32 levels_node_mid_fraction(f32 low, f32 high, f32 mid) {
	f32 span = high - low;
	if (!(span >= 0.000001f)) {
		return 0.5f;
	}
	return levels_node_clamp_range((mid - low) / span, powf(0.5f, (f32)LEVELS_GAMMA_MAX), powf(0.5f, (f32)LEVELS_GAMMA_MIN), 0.5f);
}

// Position of the In Mid handle on the value axis: In Mid as the shader uses it (strictly inside In Low .. In High)
static f32 levels_node_mid_pos(f32 low, f32 high, f32 mid) {
	return low + (high - low) * levels_node_mid_fraction(low, high, mid);
}

// The exponent of the midtone curve for In Mid (the CPU twin of levels_inv_gamma of the shader): the input at In Mid gives 0.5
static f32 levels_node_inv_gamma(f32 low, f32 high, f32 mid) {
	return logf(0.5f) / logf(levels_node_mid_fraction(low, high, mid));
}

// Where a dragged input handle may go: pos kept between lo and hi (a collapsed interval gives lo), NaN gives lo
static f32 levels_node_limit(f32 pos, f32 lo, f32 hi) {
	f32 p = levels_node_clamp_range(pos, 0.0f, 1.0f, 0.0f);
	if (hi < lo) {
		hi = lo;
	}
	return p < lo ? lo : p > hi ? hi : p;
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
float levels_inv_gamma(float low, float high, float mid) { \
	float span = high - low; \
	float frac = 0.5; \
	if (span >= 0.000001) { frac = (mid - low) / span; } \
	if (frac != frac) { frac = 0.5; } \
	frac = clamp(frac, 0.0009765625, 0.99309249); \
	return log(0.5) / log(frac); \
} \
float levels_remap(float x, float low, float high, float inv_gamma, float out_low, float out_high, float clamp_t) { \
	float v = x; \
	if (v != v) { v = 0.0; } \
	v = clamp(v, -1000000.0, 1000000.0); \
	float span = high - low; \
	float t = 0.0; \
	if (span < 0.000001) { \
		if (v > low) { t = 1.0; } \
	} \
	else { \
		t = (v - low) / span; \
		if (clamp_t > 0.5) { t = clamp(t, 0.0, 1.0); } \
	} \
	if (inv_gamma != 1.0 && t > 0.0 && t < 1.0) { t = pow(t, inv_gamma); } \
	return out_low + (out_high - out_low) * t; \
} \
float levels_core(float x, float in_low, float in_high, float in_mid, float out_low, float out_high, float clamp_t) { \
	return levels_remap(x, in_low, in_high, levels_inv_gamma(in_low, in_high, in_mid), out_low, out_high, clamp_t); \
} \
float levels_levels(float x, float in_low, float in_high, float in_mid, float out_low, float out_high, float clamp_t) { \
	return levels_core(x, levels_fin(in_low, 0.0, 1.0, 0.0), levels_fin(in_high, 0.0, 1.0, 1.0), in_mid, levels_fin(out_low, 0.0, 1.0, 0.0), levels_fin(out_high, 0.0, 1.0, 1.0), clamp_t); \
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
	return levels_remap(abs(v - center), half_w - ramp, half_w, 1.0, out_low, out_high, 1.0); \
} \
float3 levels_levels3(float3 c, float in_low, float in_high, float in_mid, float out_low, float out_high, float clamp_t) { \
	return float3(levels_levels(c.x, in_low, in_high, in_mid, out_low, out_high, clamp_t), levels_levels(c.y, in_low, in_high, in_mid, out_low, out_high, clamp_t), levels_levels(c.z, in_low, in_high, in_mid, out_low, out_high, clamp_t)); \
} \
float3 levels_range3(float3 c, float position, float width, float contrast, float invert) { \
	return float3(levels_range(c.x, position, width, contrast, invert), levels_range(c.y, position, width, contrast, invert), levels_range(c.z, position, width, contrast, invert)); \
} \
";

static i32 levels_node_mode(ui_node_t *node) {
	return (i32)make_image_node_button(node, LEVELS_BUTTON_MODE);
}

// Which rows of the node are not shown: Levels has no Invert and no Range inputs, Range no Levels inputs and no Intermediary Clamp. A linked socket is never hidden by the caller (ui_nodes_on_item_hidden).
static bool levels_node_hidden(i32 mode, ui_node_item_t kind, i32 index) {
	bool range = mode == LEVELS_MODE_RANGE;
	if (kind == UI_NODE_ITEM_INPUT) {
		if (index >= LEVELS_INPUT_IN_LOW && index <= LEVELS_INPUT_OUT_HIGH) {
			return range;
		}
		return index >= LEVELS_INPUT_POSITION && !range;
	}
	if (kind == UI_NODE_ITEM_BUTTON) {
		return index == LEVELS_BUTTON_INVERT ? !range : index == LEVELS_BUTTON_CLAMP ? range : false;
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
    .capture_format = IMAGE_CAPTURE_RGBA32F,
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
	char *in_low   = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_IN_LOW], false);
	char *in_high  = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_IN_HIGH], false);
	char *in_mid   = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_IN_MID], false);
	char *out_low  = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_OUT_LOW], false);
	char *out_high = parser_material_parse_value_input(node->inputs->buffer[LEVELS_INPUT_OUT_HIGH], false);
	bool  clamped  = make_image_node_button(node, LEVELS_BUTTON_CLAMP) != 0.0;
	return string_tmp("levels_levels3(%s, %s, %s, %s, %s, %s, %s)", image, in_low, in_high, in_mid, out_low, out_high, clamped ? "1.0" : "0.0");
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

// Auto Levels button: In Low and In High from the percentiles of the baked Image (never In Mid or the Out values), in Range mode the Position from its mean. A linked socket keeps its link and is not set.
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
		if (!levels_node_linked(canvas, node->id, LEVELS_INPUT_IN_LOW)) {
			levels_node_set_input(node, LEVELS_INPUT_IN_LOW, s->low);
		}
		if (!levels_node_linked(canvas, node->id, LEVELS_INPUT_IN_HIGH)) {
			levels_node_set_input(node, LEVELS_INPUT_IN_HIGH, s->high);
		}
	}
	g_ui->changed = true;
}

// The strip has two rows of handles, each its own stops control (own active handle and drag):
//   Levels, input row (above the histogram): 0 In Low, 1 In Mid, 2 In High; output row (below the gradient bar): 0 Out Low, 1 Out High.
//   Range, one row below the bar: 0 window low edge, 1 position, 2 window high edge.
// Positions on the value axis 0..1.
typedef struct levels_strip {
	ui_node_t        *node;
	ui_node_canvas_t *canvas;
	bool              range;
	bool              output_row; // Levels: the Out Low / Out High row
} levels_strip_t;

static i32 levels_strip_count(void *data) {
	levels_strip_t *st = data;
	return st->output_row ? 2 : 3;
}

static f32 levels_strip_value(levels_strip_t *st, i32 socket, f32 fallback) {
	return levels_node_clamp_range(levels_node_input(st->node, socket), 0.0f, 1.0f, fallback);
}

static f32 levels_strip_get_pos(void *data, i32 i) {
	levels_strip_t *st = data;
	if (st->range) {
		f32 p = levels_strip_value(st, LEVELS_INPUT_POSITION, 0.5f);
		f32 h = 0.5f * levels_strip_value(st, LEVELS_INPUT_WIDTH, 0.0f);
		return i == 0 ? levels_node_clamp01(p - h) : i == 1 ? p : levels_node_clamp01(p + h);
	}
	if (st->output_row) {
		return i == 0 ? levels_strip_value(st, LEVELS_INPUT_OUT_LOW, 0.0f) : levels_strip_value(st, LEVELS_INPUT_OUT_HIGH, 1.0f);
	}
	f32 low  = levels_strip_value(st, LEVELS_INPUT_IN_LOW, 0.0f);
	f32 high = levels_strip_value(st, LEVELS_INPUT_IN_HIGH, 1.0f);
	return i == 0 ? low : i == 1 ? levels_node_mid_pos(low, high, levels_strip_value(st, LEVELS_INPUT_IN_MID, 0.5f)) : high;
}

static i32 levels_strip_socket_of(levels_strip_t *st, i32 i) {
	if (st->range) {
		return i == 1 ? LEVELS_INPUT_POSITION : LEVELS_INPUT_WIDTH;
	}
	if (st->output_row) {
		return i == 0 ? LEVELS_INPUT_OUT_LOW : LEVELS_INPUT_OUT_HIGH;
	}
	return i == 0 ? LEVELS_INPUT_IN_LOW : i == 1 ? LEVELS_INPUT_IN_MID : LEVELS_INPUT_IN_HIGH;
}

static bool levels_strip_is_locked(void *data, i32 i) {
	levels_strip_t *st = data;
	return levels_node_linked(st->canvas, st->node->id, levels_strip_socket_of(st, i));
}

// An input handle never crosses its neighbours: In Low <= In Mid <= In High, a gap apart. The limits use the STORED In Mid (the handle
// is drawn at the value the shader uses, held inside the range: limiting by that would ratchet the end handle inwards). A neighbour that is linked
// has no value here to bump into, the next one that is not linked (or the end of the axis) limits instead; an In Mid outside the range
// (set by Auto Levels or a slider) does not limit the ends.
static void levels_strip_set_input_handle(levels_strip_t *st, i32 i, f32 pos) {
	ui_node_t *n         = st->node;
	f32        low       = levels_strip_get_pos(st, 0);
	f32        high      = levels_strip_get_pos(st, 2);
	f32        mid       = levels_strip_value(st, LEVELS_INPUT_IN_MID, 0.5f);
	f32        gap       = (f32)LEVELS_HANDLE_GAP;
	bool       lo_free   = !levels_strip_is_locked(st, 0);
	bool       mid_free  = !levels_strip_is_locked(st, 1);
	bool       hi_free   = !levels_strip_is_locked(st, 2);
	bool       mid_inside = mid_free && mid > low && mid < high;
	if (i == 0) {
		f32 top = hi_free ? high - gap : 1.0f;
		if (mid_inside && mid - gap < top) {
			top = mid - gap;
		}
		levels_node_set_input(n, LEVELS_INPUT_IN_LOW, levels_node_limit(pos, 0.0f, top));
	}
	else if (i == 2) {
		f32 bottom = lo_free ? low + gap : 0.0f;
		if (mid_inside && mid + gap > bottom) {
			bottom = mid + gap;
		}
		levels_node_set_input(n, LEVELS_INPUT_IN_HIGH, levels_node_limit(pos, bottom, 1.0f));
	}
	else {
		f32 bottom = lo_free ? low + gap : 0.0f;
		f32 top    = hi_free ? high - gap : 1.0f;
		levels_node_set_input(n, LEVELS_INPUT_IN_MID, levels_node_limit(pos, bottom, top));
	}
}

static void levels_strip_set_pos(void *data, i32 i, f32 pos) {
	levels_strip_t *st = data;
	ui_node_t      *n  = st->node;
	if (st->range) {
		f32 p = levels_strip_value(st, LEVELS_INPUT_POSITION, 0.5f);
		if (i == 1) {
			levels_node_set_input(n, LEVELS_INPUT_POSITION, levels_node_clamp01(pos));
		}
		else { // An edge moves symmetrically: the width is twice the distance to the position
			f32 w = 2.0f * (i == 0 ? p - pos : pos - p);
			levels_node_set_input(n, LEVELS_INPUT_WIDTH, levels_node_clamp01(w));
		}
		return;
	}
	if (st->output_row) { // The output values may cross: reversing the image is allowed
		levels_node_set_input(n, i == 0 ? LEVELS_INPUT_OUT_LOW : LEVELS_INPUT_OUT_HIGH, levels_node_clamp01(pos));
		return;
	}
	levels_strip_set_input_handle(st, i, pos);
}

static u32 levels_strip_fill(levels_strip_t *st, i32 i) {
	if (st->range) {
		return i == 1 ? 0xff9a9a9a : 0xffd8d8d8;
	}
	if (st->output_row) {
		return i == 0 ? 0xff202020 : 0xffffffff;
	}
	return i == 0 ? 0xff202020 : i == 1 ? 0xff808080 : 0xffffffff;
}

static u32 levels_strip_fill_cb(void *data, i32 i) {
	return levels_strip_fill(data, i);}

// Rows of the strip in lines of the node button (Levels): the input handles (the press zone above the histogram), the histogram box, a small gap,
// the output gradient bar, the output handle row and the space below it. Range uses the same height: box, bar, handle row, the rest is blank.
#define LEVELS_STRIP_TOP_LINES    0.6f  // Input handle row: houses 0.5 line tall inside it
#define LEVELS_STRIP_HIST_LINES   1.0f  // Histogram box
#define LEVELS_STRIP_GAP_LINES    0.12f // Between the box and the output bar
#define LEVELS_STRIP_BAR_LINES    0.3f  // Output gradient bar
#define LEVELS_STRIP_MARKER_LINES 0.5f  // Row of the output handles
#define LEVELS_STRIP_BELOW_LINES  0.3f  // Space below the output handles (the press zone reaches into it)
#define LEVELS_STRIP_HIT_LINES    0.6f  // Press zone of a handle row
#define LEVELS_STRIP_HEIGHT       (LEVELS_STRIP_TOP_LINES + LEVELS_STRIP_HIST_LINES + LEVELS_STRIP_GAP_LINES + LEVELS_STRIP_BAR_LINES + LEVELS_STRIP_MARKER_LINES + LEVELS_STRIP_BELOW_LINES) // Lines of the node button

static u32 levels_strip_mix(u32 a, u32 b, f32 t) {
	u32 out = 0xff000000;
	for (i32 shift = 0; shift < 24; shift += 8) {
		f32 ca = (f32)((a >> shift) & 0xff);
		f32 cb = (f32)((b >> shift) & 0xff);
		out |= (u32)(ca + (cb - ca) * t + 0.5f) << shift;
	}
	return out;
}

// A filled rect at pixel offsets from the origin of the button (the node row), faded like ui_fill while the node is disabled
static void levels_strip_rect(f32 x, f32 y, f32 w, f32 h, u32 color) {
	draw_set_color(color);
	if (!g_ui->enabled) {
		ui_fade_color(0.25);
	}
	draw_filled_rect(g_ui->_x + x, g_ui->_y + y, w, h);
	draw_set_color(0xffffffff);
}

// The histogram in its box: a panel that stands out from the node (darker than the node background, in the light theme too), a frame of the
// handle ring width (the roof tips of the input handles touch it), the bars inside. x, w: the track; y, h: the whole box including the frame.
static void levels_strip_histogram(f32 x, f32 y, f32 w, f32 h, f32 frame, levels_summary_t *summary, u32 text_col, u32 bg_col) {
	u32 panel = levels_strip_mix(bg_col, 0xff000000, (bg_col & 0xff) > 0x90 ? 0.2f : 0.45f); // A light theme needs less black
	u32 line  = levels_strip_mix(text_col, bg_col, 0.55f); // The frame grey of the handles
	u32 bars  = levels_strip_mix(panel, text_col, 0.55f);
	levels_strip_rect(x, y, w, h, line);
	levels_strip_rect(x + frame, y + frame, w - 2 * frame, h - 2 * frame, panel);
	if (summary == NULL) {
		return;
	}
	f32 inner_x = x + frame, inner_w = w - 2 * frame, inner_h = h - 2 * frame;
	f32 largest = 0.0f;
	for (i32 b = 0; b < LEVELS_HISTOGRAM_BINS; ++b) {
		largest = summary->bins[b] > largest ? summary->bins[b] : largest;
	}
	for (i32 b = 0; b < LEVELS_HISTOGRAM_BINS && largest > 0.0f; ++b) {
		f32 bh = floorf(sqrtf(summary->bins[b] / largest) * inner_h + 0.5f); // sqrt: a few dominant bins do not flatten the rest
		if (bh > 0.0f) {
			f32 x0 = floorf(inner_x + inner_w * (f32)b / LEVELS_HISTOGRAM_BINS);
			f32 x1 = floorf(inner_x + inner_w * (f32)(b + 1) / LEVELS_HISTOGRAM_BINS);
			levels_strip_rect(x0, y + frame + inner_h - bh, (x1 > x0 ? x1 - x0 : 1.0f), bh, bars);
		}
	}
}

// The gradient 0..1 in a frame: x, w the track, y, h the whole bar including the frame
static void levels_strip_gradient(f32 x, f32 y, f32 w, f32 h, f32 frame, u32 line) {
	levels_strip_rect(x, y, w, h, line);
	i32 slices  = 64;
	f32 inner_w = w - 2 * frame;
	for (i32 s = 0; s < slices; ++s) {
		f32 t  = (f32)s / (f32)(slices - 1);
		f32 x0 = floorf(x + frame + inner_w * (f32)s / (f32)slices);
		f32 x1 = floorf(x + frame + inner_w * (f32)(s + 1) / (f32)slices);
		levels_strip_rect(x0, y + frame, x1 - x0, h - 2 * frame, color_from_floats(t, t, t, 1.0));
	}
}

static void levels_node_strip_button(i32 node_id) {
	ui_nodes_t       *nodes  = ui_nodes_get_nodes();
	ui_node_canvas_t *canvas = ui_nodes_get_canvas(true);
	ui_node_t        *node   = ui_get_node(canvas->nodes, node_id);
	if (node == NULL) {
		return;
	}
	ui_nodes_editor_state_t *state = ui_nodes_editor_state(node);
	levels_strip_t           strip = {.node = node, .canvas = canvas, .range = levels_node_mode(node) == LEVELS_MODE_RANGE};
	f32 L        = UI_LINE_H();
	f32 marker_h = L * LEVELS_STRIP_MARKER_LINES;
	f32 hit_h    = L * LEVELS_STRIP_HIT_LINES;
	f32 extent   = (f32)ui_stops_pin_extent(marker_h); // Histogram, bar and track are inset so the handles at 0 and 1 stay in the node
	f32 track_w  = g_ui->_w - 2 * extent;
	i32 outline, ring;
	ui_stops_pin_rings(marker_h, &outline, &ring);
	f32 frame    = (f32)outline;
	f32 hist_h   = floorf(L * LEVELS_STRIP_HIST_LINES);
	f32 gap_h    = floorf(L * LEVELS_STRIP_GAP_LINES);
	f32 bar_h    = floorf(L * LEVELS_STRIP_BAR_LINES);
	u32 text_col = (u32)g_ui->ops->theme->TEXT_COL;
	u32 bg_col   = (u32)g_ui->ops->theme->WINDOW_BG_COL;
	f32 hist_y   = strip.range ? 0.0f : floorf(L * LEVELS_STRIP_TOP_LINES); // Range: the box starts at the top, its handle row is the one below the bar
	f32 bar_y    = hist_y + hist_h + gap_h;
	levels_summary_t *summary = levels_node_summary_of(node);

	levels_strip_histogram(extent, hist_y, track_w, hist_h, frame, summary, text_col, bg_col);
	if (strip.range) { // The window the mask selects
		f32 lo = levels_strip_get_pos(&strip, 0);
		f32 hi = levels_strip_get_pos(&strip, 2);
		levels_strip_rect(extent + frame + lo * (track_w - 2 * frame), hist_y + frame, (hi - lo) * (track_w - 2 * frame), hist_h - 2 * frame, 0x40ffffff);
	}
	levels_strip_gradient(extent, bar_y, track_w, bar_h, frame, levels_strip_mix(text_col, bg_col, 0.55f));

	i32 hit = 0;
	if (strip.range) { // One row below the bar: window low edge, position, window high edge
		ui_stops_ops_t ops = {.count     = levels_strip_count,
		                      .get_pos   = levels_strip_get_pos,
		                      .set_pos   = levels_strip_set_pos,
		                      .fill      = levels_strip_fill_cb,
		                      .is_locked = levels_strip_is_locked,
		                      .min_count = 3,
		                      .max_count = 3};
		ui_stops_layout_t layout = {.x = g_ui->_x + extent, .y = g_ui->_y + bar_y, .w = track_w, .bar_h = bar_h, .marker_h = marker_h, .hit_h = hit_h};
		hit |= ui_stops_update(&state->stops, &ops, &strip, &layout);
	}
	else {
		levels_strip_t in_strip  = strip;
		levels_strip_t out_strip = strip;
		out_strip.output_row     = true;
		ui_stops_ops_t in_ops    = {.count     = levels_strip_count,
		                            .get_pos   = levels_strip_get_pos,
		                            .set_pos   = levels_strip_set_pos,
		                            .fill      = levels_strip_fill_cb,
		                            .is_locked = levels_strip_is_locked,
		                            .min_count = 3,
		                            .max_count = 3,
		                            .flipped   = true};
		ui_stops_ops_t out_ops   = {.count     = levels_strip_count,
		                            .get_pos   = levels_strip_get_pos,
		                            .set_pos   = levels_strip_set_pos,
		                            .fill      = levels_strip_fill_cb,
		                            .is_locked = levels_strip_is_locked,
		                            .min_count = 2,
		                            .max_count = 2};
		// The input handles on top of the histogram box (their press zone above it), the output handles below the bar. A press in the box does nothing.
		ui_stops_layout_t in_layout  = {.x = g_ui->_x + extent, .y = g_ui->_y + hist_y, .w = track_w, .bar_h = hist_h, .marker_h = marker_h, .hit_h = hit_h};
		ui_stops_layout_t out_layout = {.x = g_ui->_x + extent, .y = g_ui->_y + bar_y, .w = track_w, .bar_h = bar_h, .marker_h = marker_h, .hit_h = hit_h};
		hit |= ui_stops_update(&state->stops, &in_ops, &in_strip, &in_layout);
		hit |= ui_stops_update(&state->stops_out, &out_ops, &out_strip, &out_layout);
	}
	if (hit & UI_STOPS_CONSUMED) {
		nodes->_input_started  = false;
		g_context->select_time = 0; // Two quick presses on handles are not a node double click (2D view)
	}
	g_ui->_y += L * LEVELS_STRIP_HEIGHT;
}

// The sections of the node: General (Mode, Invert, Auto Levels, the strip and Image) and Parameters (the other inputs). No Texture section:
// the node has no baked texture.
static void levels_node_sections(ui_node_t *def) {
	ui_node_section_t *list = calloc(3, sizeof(ui_node_section_t)); // The list ends with the zeroed entry
	list[0]                 = (ui_node_section_t){.title             = _tr("General"),
	                                              .kind              = UI_NODE_ITEM_BUTTON,
	                                              .first             = 0,
	                                              .count             = LEVELS_BUTTON_CLAMP,
	                                              .closed_by_default = false,
	                                              .inputs_first      = 0,
	                                              .inputs_count      = 1};
	// Parameters also holds Intermediary Clamp as a trailing member: drawn below the sliders (hidden with them while the section is closed)
	list[1]                 = (ui_node_section_t){.title = _tr("Parameters"), .kind = UI_NODE_ITEM_INPUT, .first = 1, .count = def->inputs->length - 1, .closed_by_default = false, .trailing_first = LEVELS_BUTTON_CLAMP, .trailing_count = 1};
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
	                        make_image_node_socket(_tr("In Low"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("In High"), "VALUE", value_color, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("In Mid"), "VALUE", value_color, f32_array_create_x(LEVELS_DEFAULT_IN_MID), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Out Low"), "VALUE", value_color, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Out High"), "VALUE", value_color, f32_array_create_x(1.0), 0.0, 1.0, 100),
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
	                        make_image_node_setting(_tr("Intermediary Clamp"), "BOOL", 1, NULL),
	                    },
	                    5),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, levels_node_def);
	any_map_set(parser_material_node_vectors, "LEVELS", levels_node_vector);
	any_map_set(ui_nodes_hidden_rules, "LEVELS", levels_node_item_hidden);
	any_map_set(ui_nodes_custom_buttons, "levels_auto_button", levels_node_auto_button);
	any_map_set(ui_nodes_custom_buttons, "levels_strip_button", levels_node_strip_button);
	levels_node_sections(levels_node_def);
	make_image_node_register(&levels_node_image_type);
}
