#include "../global.h"

// Edge Detect (Filter menu): edges of the Image input. An image node (render/make_image_node.c): the Image input is
// evaluated in image space (grey, 32 bit float so smooth inputs do not band) and the edge image is computed by passes on the GPU (the render target of the
// last pass is the node's result, no readback, IMAGE_NODES.md section 11) into one RGBA64 texture per node, R = Image (the edge mask). The texture is sampled like an Image Texture at tex_coord, repeating (Tile UV places it on the uv).
//
// A one pixel edge (Sobel gradient magnitude) widened by Edge Width (a fraction of Size) with an isotropic blur, see
// edge_detect_node_gpu_bake. Edge Width 0 (below about 0.0007 at Size 1024) is not widened but antialiased: the captured
// grey (one sample per pixel, so a diagonal border is a hard staircase) is smoothed with [1 2 1] / 4 before the Sobel,
// giving a line 1 to 2.5 pixels wide whose slope has no stairs. Gradient mode: ramps from 0 at Tolerance to 1 at the largest gradient of the image, so a
// smooth input (noise, distance fields) still gives visible edges. Boundary mode: 1 where the gradient exceeds
// Tolerance, else 0 (soft sides after the widening). The result is kept in 32 bit float.
// Edge Width and Tolerance hold for the whole image (scalar, evaluated at the image centre when linked): the blur
// width has to be known per bake, and a per pixel link has no neighbours to compare in image space.
// A linked Edge Width is clamped to the slider range 0 to EDGE_DETECT_WIDTH_MAX (a huge value saturates the ridge
// normalisation and gave an all white image; not a number and negative values give 0).

#define EDGE_DETECT_WIDTH_MAX 0.1 // Maximum of the Edge Width slider

// Defaults: Gradient, Edge Width 0, Tolerance 0. The widening scales the blurred edge image by 4 w / 3 so that a one pixel
// wide line still peaks at 1; a smooth input (noise, a distance field) has gradient everywhere, not lines, and any
// Edge Width above the antialiasing threshold multiplies it by that gain (Edge Width 0.005: all white, measured 99 percent
// of a Perlin noise above 0.9). Edge Width 0 gives the gradient magnitude relative to its largest value (a soft slope map on
// smooth input, mean 0.32 on noise) and a clean antialiased line of about 2.7 pixels on a hard edge. A Tolerance above 0 would
// cut a smooth input to black (its raw slopes are far below 0.01). Both stay sliders: the user widens the line when needed.
#define EDGE_DETECT_DEFAULT_WIDTH     0.0
#define EDGE_DETECT_DEFAULT_TOLERANCE 0.0

// Input socket indices
enum {
	EDGE_DETECT_INPUT_IMAGE      = 0,
	EDGE_DETECT_INPUT_EDGE_WIDTH = 1,
	EDGE_DETECT_INPUT_TOLERANCE  = 2,
};

// Node button indices (settings)
enum {
	EDGE_DETECT_BUTTON_SIZE = 0, // Button 1 is the state row
	EDGE_DETECT_BUTTON_MODE = 2,
	EDGE_DETECT_BUTTON_WRAP = 3,
};

typedef struct edge_detect_params {
	i32 boundary; // 0 Gradient, 1 Boundary
	i32 wrap;
	f32 edge_width;
	f32 tolerance;
} edge_detect_params_t;

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 edge_detect_node_size(ui_node_t *node) {
	return make_image_node_size(node, EDGE_DETECT_BUTTON_SIZE);
}

// Captured per pixel: the Image input as grey
static char *edge_detect_node_capture(ui_node_t *node) {
	char *image = parser_material_parse_value_input(node->inputs->buffer[EDGE_DETECT_INPUT_IMAGE], false);
	char *grey  = string_tmp("%s_grey", parser_material_store_var_name(node));
	parser_material_write(parser_material_kong, string_tmp("float %s = clamp(%s, 0.0, 1.0);", grey, image));
	return string_tmp("float4(%s, %s, %s, 1.0)", grey, grey, grey);
}

static void edge_detect_node_params(ui_node_t *node, void *params) {
	edge_detect_params_t *p = params;
	p->boundary             = make_image_node_button(node, EDGE_DETECT_BUTTON_MODE) != 0.0;
	p->wrap                 = make_image_node_button(node, EDGE_DETECT_BUTTON_WRAP) != 0.0;
	f32 width               = make_image_node_input_value(node, EDGE_DETECT_INPUT_EDGE_WIDTH);
	p->edge_width           = !(width > 0.0) ? 0.0 : width > EDGE_DETECT_WIDTH_MAX ? EDGE_DETECT_WIDTH_MAX : width; // Not a number gives 0
	p->tolerance            = make_image_node_input_value(node, EDGE_DETECT_INPUT_TOLERANCE);
}

// The passes (GPU, no readback; make_image_node_gpu_shader and make_image_node_gpu_pass, IMAGE_NODES.md section 11). Every pass is a full screen
// triangle over R32 targets of Size x Size (the last one RGBA64, the result), reading the texels at their centres and wrapping or clamping the
// index by hand (Wrap Edges), so that the result is the same as the former cpu code (a reference copy of it is in the host test):
//   1. Width 0 only: [1 2 1] / 4 in x, then in y, of the captured grey (antialiases the staircase of the capture)
//   2. Sobel gradient magnitude (a step of 1 gives 1)
//   3. the largest magnitude of the image: passes that halve the size taking the maximum of 2 x 2 texels until 1 x 1 (no blend max, no compute)
//   4. normalise: Gradient (v - Tolerance) / (max - Tolerance), Boundary v > Tolerance (the 1 x 1 maximum is a texture input)
//   5. Width > 0 only: three box blurs in x and y (six passes), the last applies the gain and, in Boundary, the smoothstep (RGBA64)
// k = (size, wrap, x, y) of every pass; the texel index of a tap is computed in float (exact for the sizes up to 4096).
#define EDGE_DETECT_TAP(var, x, y)                                                                                                                         \
	"float " var "_x = " x "; float " var "_y = " y "; "                                                                                                   \
	"float " var "_cx = clamp(" var "_x, 0.0, s - 1.0); float " var "_cy = clamp(" var "_y, 0.0, s - 1.0); "                                               \
	var "_x = " var "_cx + (" var "_x - s * floor(" var "_x / s) - " var "_cx) * wrapf; "                                                                  \
	var "_y = " var "_cy + (" var "_y - s * floor(" var "_y / s) - " var "_cy) * wrapf; "                                                                  \
	"float " var " = sample_lod(in0, sampler_linear, float2((" var "_x + 0.5) * inv, (" var "_y + 0.5) * inv), 0.0).r; "

#define EDGE_DETECT_PRE                                                                                                                                    \
	"float s = constants.k.x; float wrapf = constants.k.y; float inv = 1.0 / s; float px = floor(tex_coord.x * s); float py = floor(tex_coord.y * s); "

// k.z = 0.25, or 0.25 * 4 / 3 for the smoothed input
static char *edge_detect_node_sobel_body =
    EDGE_DETECT_PRE EDGE_DETECT_TAP("a", "px - 1.0", "py - 1.0") EDGE_DETECT_TAP("b", "px", "py - 1.0") EDGE_DETECT_TAP("c", "px + 1.0", "py - 1.0")
        EDGE_DETECT_TAP("d", "px - 1.0", "py") EDGE_DETECT_TAP("f", "px + 1.0", "py") EDGE_DETECT_TAP("g", "px - 1.0", "py + 1.0")
            EDGE_DETECT_TAP("h", "px", "py + 1.0") EDGE_DETECT_TAP("i", "px + 1.0", "py + 1.0")
                "float gx = (c + 2.0 * f + i) - (a + 2.0 * d + g); float gy = (g + 2.0 * h + i) - (a + 2.0 * b + c); "
                "output = float4(sqrt(gx * gx + gy * gy) * constants.k.z, 0.0, 0.0, 1.0);";

// (k.z, k.w) = (1, 0) for x, (0, 1) for y
static char *edge_detect_node_binomial_body =
    EDGE_DETECT_PRE EDGE_DETECT_TAP("a", "px - constants.k.z", "py - constants.k.w") EDGE_DETECT_TAP("b", "px", "py")
        EDGE_DETECT_TAP("c", "px + constants.k.z", "py + constants.k.w") "output = float4(0.25 * a + 0.5 * b + 0.25 * c, 0.0, 0.0, 1.0);";

// Largest of 2 x 2 texels of the source: constants.s is the size of a source texel in uv, the taps sit at the centres of the four texels
static char *edge_detect_node_reduce_body =
    "float t = constants.s * 0.5; "
    "float a = sample_lod(in0, sampler_linear, tex_coord + float2(0.0 - t, 0.0 - t), 0.0).r; float b = sample_lod(in0, sampler_linear, tex_coord + float2(t, 0.0 - t), 0.0).r; "
    "float c = sample_lod(in0, sampler_linear, tex_coord + float2(0.0 - t, t), 0.0).r; float d = sample_lod(in0, sampler_linear, tex_coord + float2(t, t), 0.0).r; "
    "output = float4(max(max(a, b), max(c, d)), 0.0, 0.0, 1.0);";

// k = (size, wrap, Tolerance, Boundary). in0 the magnitude, in1 the 1 x 1 maximum. A flat image (maximum 0) gives 0 in Gradient mode, never a division by 0.
// Writes R32 (Width above 0, blurred next) or RGBA64 (the result): the grey in R, G and B.
static char *edge_detect_node_normalise_body =
    EDGE_DETECT_PRE EDGE_DETECT_TAP("v", "px", "py")
        "float m = sample_lod(in1, sampler_linear, float2(0.5, 0.5), 0.0).r; float tol = constants.k.z; float r = 0.0; "
        "if (constants.k.w > 0.5) { if (v > tol) { r = 1.0; } } "
        "else { float ramp = m - tol; if (v > tol && ramp > 0.0001) { r = min((v - tol) / ramp, 1.0); } } "
        "output = float4(r, r, r, 1.0);";

// One box blur of odd width along (k.z, k.w); m = (half width, 1 / width, gain, Boundary). The last pass (RGBA64) scales by the gain (1 / the peak of a one pixel
// wide line), clamps to 1 and, in Boundary, takes the smoothstep from 0.4 to 0.6 (a band of constant width with soft sides).
#define EDGE_DETECT_BOX_LOOP                                                                                                                               \
	EDGE_DETECT_PRE "float sum = 0.0; int hh = int(constants.m.x); "                                                                                       \
	                "for (int j = 0 - hh; j <= hh; j += 1) { float o = float(j); " EDGE_DETECT_TAP("t", "px + constants.k.z * o", "py + constants.k.w * o") "sum += t; } " \
	                "float v = sum * constants.m.y; "

static char *edge_detect_node_box_body = EDGE_DETECT_BOX_LOOP "output = float4(v, 0.0, 0.0, 1.0);";

static char *edge_detect_node_box_last_body =
    EDGE_DETECT_BOX_LOOP "float g = min(v * constants.m.z, 1.0); "
                         "if (constants.m.w > 0.5) { float u = clamp((g - 0.4) / 0.2, 0.0, 1.0); g = u * u * (3.0 - 2.0 * u); } "
                         "output = float4(g, g, g, 1.0);";

// The wide box blur (Edge Width above EDGE_DETECT_WIDE_MIN_BOX): one box of odd width w along an axis computed in two passes instead of w taps. With
// w = a b + c (c < a) the sum of the w texels x - h ... x + h (h = w / 2) is b blocks of a texels plus c single texels:
//   stage 1: A(t) = sum of a texels s(t) ... s(t + a - 1), over t = -h ... size + h - 1 (a texture of size + 2 h along the axis, the index of the texels
//            of s wrapped or clamped like the one pass box, so that A is correct outside the image too)
//   stage 2: out(x) = (A(x - h) + A(x - h + a) + ... (b blocks) + s(x - h + a b) + ... (c texels)) / w
// a, b and c are picked to minimise a + b + c taps (about 2 sqrt(w) + c instead of w). The result is the one pass box up to the float rounding. k:
//   um = (target width, target height, 1 for the y axis, Wrap Edges); ud = (size, extent of A along the axis, offset of A to s, a); ur = (b, c, 1 / w, gain); ub = Boundary
// The index of a tap along the axis of the s texels: clamped, or wrapped when Wrap Edges (wrapf = 1).
#define EDGE_DETECT_WIDE_PRE                                                                                                                               \
	"float n = constants.ud.x; float vert = constants.um.z; float wrapf = constants.um.w; float inv = 1.0 / n; "                                           \
	"float cx = floor(tex_coord.x * constants.um.x); float cy = floor(tex_coord.y * constants.um.y); "                                                      \
	"float pa = cx + vert * (cy - cx); float pq = cy + vert * (cx - cy); float sum = 0.0; "
#define EDGE_DETECT_WIDE_S_TAP(texture, index)                                                                                                             \
	"{ float i = " index "; float ic = clamp(i, 0.0, n - 1.0); i = ic + (i - n * floor(i / n) - ic) * wrapf; "                                             \
	"float tx = i + vert * (pq - i); float ty = pq + vert * (i - pq); "                                                                                    \
	"sum += sample_lod(" texture ", sampler_linear, float2((tx + 0.5) * inv, (ty + 0.5) * inv), 0.0).r; } "

static char *edge_detect_node_wide_a_body =
    EDGE_DETECT_WIDE_PRE "int na = int(constants.ud.w); "
                         "for (int j = 0; j < na; j += 1) " EDGE_DETECT_WIDE_S_TAP("in0", "pa + constants.ud.z + float(j)")
                         "output = float4(sum, 0.0, 0.0, 1.0);";

#define EDGE_DETECT_WIDE_BOX                                                                                                                               \
	EDGE_DETECT_WIDE_PRE "float ae = constants.ud.y; float aw = ae + vert * (n - ae); float ah = n + vert * (ae - n); "                                    \
	"int nb = int(constants.ur.x); int nc = int(constants.ur.y); float blk = constants.ud.w; "                                                             \
	"for (int m = 0; m < nb; m += 1) { float i = pa + float(m) * blk; float tx = i + vert * (pq - i); float ty = pq + vert * (i - pq); "                  \
	"sum += sample_lod(in0, sampler_linear, float2((tx + 0.5) / aw, (ty + 0.5) / ah), 0.0).r; } "                                                          \
	"for (int j = 0; j < nc; j += 1) " EDGE_DETECT_WIDE_S_TAP("in1", "pa + blk * float(nb) + constants.ud.z + float(j)") "float v = sum * constants.ur.z; "

static char *edge_detect_node_wide_box_body = EDGE_DETECT_WIDE_BOX "output = float4(v, 0.0, 0.0, 1.0);";

// The last box of the last axis: the gain, the clamp and in Boundary the smoothstep, like edge_detect_node_box_last_body
static char *edge_detect_node_wide_last_body =
    EDGE_DETECT_WIDE_BOX "float g = min(v * constants.ur.w, 1.0); "
                         "if (constants.ub > 0.5) { float u = clamp((g - 0.4) / 0.2, 0.0, 1.0); g = u * u * (3.0 - 2.0 * u); } "
                         "output = float4(g, g, g, 1.0);";

// Box widths from this many texels are computed in two passes (the direct sum is faster for small widths)
#define EDGE_DETECT_WIDE_MIN_BOX 24

// Splits w = a b + c (c < a) for the fewest taps a + b + c
static void edge_detect_node_wide_split(i32 w, i32 *a, i32 *b, i32 *c) {
	i32 best = 0x7fffffff;
	for (i32 t = 2; t <= w; ++t) {
		i32 cost = t + w / t + w % t;
		if (cost < best) {
			best = cost;
			*a   = t;
			*b   = w / t;
			*c   = w % t;
		}
	}
}

// One box (two passes) along an axis: s -> acc (A, size + 2 h along the axis) -> dst, size x size
static void edge_detect_node_wide_box(shader_context_t *stage1, shader_context_t *stage2, gpu_texture_t *s, gpu_texture_t *acc, gpu_texture_t *dst, i32 size, bool vertical, i32 w,
                                      f32 wrap, f32 gain, f32 boundary) {
	i32 h = w / 2;
	i32 a, b, c;
	edge_detect_node_wide_split(w, &a, &b, &c);
	f32 ext = (f32)(size + 2 * h);
	f32 vert = vertical ? 1.0f : 0.0f;
	f32 u[13] = {vertical ? (f32)size : ext, vertical ? ext : (f32)size, vert, wrap, (f32)size, ext, (f32)(-h), (f32)a, (f32)b, (f32)c, 1.0f / (f32)w, gain, boundary};
	gpu_texture_t *in[2];
	in[0] = s;
	make_image_node_gpu_pass(stage1, &acc, 1, in, 1, u);
	u[0] = (f32)size;
	u[1] = (f32)size;
	in[0] = acc;
	in[1] = s;
	make_image_node_gpu_pass(stage2, &dst, 1, in, 2, u);
}

// The edge image from the captured grey (captures[0], R32F, Size x Size): returns the result render target (RGBA64, the mask in R, G and B), NULL when a
// target could not be created. The capture is used as scratch (the shared code deletes it), so a bake holds three planes: the capture, one more R32 and the result.
// Sobel gradient magnitude (a step of 1 gives 1), then the Edge Width: the edge is blurred (three box passes in x and y, a Gaussian within a few percent,
// isotropic) and scaled back so a straight edge peaks at 1. Every direction is treated alike, so a diagonal border has no gaps or stairs. Sigma is 0.85 times
// the Edge Width in pixels, so the ridge is about twice Edge Width wide at half height. Without widening (sigma < 0.6) the input is smoothed with [1 2 1] / 4
// (x then y) before the Sobel and the magnitude is scaled by 4 / 3, so a step of 1 still gives 1 but its slope spreads over two pixels: a diagonal border
// does not show the texel staircase of the capture (which has no coverage information) in the surface slope Bump reads.
static gpu_texture_t *edge_detect_node_gpu_bake(ui_node_t *node, i32 size, void *params, gpu_texture_t **captures, i32 count) {
	edge_detect_params_t p = *(edge_detect_params_t *)params;
	f32 width_px           = p.edge_width * (f32)size;
	width_px               = width_px > (f32)size ? (f32)size : width_px; // The box width is capped to the size below anyway
	f32  tol               = p.tolerance < 0.0 ? 0.0 : p.tolerance > 1.0 ? 1.0 : p.tolerance;
	f32  sigma             = 0.85f * width_px;
	bool smooth            = sigma < 0.6f; // No widening follows: antialias the input instead
	f32  wrap              = p.wrap ? 1.0f : 0.0f;
	f32  boundary          = p.boundary ? 1.0f : 0.0f;

	gpu_texture_t *a      = captures[0]; // The grey, then scratch
	gpu_texture_t *b      = gpu_create_render_target(size, size, GPU_TEXTURE_FORMAT_R32);
	gpu_texture_t *result = gpu_create_render_target(size, size, GPU_TEXTURE_FORMAT_RGBA64);
	gpu_texture_t *chain[16]; // The reduction: size / 2, size / 4, ... 1
	i32            chain_count = 0;
	for (i32 s = size / 2; s >= 1 && chain_count < 16; s /= 2) {
		chain[chain_count] = gpu_create_render_target(s, s, GPU_TEXTURE_FORMAT_R32);
		chain_count++;
	}
	bool ok = b != NULL && result != NULL;
	for (i32 i = 0; i < chain_count; ++i) {
		ok = ok && chain[i] != NULL;
	}
	if (!ok) {
		for (i32 i = 0; i < chain_count; ++i) {
			if (chain[i] != NULL) {
				gpu_delete_texture(chain[i]);
			}
		}
		if (b != NULL) {
			gpu_delete_texture(b);
		}
		if (result != NULL) {
			gpu_delete_texture(result);
		}
		return NULL;
	}

	char *in0[]    = {"in0"};
	char *in01[]   = {"in0", "in1"};
	char *k4[]     = {"float4 k"};
	char *k4m4[]   = {"float4 k", "float4 m"};
	char *s1[]     = {"float s"};
	char *r32[]    = {"R32"};
	char *rgba64[] = {"RGBA64"};
	shader_context_t *sobel  = make_image_node_gpu_shader(edge_detect_node_sobel_body, in0, 1, k4, 1, r32, 1);
	shader_context_t *reduce = make_image_node_gpu_shader(edge_detect_node_reduce_body, in0, 1, s1, 1, r32, 1);
	shader_context_t *norm   = make_image_node_gpu_shader(edge_detect_node_normalise_body, in01, 2, k4, 1, smooth ? rgba64 : r32, 1);
	shader_context_t *bin    = smooth ? make_image_node_gpu_shader(edge_detect_node_binomial_body, in0, 1, k4, 1, r32, 1) : NULL;
	// The width of the three boxes: odd, sigma^2 = (w^2 - 1) / 4 for three of them
	i32 w = 2 * (i32)((sqrtf(4.0f * sigma * sigma + 1.0f) - 1.0f) * 0.5f + 0.5f) + 1;
	w     = w > size - 1 ? size - 1 : w;
	i32 wide_min = EDGE_DETECT_WIDE_MIN_BOX; /*TEMP*/ if (getenv("EW_MIN") != NULL) { wide_min = atoi(getenv("EW_MIN")); } /*TEMPEND*/
	bool wide = !smooth && w >= wide_min;
	char *um_ud[]       = {"float4 um", "float4 ud"};
	char *um_ud_ur[]    = {"float4 um", "float4 ud", "float4 ur"};
	char *um_ud_ur_ub[] = {"float4 um", "float4 ud", "float4 ur", "float ub"};
	shader_context_t *box    = smooth || wide ? NULL : make_image_node_gpu_shader(edge_detect_node_box_body, in0, 1, k4m4, 2, r32, 1);
	shader_context_t *last   = smooth || wide ? NULL : make_image_node_gpu_shader(edge_detect_node_box_last_body, in0, 1, k4m4, 2, rgba64, 1);
	shader_context_t *wide_a = wide ? make_image_node_gpu_shader(edge_detect_node_wide_a_body, in0, 1, um_ud, 2, r32, 1) : NULL;
	shader_context_t *wide_b = wide ? make_image_node_gpu_shader(edge_detect_node_wide_box_body, in01, 2, um_ud_ur, 3, r32, 1) : NULL;
	shader_context_t *wide_l = wide ? make_image_node_gpu_shader(edge_detect_node_wide_last_body, in01, 2, um_ud_ur_ub, 4, rgba64, 1) : NULL;

	f32            u[4] = {(f32)size, wrap, 0.25f, 0.0f};
	gpu_texture_t *in[2];
	gpu_texture_t *dst;
	if (smooth) {
		u[2]  = 1.0f; // [1 2 1] / 4 in x: a -> b, then in y: b -> a
		u[3]  = 0.0f;
		in[0] = a;
		dst   = b;
		make_image_node_gpu_pass(bin, &dst, 1, in, 1, u);
		u[2]  = 0.0f;
		u[3]  = 1.0f;
		in[0] = b;
		dst   = a;
		make_image_node_gpu_pass(bin, &dst, 1, in, 1, u);
		u[2] = 0.25f * (4.0f / 3.0f);
		u[3] = 0.0f;
	}
	// Sobel a -> b: the magnitude is in b
	in[0] = a;
	dst   = b;
	make_image_node_gpu_pass(sobel, &dst, 1, in, 1, u);
	gpu_texture_t *magnitude = b;

	// The largest magnitude: 2 x 2 per pass, to 1 x 1
	gpu_texture_t *src = magnitude;
	for (i32 i = 0; i < chain_count; ++i) {
		f32 su = 1.0f / (f32)(size >> i); // Size of a texel of the source
		in[0]  = src;
		make_image_node_gpu_pass(reduce, &chain[i], 1, in, 1, &su);
		src = chain[i];
	}

	// Normalise: smooth: magnitude -> result; else magnitude -> a
	f32 nu[4] = {(f32)size, wrap, tol, boundary};
	in[0]     = magnitude;
	in[1]     = src; // 1 x 1
	dst       = smooth ? result : a;
	make_image_node_gpu_pass(norm, &dst, 1, in, 2, nu);

	if (wide) {
		f32 fw   = (f32)w;
		f32 gain = (4.0f * fw * fw * fw) / (3.0f * fw * fw + 1.0f);
		// x, x, x then y, y, y (the boxes of the two axes commute, also with clamped indices: every box clamps its own input along its own axis)
		gpu_texture_t *acc = gpu_create_render_target(size + 2 * (w / 2), size, GPU_TEXTURE_FORMAT_R32);
		gpu_texture_t *accy = NULL;
		bool           done = acc != NULL;
		if (done) {
			edge_detect_node_wide_box(wide_a, wide_b, a, acc, b, size, false, w, wrap, gain, boundary);
			edge_detect_node_wide_box(wide_a, wide_b, b, acc, a, size, false, w, wrap, gain, boundary);
			edge_detect_node_wide_box(wide_a, wide_b, a, acc, b, size, false, w, wrap, gain, boundary);
			gpu_delete_texture(acc);
			accy = gpu_create_render_target(size, size + 2 * (w / 2), GPU_TEXTURE_FORMAT_R32);
			done = accy != NULL;
		}
		if (done) {
			edge_detect_node_wide_box(wide_a, wide_b, b, accy, a, size, true, w, wrap, gain, boundary);
			edge_detect_node_wide_box(wide_a, wide_b, a, accy, b, size, true, w, wrap, gain, boundary);
			edge_detect_node_wide_box(wide_a, wide_l, b, accy, result, size, true, w, wrap, gain, boundary);
			gpu_delete_texture(accy);
		}
		else {
			gpu_delete_texture(result);
			result = NULL;
		}
	}
	else if (!smooth) {
		f32 fw        = (f32)w;
		f32 gain      = (4.0f * fw * fw * fw) / (3.0f * fw * fw + 1.0f); // 1 / peak of a one pixel wide line
		f32 bu[8]     = {(f32)size, wrap, 1.0f, 0.0f, (f32)(w / 2), 1.0f / fw, gain, boundary};
		gpu_texture_t *from = a, *to = b;
		for (i32 k = 0; k < 6; ++k) {
			bool horizontal = k % 2 == 0;
			bu[2]           = horizontal ? 1.0f : 0.0f;
			bu[3]           = horizontal ? 0.0f : 1.0f;
			in[0]           = from;
			dst             = k == 5 ? result : to;
			make_image_node_gpu_pass(k == 5 ? last : box, &dst, 1, in, 1, bu);
			gpu_texture_t *t = from;
			from             = to;
			to               = t;
		}
	}

	make_image_node_gpu_shader_free(sobel);
	make_image_node_gpu_shader_free(reduce);
	make_image_node_gpu_shader_free(norm);
	if (bin != NULL) {
		make_image_node_gpu_shader_free(bin);
	}
	if (box != NULL) {
		make_image_node_gpu_shader_free(box);
		make_image_node_gpu_shader_free(last);
	}
	if (wide_a != NULL) {
		make_image_node_gpu_shader_free(wide_a);
		make_image_node_gpu_shader_free(wide_b);
		make_image_node_gpu_shader_free(wide_l);
	}
	for (i32 i = 0; i < chain_count; ++i) {
		gpu_delete_texture(chain[i]);
	}
	gpu_delete_texture(b); // a is the capture: deleted by the shared code
	return result;
}

static image_node_type_t edge_detect_node_image_type = {
    .type          = "TEX_EDGE_DETECT",
    .result_format       = GPU_TEXTURE_FORMAT_RGBA64, // A mask in 0..1 with ramps no wider than the blur: half floats keep at least a pixel per step
    .result_by_reference = true,                      // The render target of the last pass is the result: no readback, no cpu process, no upload
    .params_size         = sizeof(edge_detect_params_t),
    .size                = edge_detect_node_size,
    .capture             = edge_detect_node_capture,
    .capture_format      = IMAGE_CAPTURE_R32F, // The grey in one float per pixel, kept on the GPU as the input of the passes
    .smooth_sample       = true,
    .params              = edge_detect_node_params,
    .gpu_bake            = edge_detect_node_gpu_bake,
};

char *edge_detect_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

char *edge_detect_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return parser_material_to_vec3(edge_detect_node_value(node, socket));
}

void edge_detect_node_init() {

	ui_node_t *edge_detect_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Edge Detect"),
	                .type   = "TEX_EDGE_DETECT",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Edge Width"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(EDGE_DETECT_DEFAULT_WIDTH), 0.0, EDGE_DETECT_WIDTH_MAX, 1000),
	                        make_image_node_socket(_tr("Tolerance"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(EDGE_DETECT_DEFAULT_TOLERANCE), 0.0, 1.0, 100),
	                    },
	                    3),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Size"), "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS),
	                        make_image_node_state_button(),
	                        make_image_node_setting(_tr("Mode"), "ENUM", 0, "Gradient\nBoundary"),
	                        make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                    },
	                    4),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, edge_detect_node_def);
	any_map_set(parser_material_node_values, "TEX_EDGE_DETECT", edge_detect_node_value);
	any_map_set(parser_material_node_vectors, "TEX_EDGE_DETECT", edge_detect_node_vector);
	make_image_node_sections(edge_detect_node_def, NULL, 0, 1); // General: the settings and Image
	make_image_node_register(&edge_detect_node_image_type);
}
