#include "../global.h"

// Normal to Height (Filter menu): a tangent space normal map back to a height image (integration). The inverse of Height to Normal (a pass-through node,
// height_to_normal_node.c): N2H(H2N(h)) = h up to the constant. The Bump node stays as it is.
// An image node (render/make_image_node.c): the Normal input (the source) is evaluated in image space (32 bit float) and the height is computed on the cpu
// with FFTs into one RGBA texture per node (R = G = B = height, A = 1), sampled like an Image Texture at tex_coord, repeating (Tile UV places it on the
// uv). It is a baked node because the integral is global (every texel depends on the whole image): it has Size and the state row.
//
// The Normal input is the SOURCE (default flat (0.5, 0.5, 1) gives a flat height, 0.5 everywhere with Normalize off; with Normalize on a constant height is
// 0.5 as well). The normal is decoded (2 c - 1), renormalised, nz clamped to at least 0.05 (slopes up to 20, 87 degrees; no blow up for a normal that lies
// down), and turned into the slopes the forward direction produces: sx = -nx / nz, sy = ny / nz (OpenGL, green up, rows down; the same convention as Height
// to Normal, so a normal made by Height to Normal returns its height).
// The height is the least squares solution of  S(h) * c = (sx, sy)  with S the Sobel operator of Height to Normal and c = Strength / 8 *
// Size / 1024, solved with FFTs in the frequency domain (Frankot Chellappa, with the exact frequency response of the Sobel taps instead of
// i * k):
//   H = (conj(Dx) Sx + conj(Dy) Sy) / (|Dx|^2 + |Dy|^2 + L (sin^2(wx / 2) + sin^2(wy / 2))^2),  Dx = i sin(wx) (1 + cos(wy)), Dy = i sin(wy) (1 + cos(wx))
// (Sobel / 4 as a response, so h = ifft(H) / (4 c)). The frequencies where both responses vanish (the DC term and the Nyquist lines wx = pi or
// wy = pi: only a checker pattern of the height has no Sobel) are set to 0. L (NORMAL_TO_HEIGHT_SMOOTHING, 1e-2) is a smoothness (Laplacian)
// penalty of the least squares: it is negligible for the low and middle frequencies (weight ~ w^4 against w^2) and damps the high ones
// where the Sobel responses are almost lost, which would turn the noise of an 8 bit or half float normal into large waves (measured: a constant damping
// instead ate the low frequencies of a smooth bump at 1e-4, and at 1e-8 the half float normals of the old baked Height to Normal gave wave errors of 0.2).
// Using the Sobel response of Height to Normal makes the conversion the inverse of Height to Normal for any height: N2H(H2N(h)) = h up to the constant
// (the DC term is not recoverable).
// The two slope planes are packed in one complex plane (p + i q, one forward and one inverse 2D FFT; the two spectra are separated by the
// symmetry of the transform), double precision twiddle factors, float data, a radix 2 FFT of our own, rows and column groups in parallel.
// Wrap Edges on: the plane is taken as it is (periodic, the seams must match). Off: the slopes are mirrored to a 2 x 2 times larger plane (even
// extension of the height: sx changes sign in the mirror in x, sy in y), the result is cropped. That is exactly the clamp border of the
// Sobel pass of Height to Normal, so the round trip holds at the borders too. FFT sizes: Size (wrap) or 2 * Size, 512 to 8192.
// Memory of the transform plane (8 bytes per complex float): 128 MB at 4096 with Wrap Edges on, 512 MB with it off, on top of the 256 MB capture and
// 256 MB result planes of the shared pass; it is a mapped plane and goes back to the OS at once.
// The integral has mean 0. Normalize on (default): min-max normalised to 0..1 (full range; a constant result is 0.5), Strength is
// not used and hidden. Normalize off: the height is 0.5 + the integral / (Strength-scale), clamped to 0..1, where Strength is the Strength
// that Height to Normal used to make the normal (the node's Strength, default 4, "the Strength of the node that made this normal"):
// the same Strength and Size give the height back. Strength 0 gives a flat 0.5. Invert flips the height (1 - h).
// The result is RGBA half float (heights resolve about 0.0005 near 1): R = G = B = height, A = 1.
// The capture is one RGBA float: RGB = the incoming normal (encoded 0..1). A normal made by the pass-through Height to Normal reaches this node through
// its capture like any other graph (the shared pass evaluates the input in image space).

// Input socket indices
enum {
	NORMAL_TO_HEIGHT_INPUT_NORMAL   = 0,
	NORMAL_TO_HEIGHT_INPUT_STRENGTH = 1,
};

// Node button indices (settings)
enum {
	NORMAL_TO_HEIGHT_BUTTON_SIZE      = 0, // Button 1 is the state row
	NORMAL_TO_HEIGHT_BUTTON_NORMALIZE = 2,
	NORMAL_TO_HEIGHT_BUTTON_INVERT    = 3,
	NORMAL_TO_HEIGHT_BUTTON_WRAP      = 4,
};

#define NORMAL_TO_HEIGHT_MIN_NZ 0.05 // The smallest z of a decoded normal (slope 20)
#define NORMAL_TO_HEIGHT_SMOOTHING 1e-2 // Weight of the smoothness term of the integration, see the header

typedef struct normal_to_height_params {
	i32 normalize;
	i32 invert;
	i32 wrap;
	f32 strength;
} normal_to_height_params_t;

// Which rows of the node are not shown: a normalised result does not use Strength. A linked socket is never hidden by the caller (ui_nodes_on_item_hidden).
static bool normal_to_height_node_hidden(i32 normalize, ui_node_item_t kind, i32 index) {
	return kind == UI_NODE_ITEM_INPUT && index == NORMAL_TO_HEIGHT_INPUT_STRENGTH && normalize != 0;
}

static bool normal_to_height_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	return normal_to_height_node_hidden(make_image_node_button(node, NORMAL_TO_HEIGHT_BUTTON_NORMALIZE) != 0.0, kind, index);
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 normal_to_height_node_size(ui_node_t *node) {
	return make_image_node_size(node, NORMAL_TO_HEIGHT_BUTTON_SIZE);
}

// Captured per pixel: the Normal input in RGB
static char *normal_to_height_node_capture(ui_node_t *node) {
	char *normal = parser_material_parse_vector_input(node->inputs->buffer[NORMAL_TO_HEIGHT_INPUT_NORMAL]);
	return string_tmp("float4(%s, 1.0)", normal);
}

static void normal_to_height_node_params(ui_node_t *node, void *params) {
	normal_to_height_params_t *p = params;
	p->normalize                 = make_image_node_button(node, NORMAL_TO_HEIGHT_BUTTON_NORMALIZE) != 0.0;
	p->invert                    = make_image_node_button(node, NORMAL_TO_HEIGHT_BUTTON_INVERT) != 0.0;
	p->wrap                      = make_image_node_button(node, NORMAL_TO_HEIGHT_BUTTON_WRAP) != 0.0;
	p->strength                  = make_image_node_input_value(node, NORMAL_TO_HEIGHT_INPUT_STRENGTH);
}

// In place radix 2 FFT of the interleaved complex line a (n complex floats, n a power of two). tw holds cos and sin of 2 pi j / n
// for j < n / 2 (computed in double); dir 1 is the forward transform (e^-i), -1 the inverse (unnormalised)
static void normal_to_height_node_fft_line(f32 *a, i32 n, const f32 *tw, i32 dir) {
	for (i32 i = 1, j = 0; i < n; ++i) {
		i32 bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			f32 tr = a[2 * i], ti = a[2 * i + 1];
			a[2 * i] = a[2 * j];
			a[2 * i + 1] = a[2 * j + 1];
			a[2 * j] = tr;
			a[2 * j + 1] = ti;
		}
	}
	for (i32 len = 2; len <= n; len <<= 1) {
		i32 half = len >> 1, step = n / len;
		for (i32 i = 0; i < n; i += len) {
			for (i32 k = 0; k < half; ++k) {
				f32 wr = tw[2 * k * step], wi = -(f32)dir * tw[2 * k * step + 1];
				f32 *u = a + 2 * (i + k), *v = a + 2 * (i + k + half);
				f32 xr = v[0] * wr - v[1] * wi, xi = v[0] * wi + v[1] * wr;
				v[0] = u[0] - xr;
				v[1] = u[1] - xi;
				u[0] += xr;
				u[1] += xi;
			}
		}
	}
}

typedef struct normal_to_height_fft_ctx {
	f32       *data; // m x m complex floats, row major
	i32        m;
	const f32 *tw;
	i32        dir;
} normal_to_height_fft_ctx_t;

static void normal_to_height_node_fft_rows(void *param, i32 begin, i32 end) {
	normal_to_height_fft_ctx_t *c = param;
	for (i32 y = begin; y < end; ++y) {
		normal_to_height_node_fft_line(c->data + (size_t)y * c->m * 2, c->m, c->tw, c->dir);
	}
}

// Column groups of 8 (one cache line of complex floats) are copied to a scratch buffer, transformed and copied back
static void normal_to_height_node_fft_columns(void *param, i32 begin, i32 end) {
	normal_to_height_fft_ctx_t *c = param;
	i32  m       = c->m;
	f32 *scratch = malloc((size_t)8 * m * 2 * sizeof(f32));
	if (scratch == NULL) {
		return;
	}
	for (i32 g = begin; g < end; ++g) {
		for (i32 y = 0; y < m; ++y) {
			for (i32 k = 0; k < 8; ++k) {
				f32 *s = c->data + ((size_t)y * m + g * 8 + k) * 2;
				scratch[((size_t)k * m + y) * 2]     = s[0];
				scratch[((size_t)k * m + y) * 2 + 1] = s[1];
			}
		}
		for (i32 k = 0; k < 8; ++k) {
			normal_to_height_node_fft_line(scratch + (size_t)k * m * 2, m, c->tw, c->dir);
		}
		for (i32 y = 0; y < m; ++y) {
			for (i32 k = 0; k < 8; ++k) {
				f32 *s = c->data + ((size_t)y * m + g * 8 + k) * 2;
				s[0] = scratch[((size_t)k * m + y) * 2];
				s[1] = scratch[((size_t)k * m + y) * 2 + 1];
			}
		}
	}
	free(scratch);
}

// 2D FFT of the m x m complex plane in place, m a power of two of at least 8
static void normal_to_height_node_fft2d(f32 *data, i32 m, const f32 *tw, i32 dir) {
	normal_to_height_fft_ctx_t c = {.data = data, .m = m, .tw = tw, .dir = dir};
	make_image_node_parallel(m, normal_to_height_node_fft_rows, &c);
	make_image_node_parallel(m / 8, normal_to_height_node_fft_columns, &c);
}

typedef struct normal_to_height_n2h_ctx {
	const f32 *pixels; // size x size RGBA, the normal encoded in RGB
	f32       *plane;  // m x m complex
	i32        size;
	i32        m;
	bool       mirror;
	const f32 *sn; // sin and cos of 2 pi k / m
	const f32 *cs;
} normal_to_height_n2h_ctx_t;

// Rows [begin, end) of the image: the slopes sx + i sy of the decoded normals into the plane (and their mirror images)
static void normal_to_height_node_n2h_fill(void *param, i32 begin, i32 end) {
	normal_to_height_n2h_ctx_t *c = param;
	i32 size = c->size, m = c->m;
	for (i32 y = begin; y < end; ++y) {
		for (i32 x = 0; x < size; ++x) {
			const f32 *in = c->pixels + ((size_t)y * size + x) * 4;
			f32 nx = in[0] * 2.0f - 1.0f, ny = in[1] * 2.0f - 1.0f, nz = in[2] * 2.0f - 1.0f;
			f32 l = sqrtf(nx * nx + ny * ny + nz * nz);
			f32 sx = 0.0f, sy = 0.0f;
			if (l > 1e-6f && isfinite(l)) { // A zero or NaN normal is flat
				nx /= l;
				ny /= l;
				nz /= l;
				nz = nz < (f32)NORMAL_TO_HEIGHT_MIN_NZ ? (f32)NORMAL_TO_HEIGHT_MIN_NZ : nz;
				sx = -nx / nz;
				sy = ny / nz;
			}
			f32 *p = c->plane + ((size_t)y * m + x) * 2;
			p[0] = sx;
			p[1] = sy;
			if (c->mirror) {
				i32 x2 = m - 1 - x, y2 = m - 1 - y;
				f32 *a = c->plane + ((size_t)y * m + x2) * 2;
				f32 *b = c->plane + ((size_t)y2 * m + x) * 2;
				f32 *d = c->plane + ((size_t)y2 * m + x2) * 2;
				a[0] = -sx; a[1] = sy;
				b[0] = sx;  b[1] = -sy;
				d[0] = -sx; d[1] = -sy;
			}
		}
	}
}

// The spectrum of the slopes to the spectrum of the height, for the rows ky = r and m - r of every r in [begin, end) (r in 0 .. m / 2).
// Z = P + i Q holds the spectra of both real slope planes; P(k) = (Z(k) + conj Z(-k)) / 2, Q(k) = (Z(k) - conj Z(-k)) / 2i. The height spectrum is
// Hermitian: H(-k) = conj H(k), both are written, so every pair is visited once.
static void normal_to_height_node_n2h_solve(void *param, i32 begin, i32 end) {
	normal_to_height_n2h_ctx_t *c = param;
	i32 m = c->m;
	for (i32 r = begin; r < end; ++r) {
		i32 jy = (m - r) % m;
		for (i32 kx = 0; kx < m; ++kx) {
			i32 jx = (m - kx) % m;
			f32 *a = c->plane + ((size_t)r * m + kx) * 2;
			f32 *b = c->plane + ((size_t)jy * m + jx) * 2;
			if (a == b) { // Its own partner (frequency 0 or Nyquist): the Sobel has no response there
				a[0] = a[1] = 0.0f;
				continue;
			}
			if (r == jy && kx > jx) { // The pair of this row, done at the smaller kx
				continue;
			}
			f32 zr = a[0], zi = a[1], wr = b[0], wi = -b[1]; // Z(k), conj Z(-k)
			f32 pr = (zr + wr) * 0.5f, pi_ = (zi + wi) * 0.5f;
			f32 qr = (zi - wi) * 0.5f, qi = -(zr - wr) * 0.5f; // (Z - conj Z(-k)) / 2i
			f32 dx = c->sn[kx] * (1.0f + c->cs[r]);            // Dx = i dx
			f32 dy = c->sn[r] * (1.0f + c->cs[kx]);            // Dy = i dy
			f32 den = dx * dx + dy * dy;
			f32 hr = 0.0f, hi = 0.0f;
			if (den > 1e-12f) {
				// conj(Dx) P + conj(Dy) Q = -i (dx P + dy Q)
				f32 sr = dx * pr + dy * qr, si = dx * pi_ + dy * qi;
				f32 lap = 1.0f - 0.5f * (c->cs[kx] + c->cs[r]); // sin^2(wx / 2) + sin^2(wy / 2)
				f32 g   = 1.0f / (den + (f32)NORMAL_TO_HEIGHT_SMOOTHING * lap * lap);
				hr     = si * g;
				hi     = -sr * g;
			}
			a[0] = hr;
			a[1] = hi;
			b[0] = hr;
			b[1] = -hi;
		}
	}
}

// The integration: see the header. bytes is the capture (float RGBA, the normal in RGB), result the size x size RGBA plane
static i32 normal_to_height_node_integrate(u8 *bytes, i32 size, normal_to_height_params_t *p, f32 *result) {
	f32 strength = p->strength;
	strength     = !(strength > 0.0f) ? 0.0f : strength > 1000.0f ? 1000.0f : strength; // Also keeps NaN out
	i32    m     = p->wrap ? size : size * 2;
	size_t plane_bytes = (size_t)m * m * 2 * sizeof(f32);
	f32   *plane = make_image_node_plane_alloc(plane_bytes);
	f32   *tw    = malloc((size_t)m * sizeof(f32)); // m / 2 pairs
	f32   *sn    = malloc((size_t)m * sizeof(f32) * 2);
	if (plane == NULL || tw == NULL || sn == NULL) {
		make_image_node_plane_free(plane, plane_bytes);
		free(tw);
		free(sn);
		return -1;
	}
	f32 *cs = sn + m;
	for (i32 j = 0; j < m; ++j) {
		double a = 6.283185307179586476925286766559 * (double)j / (double)m;
		sn[j]    = (f32)sin(a);
		cs[j]    = (f32)cos(a);
		if (j < m / 2) {
			tw[2 * j]     = cs[j];
			tw[2 * j + 1] = sn[j];
		}
	}
	normal_to_height_n2h_ctx_t c = {.pixels = (const f32 *)bytes, .plane = plane, .size = size, .m = m, .mirror = !p->wrap, .sn = sn, .cs = cs};
	make_image_node_parallel(size, normal_to_height_node_n2h_fill, &c);
	normal_to_height_node_fft2d(plane, m, tw, 1);
	make_image_node_parallel(m / 2 + 1, normal_to_height_node_n2h_solve, &c);
	normal_to_height_node_fft2d(plane, m, tw, -1);
	// Height units: 1 / (m * m) of the inverse transform, 1 / (4 c) of the Sobel sum, c = Strength / 8 * Size / 1024
	f32  inv_scale = strength > 0.0f ? 1.0f / ((f32)m * (f32)m) / (4.0f * (strength / 8.0f * (f32)size / 1024.0f)) : 0.0f;
	f32  lo = 1e30f, hi = -1e30f;
	bool normalize = p->normalize != 0;
	for (i32 y = 0; y < size; ++y) {
		for (i32 x = 0; x < size; ++x) {
			f32 v = plane[((size_t)y * m + x) * 2] * (normalize ? 1.0f : inv_scale);
			plane[((size_t)y * m + x) * 2] = v;
			lo = v < lo ? v : lo;
			hi = v > hi ? v : hi;
		}
	}
	f32 range = hi - lo;
	for (i32 y = 0; y < size; ++y) {
		for (i32 x = 0; x < size; ++x) {
			f32 v = plane[((size_t)y * m + x) * 2];
			if (normalize) {
				v = range > 1e-12f ? (v - lo) / range : 0.5f; // A constant height is 0.5
				v = p->invert ? 1.0f - v : v;
			}
			else {
				v = 0.5f + (p->invert ? -v : v);
				v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
			}
			f32 *o = result + ((size_t)y * size + x) * 4;
			o[0] = o[1] = o[2] = v;
			o[3] = 1.0f;
		}
	}
	make_image_node_plane_free(plane, plane_bytes);
	free(tw);
	free(sn);
	return 0;
}

// Result of the captured normal; returns 0, or -1 when memory runs out
static i32 normal_to_height_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	normal_to_height_params_t p = *(normal_to_height_params_t *)params;
	return normal_to_height_node_integrate(bytes, size, &p, result);
}

static image_node_type_t normal_to_height_node_image_type = {
    .type          = "TEX_NORMAL_TO_HEIGHT",
    .result_format = GPU_TEXTURE_FORMAT_RGBA64, // Heights resolve about 0.0005 near 1, no second plane
    .params_size   = sizeof(normal_to_height_params_t),
    .size          = normal_to_height_node_size,
    .capture       = normal_to_height_node_capture,
    .capture_format = IMAGE_CAPTURE_RGBA32F,
    .smooth_sample = true,
    .params        = normal_to_height_node_params,
    .process       = normal_to_height_node_process,
};

char *normal_to_height_node_vector(ui_node_t *node, ui_node_socket_t *socket) { // R = G = B = height
	return string_tmp("%s.rgb", make_image_node_sample(node));
}

char *normal_to_height_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

void normal_to_height_node_init() {

	ui_node_t *normal_to_height_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Normal to Height"),
	                .type   = "TEX_NORMAL_TO_HEIGHT",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Normal"), "VECTOR", NODES_MATERIAL_VECTOR_COLOR, f32_array_create_xyz(0.5, 0.5, 1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Strength"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(4.0), 0.0, 100.0, 100),
	                    },
	                    2),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Height"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Size"), "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS),
	                        make_image_node_state_button(),
	                        make_image_node_setting(_tr("Normalize"), "BOOL", 1, NULL),
	                        make_image_node_setting(_tr("Invert"), "BOOL", 0, NULL),
	                        make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                    },
	                    5),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, normal_to_height_node_def);
	any_map_set(parser_material_node_values, "TEX_NORMAL_TO_HEIGHT", normal_to_height_node_value);
	any_map_set(parser_material_node_vectors, "TEX_NORMAL_TO_HEIGHT", normal_to_height_node_vector);
	any_map_set(ui_nodes_hidden_rules, "TEX_NORMAL_TO_HEIGHT", normal_to_height_node_item_hidden);
	make_image_node_sections(normal_to_height_node_def, NULL, 0, 2); // General: Normalize, Invert, Wrap Edges, Normal and Strength
	make_image_node_register(&normal_to_height_node_image_type);
}
