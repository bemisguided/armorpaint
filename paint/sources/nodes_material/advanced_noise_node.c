#include "../global.h"

// Advanced Noise (Pattern menu): a tileable noise image. An image node (render/make_image_node.c) that generates its image
// itself: the capture shader writes the noise per pixel in image space (uv across the image, float, grey) and the render target it
// draws into IS the result texture at the Size setting (a GPU baked node, result by reference: no readback, no copy, no upload),
// sampled by Tile UV like every other pattern node. No Vector input (image space rule).
// Next to Noise Texture (Blender style 3D procedural noise on the mesh, no resolution limit) it is the Substance style
// generator: modes, tileable by construction, composes with the other image nodes (specs/noise/ADVANCED_NOISE.md).
// The Mode setting decides the maths and which inputs and settings are shown (ui_nodes_hidden_rules, a linked socket is
// always shown). Modes are rows of a table (advanced_noise_modes_init); the spec describes how to add one.
//
// The noise point is p = (uv, z): uv in 0..1 across the image, z = 0.3 + 0.4 * frac(Seed) a constant slice (the integer part of
// Seed picks the hash seed, the fraction moves the slice). Every octave multiplies x and y of p by its frequency (Scale = lattice
// cells across the image for the first octave), z stays: the slice is inside the first z cell of every octave, never on a lattice
// plane (z * frequency must not be an integer: a slice exactly on a z lattice plane is a 2D noise of the 12 gradients projected, which
// shows axis aligned streaks and triangular crests, seen with z = 0 and with z = 0.5 at Scale 5).
//
// Mode Perlin: gradient noise (improved Perlin, the 12 edge midpoint gradients), fade 6t^5 - 15t^4 + 10t^3.
//   - Lattice coordinates wrap at 65536 cells (a bound for the int conversion), or at the period when Tileable (at most 65536).
//   - Octave k >= 1 turns the lattice by the angle perlin_turn(k) (a table of fractions of a quarter turn, the first octave stays
//     axis aligned so Detail 0 is the plain noise): every octave of a gradient noise has its power on the lattice axes, equal
//     octaves stack that into a grid bias (faint horizontal and vertical streaks after an edge detect). The turned lookup is the
//     point (a x - b y, b x + a y) with (a, b) = f (cos, sin); Tileable rounds a and b to integers: the matrix [[a, -b], [b, a]]
//     maps the integer lattice onto the sublattice it spans, and the lattice cells are reduced modulo that sublattice
//     (perlin_wrap_i, perlin_wrap_j) so uv + (1, 0) and uv + (0, 1) land on the same cell: the image still repeats exactly
//     with no shorter repeat (the sublattice has index a^2 + b^2 = the number of cells of the image). The octave frequency becomes
//     sqrt(a^2 + b^2) (within a few percent of f at f >= 8). Not Tileable uses the exact cos and sin.
//   - Integer hash (no sin) of the lattice corner and the seed, so the CPU host test and the GPU agree.
//   - fBM like Noise Texture: Detail 0 is one octave, Detail d has floor(d) + 1 octaves and a fractional part blends
//     the next octave in, Roughness is the gain, Lacunarity the frequency step; the sum is divided by the sum of the
//     gains. Each octave has its own seed.
//   - Distortion displaces the point by a noise vector (in lattice units of Scale) before the fBM.
//   - Output 0.5 + 0.5 * clamp(PERLIN_NORM * n, -1, 1): about 0..1 centred on 0.5 (PERLIN_NORM = 1.3 is the measured
//     99.9 percentile of |n| (0.756) rounded so that the practical range of one octave fills 0..1: automation/host/advanced_noise).
//   - Tileable (default on, the generator's Wrap Edges): the lattice repeats every round(Scale * Lacunarity^k) cells in
//     each octave k (Scale is snapped to an integer first), so the image repeats exactly: the value at uv 0 equals the value at uv 1.

char *str_advanced_noise_perlin = "\
float perlin_wrap(float i, float period) { \
	float m = 65536.0; \
	if (period >= 0.5) { m = min(period, 65536.0); } \
	return i - m * floor((i + 0.5) / m); \
} \
int perlin_hash(float ix, float iy, float iz, int seed) { \
	int h = int(ix) * 374761393 + int(iy) * 668265263 + int(iz) * 1274126177 + seed * 1103515245; \
	h = (h ^ ((h >> 13) & 524287)) * 1274126177; \
	h = h ^ ((h >> 16) & 65535); \
	h = h * 668265263; \
	h = h ^ ((h >> 15) & 131071); \
	return h; \
} \
float perlin_grad(int h, float x, float y, float z) { \
	int k = h & 15; \
	float u = y; \
	if (k < 8) { u = x; } \
	float v = z; \
	if (k < 4) { v = y; } \
	if (k == 12 || k == 14) { v = x; } \
	float su = u; \
	if ((k & 1) != 0) { su = 0.0 - u; } \
	float sv = v; \
	if ((k & 2) != 0) { sv = 0.0 - v; } \
	return su + sv; \
} \
float perlin_fade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); } \
float perlin_noise_3d(float3 p, float period, int seed) { \
	float3 fl = floor(p); \
	float fx = p.x - fl.x; \
	float fy = p.y - fl.y; \
	float fz = p.z - fl.z; \
	float x0 = perlin_wrap(fl.x, period); \
	float x1 = perlin_wrap(fl.x + 1.0, period); \
	float y0 = perlin_wrap(fl.y, period); \
	float y1 = perlin_wrap(fl.y + 1.0, period); \
	float z0 = perlin_wrap(fl.z, period); \
	float z1 = perlin_wrap(fl.z + 1.0, period); \
	float n000 = perlin_grad(perlin_hash(x0, y0, z0, seed), fx, fy, fz); \
	float n100 = perlin_grad(perlin_hash(x1, y0, z0, seed), fx - 1.0, fy, fz); \
	float n010 = perlin_grad(perlin_hash(x0, y1, z0, seed), fx, fy - 1.0, fz); \
	float n110 = perlin_grad(perlin_hash(x1, y1, z0, seed), fx - 1.0, fy - 1.0, fz); \
	float n001 = perlin_grad(perlin_hash(x0, y0, z1, seed), fx, fy, fz - 1.0); \
	float n101 = perlin_grad(perlin_hash(x1, y0, z1, seed), fx - 1.0, fy, fz - 1.0); \
	float n011 = perlin_grad(perlin_hash(x0, y1, z1, seed), fx, fy - 1.0, fz - 1.0); \
	float n111 = perlin_grad(perlin_hash(x1, y1, z1, seed), fx - 1.0, fy - 1.0, fz - 1.0); \
	float u = perlin_fade(fx); \
	float v = perlin_fade(fy); \
	float w = perlin_fade(fz); \
	return lerp(lerp(lerp(n000, n100, u), lerp(n010, n110, u), v), lerp(lerp(n001, n101, u), lerp(n011, n111, u), v), w); \
} \
float perlin_base_scale(float scale, float tileable) { \
	if (tileable > 0.5) { return max(1.0, floor(scale + 0.5)); } \
	return max(scale, 0.0001); \
} \
float perlin_wrap_i(float i, float j, float a, float b, float n2) { \
	if (n2 < 0.5) { return perlin_wrap(i, 0.0); } \
	float u = floor((a * i + b * j) / n2); \
	float w = floor((0.0 - b * i + a * j) / n2); \
	return perlin_wrap(i - (a * u - b * w), 0.0); \
} \
float perlin_wrap_j(float i, float j, float a, float b, float n2) { \
	if (n2 < 0.5) { return perlin_wrap(j, 0.0); } \
	float u = floor((a * i + b * j) / n2); \
	float w = floor((0.0 - b * i + a * j) / n2); \
	return perlin_wrap(j - (b * u + a * w), 0.0); \
} \
float perlin_corner(float cx, float cy, float z, float dx, float dy, float dz, float a, float b, float n2, int seed) { \
	return perlin_grad(perlin_hash(perlin_wrap_i(cx, cy, a, b, n2), perlin_wrap_j(cx, cy, a, b, n2), z, seed), dx, dy, dz); \
} \
float perlin_noise_rot(float3 p, float a, float b, float n2, int seed) { \
	float3 fl = floor(p); \
	float fx = p.x - fl.x; \
	float fy = p.y - fl.y; \
	float fz = p.z - fl.z; \
	float z0 = perlin_wrap(fl.z, 0.0); \
	float z1 = perlin_wrap(fl.z + 1.0, 0.0); \
	float n000 = perlin_corner(fl.x, fl.y, z0, fx, fy, fz, a, b, n2, seed); \
	float n100 = perlin_corner(fl.x + 1.0, fl.y, z0, fx - 1.0, fy, fz, a, b, n2, seed); \
	float n010 = perlin_corner(fl.x, fl.y + 1.0, z0, fx, fy - 1.0, fz, a, b, n2, seed); \
	float n110 = perlin_corner(fl.x + 1.0, fl.y + 1.0, z0, fx - 1.0, fy - 1.0, fz, a, b, n2, seed); \
	float n001 = perlin_corner(fl.x, fl.y, z1, fx, fy, fz - 1.0, a, b, n2, seed); \
	float n101 = perlin_corner(fl.x + 1.0, fl.y, z1, fx - 1.0, fy, fz - 1.0, a, b, n2, seed); \
	float n011 = perlin_corner(fl.x, fl.y + 1.0, z1, fx, fy - 1.0, fz - 1.0, a, b, n2, seed); \
	float n111 = perlin_corner(fl.x + 1.0, fl.y + 1.0, z1, fx - 1.0, fy - 1.0, fz - 1.0, a, b, n2, seed); \
	float u = perlin_fade(fx); \
	float v = perlin_fade(fy); \
	float w = perlin_fade(fz); \
	return lerp(lerp(lerp(n000, n100, u), lerp(n010, n110, u), v), lerp(lerp(n001, n101, u), lerp(n011, n111, u), v), w); \
} \
float perlin_turn(int k) { \
	if (k == 1) { return 0.5; } \
	if (k == 2) { return 0.4; } \
	if (k == 3) { return 0.15; } \
	if (k == 4) { return 0.65; } \
	if (k == 5) { return 0.3; } \
	if (k == 6) { return 0.8; } \
	if (k == 7) { return 0.05; } \
	if (k == 8) { return 0.55; } \
	if (k == 9) { return 0.9; } \
	return 0.0; \
} \
float perlin_octave(float3 p, float freq, float tileable, int seed, int k) { \
	float f = freq; \
	float period = 0.0; \
	if (tileable > 0.5) { \
		f = max(1.0, floor(freq + 0.5)); \
		period = f; \
	} \
	if (k == 0) { return perlin_noise_3d(float3(p.x * f, p.y * f, p.z), period, seed); } \
	float ang = perlin_turn(k) * 1.5707963; \
	float a = f * cos(ang); \
	float b = f * sin(ang); \
	float n2 = 0.0; \
	if (tileable > 0.5) { \
		a = floor(a + 0.5); \
		b = floor(b + 0.5); \
		n2 = a * a + b * b; \
	} \
	return perlin_noise_rot(float3(a * p.x - b * p.y, b * p.x + a * p.y, p.z), a, b, n2, seed); \
} \
float perlin_signed(float3 p, float freq, float tileable, int seed) { \
	return clamp(perlin_octave(p, freq, tileable, seed, 0) * 1.3, -1.0, 1.0); \
} \
float perlin_fbm(float3 p, float scale, float detail, float roughness, float lacunarity, float tileable, int seed) { \
	float freq = perlin_base_scale(scale, tileable); \
	float amp = 1.0; \
	float maxamp = 0.0; \
	float sum = 0.0; \
	int n = int(clamp(detail, 0.0, 8.0)); \
	for (int ii = 0; ii <= n; ii += 1) { \
		sum = sum + amp * perlin_octave(p, freq, tileable, seed + ii * 31, ii); \
		maxamp = maxamp + amp; \
		amp = amp * roughness; \
		freq = freq * lacunarity; \
	} \
	float rmd = clamp(detail, 0.0, 8.0) - floor(clamp(detail, 0.0, 8.0)); \
	if (rmd > 0.0) { \
		float t = perlin_octave(p, freq, tileable, seed + (n + 1) * 31, n + 1); \
		return lerp(sum / maxamp, (sum + t * amp) / (maxamp + amp), rmd); \
	} \
	return sum / maxamp; \
} \
float perlin_factor(float3 p, float scale, float detail, float roughness, float lacunarity, float distortion, float tileable, float seed) { \
	int sd = int(floor(seed)); \
	float3 q = p; \
	if (distortion != 0.0) { \
		float se = perlin_base_scale(scale, tileable); \
		q = p + distortion / se * float3(perlin_signed(p, se, tileable, sd + 101), perlin_signed(p, se, tileable, sd + 202), perlin_signed(p, se, tileable, sd + 303)); \
	} \
	float n = perlin_fbm(q, scale, detail, roughness, lacunarity, tileable, sd); \
	return 0.5 + 0.5 * clamp(n * 1.3, -1.0, 1.0); \
} \
float perlin_image(float u, float v, float scale, float detail, float roughness, float lacunarity, float distortion, float tileable, float seed) { \
	float z = 0.3 + 0.4 * (seed - floor(seed)); \
	return perlin_factor(float3(u, v, z), scale, detail, roughness, lacunarity, distortion, tileable, seed); \
} \
";

// Mode Voronoi: tileable cell pattern (Worley noise) on a jittered grid, the Substance style generator next to the Voronoi Texture node
// (Blender style 3D shader node, sin hash, untouched). 2D: one feature point per grid cell, Scale cells across the image.
//   - The feature point of cell (i, j) is (i + 0.5 + R (jx - 0.5), j + 0.5 + R (jy - 0.5)), R = Randomness 0..1, jx and jy two 10 bit
//     fields of the integer hash of the cell (perlin_hash, shared with the Perlin mode): the point lies in its own cell for every R, R = 0
//     is the regular grid of cell centres.
//   - F1 and F2 are the distances (metric: Euclidean, Manhattan, Chebyshev or Minkowski) to the nearest and the second nearest feature
//     point, searched in the 5 x 5 cells around the pixel: exact (a point 3 cells away is 2 or more away in one axis, so every metric with
//     d >= Chebyshev distance is more than 2; F2 stays below 2 for R <= 1, measured for the Minkowski exponents 1..32).
//   - Metric Minkowski: d = (|dx|^p + |dy|^p)^(1/p) with p = the Exponent input (1..32): p = 1 is Manhattan, p = 2 Euclidean, a large p
//     approaches Chebyshev. Computed as m (|dx| / m)^p ... with m = max(|dx|, |dy|) so a tiny offset does not underflow the power. Below
//     p = 1 the metric is not convex and the 5 x 5 window misses the nearest point (measured 0.01 % of the pixels at p = 0.5, 1 % at 0.25):
//     the range starts at 1. The normalisation constant is interpolated between knots at p = 1, 2, 4, 8, 16 (the constants of Manhattan,
//     Euclidean and Chebyshev at the ends).
//   - Feature: Distance = F1 (0 at the points), Edges = 1 - (F2 - F1) (1 at the cell borders where F1 = F2), Cells = a random grey per
//     cell (hash of the winning cell, flat), Crystals = F2, Smooth F1 (Blender's voronoi_smooth_f1: the polynomial smooth minimum of the
//     distances of the 5 x 5 cells with smoothstep weights, Smoothness 0..1 is halved as in Blender; Smoothness 0 is F1), Distance to Edge
//     (Blender's voronoi_distance_to_edge: the nearest point, then the smallest distance to a perpendicular bisector between it and the other
//     points of the 5 x 5 cells; Euclidean only, 0 on the cell borders, no Metric / Exponent). Each is normalised to 0..1 and clamped
//     (constants measured at R = 1 per metric; the distance to edge is scaled by 1.8, Blender returns the raw distance).
//   - Fractal Voronoi (Detail > 0): Blender's fractal accumulation of the normalised feature value: octave k at frequency Scale * Lacunarity^k
//     and weight Roughness^k (Roughness is the gain), its own hash seed (Seed + 31 k) so the octaves are different patterns; Detail 0 is one
//     octave (bit identical to the single feature), floor(Detail) + 1 octaves and the fraction blends the next octave in; Normalize (default on)
//     divides by the sum of the weights (0..1 like the Perlin fractal), off adds the octaves up and clamps (brighter). Every feature
//     supports it. Tileable rounds the frequency of every octave to an integer, so the image repeats exactly for ANY Lacunarity (a fractional
//     one only changes the rounded frequencies, e.g. Scale 5, Lacunarity 2.5: 5 13 31 78 195); the Distortion warp is applied once before the octaves.
//   - Tileable: the cell index is reduced modulo the period (round(Scale), at least 1) before the hash, so uv + 1 is the same cell; the
//     distances use the unreduced cell coordinates. Distortion warps the image point with the periodic Perlin noise of the base Scale
//     scaled by 0.25 (voronoi_gen_warp: the slider range 0..10 is shared with Perlin, whose Distortion 1 is a gentle warp of the noise, but
//     for cells the full amplitude moves the points a whole cell; Voronoi Distortion 1 = about 0.25 cell, 10 = about 2.5 cells)
//     (period = the same integer, so the warped point shifts by exactly one period: the image still repeats). The Seed fraction only
//     moves the z slice of that warp noise.
// It uses perlin_wrap, perlin_hash, perlin_base_scale and perlin_signed: the emit function adds the Perlin text first.
char *str_advanced_noise_voronoi = "\
float voronoi_gen_dist(float dx, float dy, int metric, float expo) { \
	if (metric == 1) { return abs(dx) + abs(dy); } \
	if (metric == 2) { return max(abs(dx), abs(dy)); } \
	if (metric == 3) { \
		float m = max(abs(dx), abs(dy)); \
		if (m < 0.000001) { return 0.0; } \
		return m * pow(pow(abs(dx) / m, expo) + pow(abs(dy) / m, expo), 1.0 / expo); \
	} \
	return sqrt(dx * dx + dy * dy); \
} \
float voronoi_gen_knots(float p, float k0, float k1, float k2, float k3, float k4) { \
	float t = clamp(log(p) * 1.442695, 0.0, 3.999); \
	float w = t - floor(t); \
	int k = int(floor(t)); \
	if (k == 0) { return lerp(k0, k1, w); } \
	if (k == 1) { return lerp(k1, k2, w); } \
	if (k == 2) { return lerp(k2, k3, w); } \
	return lerp(k3, k4, w); \
} \
float voronoi_gen_norm_f1(int metric, float expo) { \
	if (metric == 1) { return 0.8; } \
	if (metric == 2) { return 1.1; } \
	if (metric == 3) { return voronoi_gen_knots(expo, 0.8, 1.0, 1.06, 1.09, 1.1); } \
	return 1.0; \
} \
float voronoi_gen_norm_f2(int metric, float expo) { \
	if (metric == 1) { return 0.65; } \
	if (metric == 2) { return 0.9; } \
	if (metric == 3) { return voronoi_gen_knots(expo, 0.65, 0.85, 0.88, 0.895, 0.9); } \
	return 0.85; \
} \
float voronoi_gen_norm_edge(int metric, float expo) { \
	if (metric == 1) { return 0.8; } \
	if (metric == 2) { return 1.1; } \
	if (metric == 3) { return voronoi_gen_knots(expo, 0.8, 1.0, 1.06, 1.09, 1.1); } \
	return 1.0; \
} \
float3 voronoi_gen_cells(float x, float y, float period, float randomness, int metric, float expo, int seed) { \
	float ix = floor(x); \
	float iy = floor(y); \
	float f1 = 1000.0; \
	float f2 = 1000.0; \
	float wx = 0.0; \
	float wy = 0.0; \
	for (int jj = 0; jj < 5; jj += 1) { \
		for (int ii = 0; ii < 5; ii += 1) { \
			float cx = ix + float(ii) - 2.0; \
			float cy = iy + float(jj) - 2.0; \
			float rx = perlin_wrap(cx, period); \
			float ry = perlin_wrap(cy, period); \
			int h = perlin_hash(rx, ry, 0.0, seed); \
			float px = cx + 0.5 + randomness * (float(h & 1023) / 1023.0 - 0.5); \
			float py = cy + 0.5 + randomness * (float((h >> 10) & 1023) / 1023.0 - 0.5); \
			float d = voronoi_gen_dist(px - x, py - y, metric, expo); \
			if (d < f1) { \
				wx = rx; \
				wy = ry; \
			} \
			f2 = min(f2, max(d, f1)); \
			f1 = min(f1, d); \
		} \
	} \
	float id = float(perlin_hash(wx, wy, 1.0, seed) & 65535) / 65535.0; \
	return float3(f1, f2, id); \
} \
float voronoi_gen_smooth(float x, float y, float period, float randomness, int metric, float expo, float smoothness, int seed) { \
	float ix = floor(x); \
	float iy = floor(y); \
	float sm = clamp(smoothness, 0.0, 1.0) * 0.5; \
	float sd = 1000.0; \
	for (int jj = 0; jj < 5; jj += 1) { \
		for (int ii = 0; ii < 5; ii += 1) { \
			float cx = ix + float(ii) - 2.0; \
			float cy = iy + float(jj) - 2.0; \
			int h = perlin_hash(perlin_wrap(cx, period), perlin_wrap(cy, period), 0.0, seed); \
			float px = cx + 0.5 + randomness * (float(h & 1023) / 1023.0 - 0.5); \
			float py = cy + 0.5 + randomness * (float((h >> 10) & 1023) / 1023.0 - 0.5); \
			float d = voronoi_gen_dist(px - x, py - y, metric, expo); \
			float hh = 0.0; \
			if (sm > 0.0) { hh = smoothstep(0.0, 1.0, 0.5 + 0.5 * (sd - d) / sm); } \
			if (sm <= 0.0 && d <= sd) { hh = 1.0; } \
			sd = lerp(sd, d, hh) - sm * hh * (1.0 - hh); \
		} \
	} \
	return sd; \
} \
float voronoi_gen_norm_border() { return 1.8; } \
float voronoi_gen_edge(float x, float y, float period, float randomness, int seed) { \
	float ix = floor(x); \
	float iy = floor(y); \
	float cvx = 0.0; \
	float cvy = 0.0; \
	float md = 1000.0; \
	for (int jj = 0; jj < 5; jj += 1) { \
		for (int ii = 0; ii < 5; ii += 1) { \
			float cx = ix + float(ii) - 2.0; \
			float cy = iy + float(jj) - 2.0; \
			int h = perlin_hash(perlin_wrap(cx, period), perlin_wrap(cy, period), 0.0, seed); \
			float vx = cx + 0.5 + randomness * (float(h & 1023) / 1023.0 - 0.5) - x; \
			float vy = cy + 0.5 + randomness * (float((h >> 10) & 1023) / 1023.0 - 0.5) - y; \
			float d = vx * vx + vy * vy; \
			if (d < md) { \
				md = d; \
				cvx = vx; \
				cvy = vy; \
			} \
		} \
	} \
	float me = 1000.0; \
	for (int kk = 0; kk < 5; kk += 1) { \
		for (int ll = 0; ll < 5; ll += 1) { \
			float cx = ix + float(ll) - 2.0; \
			float cy = iy + float(kk) - 2.0; \
			int h = perlin_hash(perlin_wrap(cx, period), perlin_wrap(cy, period), 0.0, seed); \
			float vx = cx + 0.5 + randomness * (float(h & 1023) / 1023.0 - 0.5) - x; \
			float vy = cy + 0.5 + randomness * (float((h >> 10) & 1023) / 1023.0 - 0.5) - y; \
			float ex = vx - cvx; \
			float ey = vy - cvy; \
			float e2 = ex * ex + ey * ey; \
			if (e2 > 0.0001) { me = min(me, ((cvx + vx) * ex + (cvy + vy) * ey) * 0.5 / sqrt(e2)); } \
		} \
	} \
	return me; \
} \
float voronoi_gen_warp() { return 0.25; } \
float voronoi_gen_octave(float x, float y, float period, float randomness, int feature, int metric, float expo, float smoothness, int seed) { \
	if (feature == 5) { return clamp(voronoi_gen_edge(x, y, period, randomness, seed) * voronoi_gen_norm_border(), 0.0, 1.0); } \
	if (feature == 4) { return clamp(voronoi_gen_smooth(x, y, period, randomness, metric, expo, smoothness, seed) * voronoi_gen_norm_f1(metric, expo), 0.0, 1.0); } \
	float3 c = voronoi_gen_cells(x, y, period, randomness, metric, expo, seed); \
	if (feature == 2) { return c.z; } \
	if (feature == 1) { return 1.0 - clamp((c.y - c.x) * voronoi_gen_norm_edge(metric, expo), 0.0, 1.0); } \
	if (feature == 3) { return clamp(c.y * voronoi_gen_norm_f2(metric, expo), 0.0, 1.0); } \
	return clamp(c.x * voronoi_gen_norm_f1(metric, expo), 0.0, 1.0); \
} \
float voronoi_gen_layer(float3 q, float freq, float tileable, float randomness, int feature, int metric, float expo, float smoothness, int seed) { \
	float f = freq; \
	float period = 0.0; \
	if (tileable > 0.5) { \
		f = max(1.0, floor(freq + 0.5)); \
		period = f; \
	} \
	return voronoi_gen_octave(q.x * f, q.y * f, period, randomness, feature, metric, expo, smoothness, seed); \
} \
float voronoi_gen_factor(float3 p, float scale, float randomness, float distortion, float tileable, float seed, int feature, int metric, float expo, float smoothness, float detail, float roughness, float lacunarity, float norm_on) { \
	int sd = int(floor(seed)); \
	float se = perlin_base_scale(scale, tileable); \
	float3 q = p; \
	if (distortion != 0.0) { \
		q = p + distortion * voronoi_gen_warp() / se * float3(perlin_signed(p, se, tileable, sd + 101), perlin_signed(p, se, tileable, sd + 202), perlin_signed(p, se, tileable, sd + 303)); \
	} \
	float ex = clamp(expo, 1.0, 32.0); \
	float r = clamp(randomness, 0.0, 1.0); \
	float freq = se; \
	float amp = 1.0; \
	float maxamp = 0.0; \
	float sum = 0.0; \
	float dt = clamp(detail, 0.0, 8.0); \
	int n = int(dt); \
	for (int oo = 0; oo <= n; oo += 1) { \
		sum = sum + amp * voronoi_gen_layer(q, freq, tileable, r, feature, metric, ex, smoothness, sd + oo * 31); \
		maxamp = maxamp + amp; \
		amp = amp * roughness; \
		freq = freq * lacunarity; \
	} \
	float rmd = dt - floor(dt); \
	if (rmd > 0.0) { \
		float t = voronoi_gen_layer(q, freq, tileable, r, feature, metric, ex, smoothness, sd + (n + 1) * 31); \
		sum = lerp(sum, sum + t * amp, rmd); \
		maxamp = lerp(maxamp, maxamp + amp, rmd); \
	} \
	if (norm_on > 0.5) { return clamp(sum / maxamp, 0.0, 1.0); } \
	return clamp(sum, 0.0, 1.0); \
} \
float voronoi_gen_image(float u, float v, float scale, float randomness, float distortion, float tileable, float seed, int feature, int metric, float expo, float smoothness, float detail, float roughness, float lacunarity, float norm_on) { \
	float z = 0.3 + 0.4 * (seed - floor(seed)); \
	return voronoi_gen_factor(float3(u, v, z), scale, randomness, distortion, tileable, seed, feature, metric, expo, smoothness, detail, roughness, lacunarity, norm_on); \
} \
";

// Input socket indices (all in the Parameters section)
enum {
	ADVANCED_NOISE_INPUT_SCALE,
	ADVANCED_NOISE_INPUT_DETAIL,
	ADVANCED_NOISE_INPUT_ROUGHNESS,
	ADVANCED_NOISE_INPUT_LACUNARITY,
	ADVANCED_NOISE_INPUT_DISTORTION,
	ADVANCED_NOISE_INPUT_SEED,
	ADVANCED_NOISE_INPUT_RANDOMNESS, // Voronoi
	ADVANCED_NOISE_INPUT_EXPONENT,   // Voronoi, Metric Minkowski
	ADVANCED_NOISE_INPUT_SMOOTHNESS, // Voronoi, Feature Smooth F1
};

// Node button indices (settings): Size, the state row, then the General section
enum {
	ADVANCED_NOISE_BUTTON_SIZE = 0, // The state row is button 1
	ADVANCED_NOISE_BUTTON_MODE = 2,
	ADVANCED_NOISE_BUTTON_TILEABLE = 3,
	ADVANCED_NOISE_BUTTON_FEATURE = 4, // Voronoi
	ADVANCED_NOISE_BUTTON_METRIC = 5,  // Voronoi
	ADVANCED_NOISE_BUTTON_NORMALIZE = 6, // Voronoi, with Detail > 0
};

// One value per mode, in the order of the Mode list. The count is the last entry: a new mode goes before it.
enum {
	ADVANCED_NOISE_MODE_PERLIN,
	ADVANCED_NOISE_MODE_VORONOI,
	ADVANCED_NOISE_MODE_COUNT,
};

// The Feature list of the Voronoi mode and the values of voronoi_gen_factor (the order is the saved index: append)
enum {
	ADVANCED_NOISE_FEATURE_DISTANCE,
	ADVANCED_NOISE_FEATURE_EDGES,
	ADVANCED_NOISE_FEATURE_CELLS,
	ADVANCED_NOISE_FEATURE_CRYSTALS,
	ADVANCED_NOISE_FEATURE_SMOOTH_F1,
	ADVANCED_NOISE_FEATURE_DISTANCE_TO_EDGE,
};

// The Metric list of the Voronoi mode (the saved index: append)
enum {
	ADVANCED_NOISE_METRIC_EUCLIDEAN,
	ADVANCED_NOISE_METRIC_MANHATTAN,
	ADVANCED_NOISE_METRIC_CHEBYSHEV,
	ADVANCED_NOISE_METRIC_MINKOWSKI,
};

// A mode: its name in the Mode list, the rows it hides (bit n of a mask = input or setting n of the node) and the function
// that returns the shader expression (float, 0..1) of the pixel; it is written in the capture shader, tex_coord is the image uv.
typedef struct advanced_noise_mode {
	char *name;
	i32   hidden_inputs;
	i32   hidden_buttons;
	char *(*emit)(ui_node_t *node);
} advanced_noise_mode_t;

static advanced_noise_mode_t advanced_noise_modes[ADVANCED_NOISE_MODE_COUNT];

static void advanced_noise_mode_set(i32 mode, char *name, i32 hidden_inputs, i32 hidden_buttons, char *(*emit)(ui_node_t *node)) {
	advanced_noise_modes[mode].name           = name;
	advanced_noise_modes[mode].hidden_inputs  = hidden_inputs;
	advanced_noise_modes[mode].hidden_buttons = hidden_buttons;
	advanced_noise_modes[mode].emit           = emit;
}

static i32 advanced_noise_node_mode(ui_node_t *node) {
	i32 mode = (i32)make_image_node_button(node, ADVANCED_NOISE_BUTTON_MODE);
	return mode >= 0 && mode < ADVANCED_NOISE_MODE_COUNT ? mode : 0;
}

// The input as a shader value: a literal, or per pixel when linked
static char *advanced_noise_node_input(ui_node_t *node, i32 input) {
	if (input >= node->inputs->length) {
		return "0.0"; // A node saved with fewer inputs (make_image_node_reconcile completes it, this is the safety net)
	}
	return parser_material_parse_value_input(node->inputs->buffer[input], false);
}

// Perlin hides the rows of the Voronoi mode (Randomness, Feature, Metric).
static char *advanced_noise_perlin_emit(ui_node_t *node) {
	node_shader_add_function(parser_material_kong, str_advanced_noise_perlin);
	char *scale      = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_SCALE);
	char *detail     = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_DETAIL);
	char *roughness  = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_ROUGHNESS);
	char *lacunarity = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_LACUNARITY);
	char *distortion = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_DISTORTION);
	char *seed       = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_SEED);
	char *tileable   = make_image_node_button(node, ADVANCED_NOISE_BUTTON_TILEABLE) > 0.5 ? "1.0" : "0.0";
	return string_tmp("perlin_image(tex_coord.x, tex_coord.y, %s, %s, %s, %s, %s, %s, %s)", scale, detail, roughness, lacunarity, distortion, tileable, seed);
}

// Voronoi shows Scale, Randomness, Distortion, Seed, Tileable, Feature and Metric; Exponent with Minkowski, Smoothness with Smooth F1,
// Roughness, Lacunarity and Normalize with Detail > 0 (advanced_noise_voronoi_item_hidden); Distance to Edge has no Metric.
static char *advanced_noise_voronoi_emit(ui_node_t *node) {
	node_shader_add_function(parser_material_kong, str_advanced_noise_perlin); // The hash, the wrap and the distortion noise
	node_shader_add_function(parser_material_kong, str_advanced_noise_voronoi);
	char *scale      = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_SCALE);
	char *randomness = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_RANDOMNESS);
	char *distortion = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_DISTORTION);
	char *seed       = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_SEED);
	char *exponent   = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_EXPONENT);
	char *smoothness = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_SMOOTHNESS);
	char *detail     = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_DETAIL);
	char *roughness  = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_ROUGHNESS);
	char *lacunarity = advanced_noise_node_input(node, ADVANCED_NOISE_INPUT_LACUNARITY);
	char *normalize  = make_image_node_button(node, ADVANCED_NOISE_BUTTON_NORMALIZE) > 0.5 ? "1.0" : "0.0";
	char *tileable   = make_image_node_button(node, ADVANCED_NOISE_BUTTON_TILEABLE) > 0.5 ? "1.0" : "0.0";
	i32   feature    = (i32)make_image_node_button(node, ADVANCED_NOISE_BUTTON_FEATURE);
	i32   metric     = (i32)make_image_node_button(node, ADVANCED_NOISE_BUTTON_METRIC);
	return string_tmp("voronoi_gen_image(tex_coord.x, tex_coord.y, %s, %s, %s, %s, %s, %d, %d, %s, %s, %s, %s, %s, %s)", scale, randomness, distortion, tileable, seed, feature, metric, exponent, smoothness, detail, roughness, lacunarity, normalize);
}

static void advanced_noise_modes_init() {
	i32 perlin_hides_inputs   = (1 << ADVANCED_NOISE_INPUT_RANDOMNESS) | (1 << ADVANCED_NOISE_INPUT_EXPONENT) | (1 << ADVANCED_NOISE_INPUT_SMOOTHNESS);
	i32 perlin_hides_buttons  = (1 << ADVANCED_NOISE_BUTTON_FEATURE) | (1 << ADVANCED_NOISE_BUTTON_METRIC) | (1 << ADVANCED_NOISE_BUTTON_NORMALIZE);
	advanced_noise_mode_set(ADVANCED_NOISE_MODE_PERLIN, _tr("Perlin"), perlin_hides_inputs, perlin_hides_buttons, advanced_noise_perlin_emit);
	advanced_noise_mode_set(ADVANCED_NOISE_MODE_VORONOI, _tr("Voronoi"), 0, 0, advanced_noise_voronoi_emit); // The rows that depend on its settings: advanced_noise_voronoi_item_hidden
}

static bool advanced_noise_node_item_hidden_for_mode(i32 mode, ui_node_item_t kind, i32 index) {
	if (mode < 0 || mode >= ADVANCED_NOISE_MODE_COUNT || index < 0 || index > 30) {
		return false;
	}
	if (kind == UI_NODE_ITEM_OUTPUT) {
		return false; // One output
	}
	i32 mask = kind == UI_NODE_ITEM_INPUT ? advanced_noise_modes[mode].hidden_inputs : advanced_noise_modes[mode].hidden_buttons;
	return ((mask >> index) & 1) != 0;
}

// The rows of the Voronoi mode that depend on its settings: Exponent is only used by the Minkowski metric, Smoothness by Smooth F1,
// Distance to Edge is Euclidean only (Blender's definition: the perpendicular bisectors of the points), so it has no Metric and no Exponent.
// Roughness, Lacunarity and Normalize belong to the fractal Voronoi: shown when Detail is above 0 (or linked).
static bool advanced_noise_voronoi_item_hidden(i32 feature, i32 metric, bool fractal, ui_node_item_t kind, i32 index) {
	bool edge = feature == ADVANCED_NOISE_FEATURE_DISTANCE_TO_EDGE;
	if (kind == UI_NODE_ITEM_BUTTON) {
		return (index == ADVANCED_NOISE_BUTTON_METRIC && edge) || (index == ADVANCED_NOISE_BUTTON_NORMALIZE && !fractal);
	}
	if (kind != UI_NODE_ITEM_INPUT) {
		return false;
	}
	if (index == ADVANCED_NOISE_INPUT_ROUGHNESS || index == ADVANCED_NOISE_INPUT_LACUNARITY) {
		return !fractal;
	}
	if (index == ADVANCED_NOISE_INPUT_EXPONENT) {
		return metric != ADVANCED_NOISE_METRIC_MINKOWSKI || edge;
	}
	return index == ADVANCED_NOISE_INPUT_SMOOTHNESS && feature != ADVANCED_NOISE_FEATURE_SMOOTH_F1;
}

// True when a link ends at the input (a linked Detail can be anything per pixel: the fractal rows stay shown)
static bool advanced_noise_node_input_linked(ui_node_t *node, i32 input) {
	ui_node_canvas_t *canvas = ui_nodes_get_canvas(true);
	if (canvas == NULL || canvas->links == NULL) {
		return false;
	}
	for (i32 i = 0; i < canvas->links->length; ++i) {
		ui_node_link_t *link = canvas->links->buffer[i];
		if (link->to_id == node->id && link->to_socket == input) {
			return true;
		}
	}
	return false;
}

static bool advanced_noise_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	i32 mode = advanced_noise_node_mode(node);
	if (advanced_noise_node_item_hidden_for_mode(mode, kind, index)) {
		return true;
	}
	if (mode != ADVANCED_NOISE_MODE_VORONOI) {
		return false;
	}
	bool fractal = advanced_noise_node_input_linked(node, ADVANCED_NOISE_INPUT_DETAIL) ||
	               (ADVANCED_NOISE_INPUT_DETAIL < node->inputs->length && node->inputs->buffer[ADVANCED_NOISE_INPUT_DETAIL]->default_value->buffer[0] > 0.0);
	return advanced_noise_voronoi_item_hidden((i32)make_image_node_button(node, ADVANCED_NOISE_BUTTON_FEATURE), (i32)make_image_node_button(node, ADVANCED_NOISE_BUTTON_METRIC), fractal, kind, index);
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 advanced_noise_node_size(ui_node_t *node) {
	return make_image_node_size(node, ADVANCED_NOISE_BUTTON_SIZE);
}

// The capture is the generator: the noise of the mode per pixel, float, the same value in R, G and B
static char *advanced_noise_node_capture(ui_node_t *node) {
	char *value = advanced_noise_modes[advanced_noise_node_mode(node)].emit(node);
	char *var   = string_tmp("%s_noise", parser_material_store_var_name(node));
	parser_material_write(parser_material_kong, string_tmp("float %s = %s;", var, value));
	return string_tmp("float4(%s, %s, %s, 1.0)", var, var, var);
}

// Everything is in the shader (and so in the signature, which hashes the shader text): no processing parameters
static void advanced_noise_node_params(ui_node_t *node, void *params) {
}

// GPU baked, result by reference: the render target the capture shader draws into is the result (no readback, no copy, no upload).
// Float, a Bump node or a blur reads the slopes of the noise, half floats would band.
static image_node_type_t advanced_noise_node_image_type = {
    .type                = "TEX_ADVANCED_NOISE",
    .result_format       = GPU_TEXTURE_FORMAT_RGBA128,
    .result_by_reference = true,
    .params_size         = 0,
    .size                = advanced_noise_node_size,
    .capture             = advanced_noise_node_capture,
    .smooth_sample       = true,
    .params              = advanced_noise_node_params,
    .process             = NULL,
};

char *advanced_noise_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	char *v = string_tmp("%s.r", make_image_node_sample(node));
	return string_tmp("float3(%s, %s, %s)", v, v, v);
}

char *advanced_noise_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

void advanced_noise_node_init() {
	advanced_noise_modes_init();
	char *mode_items = "";
	for (i32 i = 0; i < ADVANCED_NOISE_MODE_COUNT; ++i) {
		mode_items = i == 0 ? string_tmp("%s", advanced_noise_modes[i].name) : string_tmp("%s\n%s", mode_items, advanced_noise_modes[i].name);
	}
	char *feature_items = string_tmp("%s\n%s\n%s\n%s\n%s\n%s", _tr("Distance"), _tr("Edges"), _tr("Cells"), _tr("Crystals"), _tr("Smooth F1"), _tr("Distance to Edge"));
	char *metric_items  = string_tmp("%s\n%s\n%s\n%s", _tr("Euclidean"), _tr("Manhattan"), _tr("Chebyshev"), _tr("Minkowski"));

	ui_node_t *advanced_noise_node_def =
	    ALLOC_INIT(ui_node_t,
	               {.id     = 0,
	                .name   = _tr("Advanced Noise"),
	                .type   = "TEX_ADVANCED_NOISE",
	                .x      = 0,
	                .y      = 0,
	                .color  = NODES_MATERIAL_IMAGE_COLOR,
	                .inputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Scale"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(5.0), 0.0, 256.0, 100),
	                        make_image_node_socket(_tr("Detail"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.0), 0.0, 8.0, 100),
	                        make_image_node_socket(_tr("Roughness"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.5), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Lacunarity"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(2.0), 0.0, 10.0, 100),
	                        make_image_node_socket(_tr("Distortion"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.0), -10.0, 10.0, 100),
	                        make_image_node_socket(_tr("Seed"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.0), 0.0, 1000.0, 100),
	                        make_image_node_socket(_tr("Randomness"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                        make_image_node_socket(_tr("Exponent"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(2.0), 1.0, 32.0, 100),
	                        make_image_node_socket(_tr("Smoothness"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                    },
	                    9),
	                .outputs = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                    },
	                    1),
	                .buttons = any_array_create_from_raw(
	                    (void *[]){
	                        make_image_node_setting(_tr("Size"), "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS),
	                        make_image_node_state_button(),
	                        make_image_node_setting(_tr("Mode"), "ENUM", 0, mode_items),
	                        make_image_node_setting(_tr("Tileable"), "BOOL", 1, NULL),
	                        make_image_node_setting(_tr("Feature"), "ENUM", 0, feature_items),
	                        make_image_node_setting(_tr("Metric"), "ENUM", 0, metric_items),
	                        make_image_node_setting(_tr("Normalize"), "BOOL", 1, NULL),
	                    },
	                    7),
	                .width = 0,
	                .flags = 0});

	any_array_push(nodes_material_pattern, advanced_noise_node_def);
	any_map_set(parser_material_node_vectors, "TEX_ADVANCED_NOISE", advanced_noise_node_vector);
	any_map_set(parser_material_node_values, "TEX_ADVANCED_NOISE", advanced_noise_node_value);
	any_map_set(ui_nodes_hidden_rules, "TEX_ADVANCED_NOISE", advanced_noise_node_item_hidden);
	make_image_node_sections(advanced_noise_node_def, NULL, 0, 0); // General: Mode and Tileable; Parameters: all inputs
	make_image_node_register(&advanced_noise_node_image_type);
}
