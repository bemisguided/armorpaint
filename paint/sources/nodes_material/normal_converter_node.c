#include "../global.h"

// Normal Converter (Filter menu): converts between a height image and a tangent space normal map, in both directions (the Mode setting):
//   Height to Normal: a height image to a normal map, optionally on top of an incoming normal.
//   Normal to Height: a normal map back to a height image (integration, see the end of this comment).
// The Bump node stays as it is (it reads the height in the material shader, per pixel on screen); this node bakes in image space.
// An image node (render/make_image_node.c): the Height input (the height, grey, 0 black to 1 white) and the
// Normal input are evaluated in image space (32 bit float so a smooth height does not band) and the normal map is computed
// on the cpu into one RGBA texture per node, RGB = normal * 0.5 + 0.5, sampled like an Image Texture at tex_coord,
// repeating (Tile UV places it on the uv).
// Bump reads ddx / ddy of the height, derivatives over a 2 x 2 pixel quad, so the slope is constant inside the quad and a
// thin diagonal ridge gets blocky normals. Here the slope of every texel is a Sobel gradient of the baked height plane, a per
// pixel value without block structure.
//
// Maths, per texel with the 3 x 3 neighbourhood h (rows y down, columns x right, wrap or clamp at the border):
//   gx = (h[x+1][y-1] + 2 h[x+1][y] + h[x+1][y+1]) - (h[x-1][y-1] + 2 h[x-1][y] + h[x-1][y+1])   (Sobel)
//   gy = (h[x-1][y+1] + 2 h[x][y+1] + h[x+1][y+1]) - (h[x-1][y-1] + 2 h[x][y-1] + h[x+1][y-1])
//   sx = Strength * gx / 8 * Size / 1024, sy likewise    height change per texel at Size 1024 (per image width / 1024 at any Size), a ramp of 1 per texel at Size 1024 is 1
//   d  = normalize(-sx, sy, 1)                        the detail normal of the height; green up on the image (up is -y rows)
// The slope does not depend on Size: the 1/8 is the Sobel weight sum (4) times the two texel central difference, Size / 1024 turns
// the per texel difference into one per image width (a larger Size has smaller differences per texel for the same picture), so
// Strength 1 gives the same look at 512 and at 4096 (at 1024 a texel is the unit; a ridge of Edge Width w is w * Size texels wide).
// OpenGL convention only (+Y up, green channel up): the one the engine's Normal Map handling expects (a DirectX map is
// converted to it on import, image_texture_node.c). At Size 1024 a ramp of 1 per texel at Strength 1 tilts 45
// degrees, a height of 1 over the whole image at Strength 1 only 0.056 degrees. Strength scales the slope of the height linearly and nothing else.
// Normal input (like the Bump node's Normal input; unlinked it is the flat normal (0.5, 0.5, 1), meaning no incoming normal):
// an encoded tangent space normal, decoded (2 v - 1) and renormalised per texel. The height detail is combined with it by
// Reoriented Normal Mapping (base = incoming normal b, detail d):
//   t = b + (0, 0, 1), u = d * (-1, -1, 1), n = normalize(t * dot(t, u) / t.z - u)
// which turns the detail by the rotation that takes the flat normal to b. Without an incoming normal the result is d itself
// (exactly: the flat base is taken as a shortcut), a flat height gives b back. The Bump node adds the raw vector to its
// (ddx, ddy, 1), which is only meaningful for a decoded unit normal that does not tilt much; RNM keeps the detail's slope
// on a tilted base, so it is used here instead.
// The result is stored encoded in half floats (precision 2^-11 near 0.5..1, about 0.06 degrees), the sampling filter is bilinear:
// a filtered normal is not of unit length any more, the Normal Map node renormalises it, and a node that does not has to.
// Invert flips the sign of the height detail (Bump's Invert): the height direction is reversed, the incoming normal is not.
// The inputs and their order mirror the Bump node (Strength, Height, Normal) (Bump stays; the node is not a replacement); this is the one image
// node whose first input is not Image. Bump's Distance input has no effect in its shader and is not copied.
// Strength is scalar for the whole image (evaluated at the image centre when linked): the slope scale has to be known per bake.
// The capture packs both inputs in one RGBA float: R = height, GBA = the incoming normal (encoded 0..1).
//
// Normal to Height. The Normal input is the SOURCE here (default flat (0.5, 0.5, 1) gives a flat height, 0.5 everywhere with Normalize
// off; with Normalize on a constant height is 0.5 as well); the Height input is not read (not captured) and is hidden; Mode Height to
// Normal uses Normal as the optional base to chain on. The normal is decoded (2 c - 1), renormalised, nz clamped to at least
// 0.05 (slopes up to 20, 87 degrees; no blow up for a normal that lies down), and turned into the slopes the forward
// direction produces: sx = -nx / nz, sy = ny / nz (OpenGL, green up, rows down; the same convention as above, so a normal
// made by Height to Normal returns its height).
// The height is the least squares solution of  S(h) * c = (sx, sy)  with S the Sobel operator of Height to Normal and c = Strength / 8 *
// Size / 1024, solved with FFTs in the frequency domain (Frankot Chellappa, with the exact frequency response of the Sobel taps instead of
// i * k):
//   H = (conj(Dx) Sx + conj(Dy) Sy) / (|Dx|^2 + |Dy|^2 + L (sin^2(wx / 2) + sin^2(wy / 2))^2),  Dx = i sin(wx) (1 + cos(wy)), Dy = i sin(wy) (1 + cos(wx))
// (Sobel / 4 as a response, so h = ifft(H) / (4 c)). The frequencies where both responses vanish (the DC term and the Nyquist lines wx = pi or
// wy = pi: only a checker pattern of the height has no Sobel) are set to 0. L (NORMAL_CONVERTER_SMOOTHING, 1e-2) is a smoothness (Laplacian)
// penalty of the least squares: it is negligible for the low and middle frequencies (weight ~ w^4 against w^2) and damps the high ones
// where the Sobel responses are almost lost, which would turn the noise of an 8 bit or half float normal into large waves (measured: a constant damping
// instead ate the low frequencies of a smooth bump at 1e-4, and at 1e-8 the half float normals of the node itself gave wave errors of 0.2).
// Using the node's own Sobel response makes the conversion the inverse of Height to Normal for any height: N2H(H2N(h)) = h up to the constant
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
// that Height to Normal used to make the normal (the node's Strength, default 4, "the Strength of the converter that made this normal"):
// the same Strength and Size give the height back. Strength 0 gives a flat 0.5. Invert flips the height (1 - h).
// The result is RGBA half float like Height to Normal (heights resolve about 0.0005 near 1): R = G = B = height, A = 1.

// Input socket indices
// Same order as the Bump node (Strength, Height, Normal; Bump's Distance does nothing in its shader and is left out), like Bump
enum {
	NORMAL_CONVERTER_INPUT_STRENGTH = 0,
	NORMAL_CONVERTER_INPUT_HEIGHT   = 1,
	NORMAL_CONVERTER_INPUT_NORMAL   = 2,
};

// Node button indices (settings)
enum {
	NORMAL_CONVERTER_BUTTON_SIZE      = 0, // Button 1 is the state row
	NORMAL_CONVERTER_BUTTON_MODE      = 2,
	NORMAL_CONVERTER_BUTTON_NORMALIZE = 3,
	NORMAL_CONVERTER_BUTTON_INVERT    = 4,
	NORMAL_CONVERTER_BUTTON_WRAP      = 5,
};

// Output socket indices
enum {
	NORMAL_CONVERTER_OUTPUT_NORMAL = 0,
	NORMAL_CONVERTER_OUTPUT_HEIGHT = 1,
};

// Mode setting
enum {
	NORMAL_CONVERTER_MODE_HEIGHT_TO_NORMAL = 0,
	NORMAL_CONVERTER_MODE_NORMAL_TO_HEIGHT = 1,
};

#define NORMAL_CONVERTER_MIN_NZ 0.05 // Normal to Height: the smallest z of a decoded normal (slope 20)
#define NORMAL_CONVERTER_SMOOTHING 1e-2 // Normal to Height: weight of the smoothness term of the integration, see the header

typedef struct normal_converter_params {
	i32 mode;
	i32 normalize;
	i32 invert;
	i32 wrap;
	f32 strength;
} normal_converter_params_t;

// Which rows of the node are not shown: Height to Normal has no Height output and no Normalize; Normal to Height has no Height input and no Normal output,
// and a normalised result does not use Strength. A linked socket is never hidden by the caller (ui_nodes_on_item_hidden).
static bool normal_converter_node_hidden(i32 mode, i32 normalize, ui_node_item_t kind, i32 index) {
	bool n2h = mode == NORMAL_CONVERTER_MODE_NORMAL_TO_HEIGHT;
	if (kind == UI_NODE_ITEM_INPUT) {
		return n2h && (index == NORMAL_CONVERTER_INPUT_HEIGHT || (index == NORMAL_CONVERTER_INPUT_STRENGTH && normalize != 0));
	}
	if (kind == UI_NODE_ITEM_OUTPUT) {
		return n2h ? index == NORMAL_CONVERTER_OUTPUT_NORMAL : index == NORMAL_CONVERTER_OUTPUT_HEIGHT;
	}
	return !n2h && index == NORMAL_CONVERTER_BUTTON_NORMALIZE;
}

static bool normal_converter_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	return normal_converter_node_hidden((i32)make_image_node_button(node, NORMAL_CONVERTER_BUTTON_MODE), make_image_node_button(node, NORMAL_CONVERTER_BUTTON_NORMALIZE) != 0.0, kind, index);
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 normal_converter_node_size(ui_node_t *node) {
	return make_image_node_size(node, NORMAL_CONVERTER_BUTTON_SIZE);
}

// Captured per pixel: the Height input as grey in R, the Normal input in GBA
static char *normal_converter_node_capture(ui_node_t *node) {
	bool  n2h    = (i32)make_image_node_button(node, NORMAL_CONVERTER_BUTTON_MODE) == NORMAL_CONVERTER_MODE_NORMAL_TO_HEIGHT;
	char *image  = n2h ? "0.0" : parser_material_parse_value_input(node->inputs->buffer[NORMAL_CONVERTER_INPUT_HEIGHT], false); // Normal to Height does not read Height
	char *normal = parser_material_parse_vector_input(node->inputs->buffer[NORMAL_CONVERTER_INPUT_NORMAL]);
	char *grey   = string_tmp("%s_grey", parser_material_store_var_name(node));
	char *nor    = string_tmp("%s_nor", parser_material_store_var_name(node));
	parser_material_write(parser_material_kong, string_tmp("float %s = clamp(%s, 0.0, 1.0);", grey, image));
	parser_material_write(parser_material_kong, string_tmp("float3 %s = %s;", nor, normal));
	return string_tmp("float4(%s, %s.x, %s.y, %s.z)", grey, nor, nor, nor);
}

static void normal_converter_node_params(ui_node_t *node, void *params) {
	normal_converter_params_t *p = params;
	p->mode                      = (i32)make_image_node_button(node, NORMAL_CONVERTER_BUTTON_MODE);
	p->normalize                 = make_image_node_button(node, NORMAL_CONVERTER_BUTTON_NORMALIZE) != 0.0;
	p->invert                    = make_image_node_button(node, NORMAL_CONVERTER_BUTTON_INVERT) != 0.0;
	p->wrap                      = make_image_node_button(node, NORMAL_CONVERTER_BUTTON_WRAP) != 0.0;
	p->strength                  = make_image_node_input_value(node, NORMAL_CONVERTER_INPUT_STRENGTH);
}

static i32 normal_converter_node_wrap_index(i32 i, i32 size, bool wrap) {
	if (wrap) {
		return ((i % size) + size) % size;
	}
	return i < 0 ? 0 : i >= size ? size - 1 : i;
}

// Detail normal d (unit, tilt from the slope) on the base normal b (unit): Reoriented Normal Mapping
static void normal_converter_node_rnm(const f32 *b, const f32 *d, f32 *n) {
	f32 tx = b[0], ty = b[1], tz = b[2] + 1.0f;
	f32 ux = -d[0], uy = -d[1], uz = d[2];
	f32 k  = (tx * ux + ty * uy + tz * uz) / tz;
	f32 rx = tx * k - ux, ry = ty * k - uy, rz = tz * k - uz;
	f32 l  = sqrtf(rx * rx + ry * ry + rz * rz);
	l      = l > 1e-12f ? 1.0f / l : 0.0f;
	n[0]   = rx * l;
	n[1]   = ry * l;
	n[2]   = l > 0.0f ? rz * l : 1.0f;
}

typedef struct normal_converter_ctx {
	f32 *pixels; // Captured RGBA: the height in R, the incoming normal encoded in GBA
	f32 *result;
	f32  scale; // Strength / 8 * Size / 1024
	i32  size;
	bool wrap;
} normal_converter_ctx_t;

// Rows [begin, end) of the normal map
static void normal_converter_node_rows(void *param, i32 begin, i32 end) {
	normal_converter_ctx_t *c = param;
	f32 *pixels = c->pixels;
	i32  size   = c->size;
	for (i32 y = begin; y < end; ++y) {
		i32 ym = normal_converter_node_wrap_index(y - 1, size, c->wrap);
		i32 yp = normal_converter_node_wrap_index(y + 1, size, c->wrap);
		for (i32 x = 0; x < size; ++x) {
			i32 xm = normal_converter_node_wrap_index(x - 1, size, c->wrap);
			i32 xp = normal_converter_node_wrap_index(x + 1, size, c->wrap);
#define NC_AT(px_, py_) pixels[((size_t)(py_) * size + (px_)) * 4]
			f32 gx = (NC_AT(xp, ym) + 2.0f * NC_AT(xp, y) + NC_AT(xp, yp)) - (NC_AT(xm, ym) + 2.0f * NC_AT(xm, y) + NC_AT(xm, yp));
			f32 gy = (NC_AT(xm, yp) + 2.0f * NC_AT(x, yp) + NC_AT(xp, yp)) - (NC_AT(xm, ym) + 2.0f * NC_AT(x, ym) + NC_AT(xp, ym));
#undef NC_AT
			f32 nx    = -gx * c->scale;
			f32 ny    = gy * c->scale;
			f32 inv   = 1.0f / sqrtf(nx * nx + ny * ny + 1.0f);
			f32 d[3]  = {nx * inv, ny * inv, inv};
			f32 *in   = pixels + ((size_t)y * size + x) * 4;
			f32 bx    = in[1] * 2.0f - 1.0f, by = in[2] * 2.0f - 1.0f, bz = in[3] * 2.0f - 1.0f;
			f32 *o    = c->result + ((size_t)y * size + x) * 4;
			f32 n[3]  = {d[0], d[1], d[2]};
			if (!(bx == 0.0f && by == 0.0f && bz == 1.0f)) { // Not the flat normal: reorient the detail onto the incoming one
				f32 bl = sqrtf(bx * bx + by * by + bz * bz);
				f32 b[3];
				if (bl > 1e-6f) {
					b[0] = bx / bl;
					b[1] = by / bl;
					b[2] = bz / bl;
				}
				else {
					b[0] = 0.0f;
					b[1] = 0.0f;
					b[2] = 1.0f;
				}
				if (b[2] < 1e-3f) { // Pointing away from the surface: keep t.z away from 0
					b[2]     = 1e-3f;
					f32 l    = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
					b[0]    /= l;
					b[1]    /= l;
					b[2]    /= l;
				}
				normal_converter_node_rnm(b, d, n);
			}
			o[0] = n[0] * 0.5f + 0.5f;
			o[1] = n[1] * 0.5f + 0.5f;
			o[2] = n[2] * 0.5f + 0.5f;
			o[3] = 1.0f;
		}
	}
}

// Normal to Height. In place radix 2 FFT of the interleaved complex line a (n complex floats, n a power of two). tw holds cos and sin of 2 pi j / n
// for j < n / 2 (computed in double); dir 1 is the forward transform (e^-i), -1 the inverse (unnormalised)
static void normal_converter_node_fft_line(f32 *a, i32 n, const f32 *tw, i32 dir) {
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

typedef struct normal_converter_fft_ctx {
	f32       *data; // m x m complex floats, row major
	i32        m;
	const f32 *tw;
	i32        dir;
} normal_converter_fft_ctx_t;

static void normal_converter_node_fft_rows(void *param, i32 begin, i32 end) {
	normal_converter_fft_ctx_t *c = param;
	for (i32 y = begin; y < end; ++y) {
		normal_converter_node_fft_line(c->data + (size_t)y * c->m * 2, c->m, c->tw, c->dir);
	}
}

// Column groups of 8 (one cache line of complex floats) are copied to a scratch buffer, transformed and copied back
static void normal_converter_node_fft_columns(void *param, i32 begin, i32 end) {
	normal_converter_fft_ctx_t *c = param;
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
			normal_converter_node_fft_line(scratch + (size_t)k * m * 2, m, c->tw, c->dir);
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
static void normal_converter_node_fft2d(f32 *data, i32 m, const f32 *tw, i32 dir) {
	normal_converter_fft_ctx_t c = {.data = data, .m = m, .tw = tw, .dir = dir};
	make_image_node_parallel(m, normal_converter_node_fft_rows, &c);
	make_image_node_parallel(m / 8, normal_converter_node_fft_columns, &c);
}

typedef struct normal_converter_n2h_ctx {
	const f32 *pixels; // size x size RGBA, the normal encoded in GBA
	f32       *plane;  // m x m complex
	i32        size;
	i32        m;
	bool       mirror;
	const f32 *sn; // sin and cos of 2 pi k / m
	const f32 *cs;
} normal_converter_n2h_ctx_t;

// Rows [begin, end) of the image: the slopes sx + i sy of the decoded normals into the plane (and their mirror images)
static void normal_converter_node_n2h_fill(void *param, i32 begin, i32 end) {
	normal_converter_n2h_ctx_t *c = param;
	i32 size = c->size, m = c->m;
	for (i32 y = begin; y < end; ++y) {
		for (i32 x = 0; x < size; ++x) {
			const f32 *in = c->pixels + ((size_t)y * size + x) * 4;
			f32 nx = in[1] * 2.0f - 1.0f, ny = in[2] * 2.0f - 1.0f, nz = in[3] * 2.0f - 1.0f;
			f32 l = sqrtf(nx * nx + ny * ny + nz * nz);
			f32 sx = 0.0f, sy = 0.0f;
			if (l > 1e-6f && isfinite(l)) { // A zero or NaN normal is flat
				nx /= l;
				ny /= l;
				nz /= l;
				nz = nz < (f32)NORMAL_CONVERTER_MIN_NZ ? (f32)NORMAL_CONVERTER_MIN_NZ : nz;
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
static void normal_converter_node_n2h_solve(void *param, i32 begin, i32 end) {
	normal_converter_n2h_ctx_t *c = param;
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
				f32 g   = 1.0f / (den + (f32)NORMAL_CONVERTER_SMOOTHING * lap * lap);
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

// Normal to Height: see the header. bytes is the capture (float RGBA, the normal in GBA), result the size x size RGBA plane
static i32 normal_converter_node_normal_to_height(u8 *bytes, i32 size, normal_converter_params_t *p, f32 *result) {
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
	normal_converter_n2h_ctx_t c = {.pixels = (const f32 *)bytes, .plane = plane, .size = size, .m = m, .mirror = !p->wrap, .sn = sn, .cs = cs};
	make_image_node_parallel(size, normal_converter_node_n2h_fill, &c);
	normal_converter_node_fft2d(plane, m, tw, 1);
	make_image_node_parallel(m / 2 + 1, normal_converter_node_n2h_solve, &c);
	normal_converter_node_fft2d(plane, m, tw, -1);
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

// Result of the captured height and normal; returns 0, or -1 when memory runs out
static i32 normal_converter_node_process(u8 *bytes, i32 size, void *params, f32 *result) {
	normal_converter_params_t p = *(normal_converter_params_t *)params;
	if (p.mode == NORMAL_CONVERTER_MODE_NORMAL_TO_HEIGHT) {
		return normal_converter_node_normal_to_height(bytes, size, &p, result);
	}
	f32 strength = p.strength;
	strength     = !(strength > 0.0f) ? 0.0f : strength > 1000.0f ? 1000.0f : strength; // Also keeps NaN out
	strength     = p.invert ? -strength : strength;
	normal_converter_ctx_t c = {.pixels = (f32 *)bytes, .result = result, .scale = strength / 8.0f * (f32)size / 1024.0f, .size = size, .wrap = p.wrap != 0};
	make_image_node_parallel(size, normal_converter_node_rows, &c);
	return 0;
}

static image_node_type_t normal_converter_node_image_type = {
    .type          = "TEX_NORMAL_CONVERTER",
    .result_format = GPU_TEXTURE_FORMAT_RGBA64, // A normal encoded in 0..1: half floats resolve 0.06 degrees (a height 0.0005), no second plane
    .params_size   = sizeof(normal_converter_params_t),
    .size          = normal_converter_node_size,
    .capture       = normal_converter_node_capture,
    .capture_format = IMAGE_CAPTURE_RGBA32F,
    .smooth_sample = true,
    .params        = normal_converter_node_params,
    .process       = normal_converter_node_process,
};

char *normal_converter_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.rgb", make_image_node_sample(node));
}

char *normal_converter_node_value(ui_node_t *node, ui_node_socket_t *socket) { // The Height output (R = G = B in Normal to Height)
	return string_tmp("%s.r", make_image_node_sample(node));
}

void normal_converter_node_init() {

	ui_node_t *normal_converter_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Normal Converter"),
	                .type   = "TEX_NORMAL_CONVERTER",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Strength"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(4.0), 0.0, 100.0, 100),
	                        make_image_node_socket(_tr("Height"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Normal"), "VECTOR", NODES_MATERIAL_VECTOR_COLOR, f32_array_create_xyz(0.5, 0.5, 1.0), 0.0, 1.0, 100),
	                    },
	                    3),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Normal"), "VECTOR", NODES_MATERIAL_VECTOR_COLOR, f32_array_create_xyz(0.5, 0.5, 1.0), 0.0, 1.0, 100), // A direction: purple like the Normal input
	                        make_image_node_socket(_tr("Height"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                    },
	                    2),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Size"), "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS),
	                        make_image_node_state_button(),
	                        make_image_node_setting(_tr("Mode"), "ENUM", 0, "Height to Normal\nNormal to Height"),
	                        make_image_node_setting(_tr("Normalize"), "BOOL", 1, NULL),
	                        make_image_node_setting(_tr("Invert"), "BOOL", 0, NULL),
	                        make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                    },
	                    6),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_filter, normal_converter_node_def);
	any_map_set(parser_material_node_values, "TEX_NORMAL_CONVERTER", normal_converter_node_value);
	any_map_set(parser_material_node_vectors, "TEX_NORMAL_CONVERTER", normal_converter_node_vector);
	any_map_set(ui_nodes_hidden_rules, "TEX_NORMAL_CONVERTER", normal_converter_node_item_hidden);
	make_image_node_sections(normal_converter_node_def, NULL, 0, 3); // General: Mode, Normalize, Invert, Wrap Edges, Strength, Height and Normal
	make_image_node_register(&normal_converter_node_image_type);
}
