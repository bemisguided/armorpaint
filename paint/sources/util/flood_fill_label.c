#include "flood_fill_label.h"

#include <stdlib.h>
#include <string.h>

// Breadth first fill per island, islands numbered in scanline order of their first pixel.
// With wrap, every pixel also keeps its unwrapped coordinate (x + wrap_x * width) so the bounding
// box and the Position gradient run on across the border instead of splitting the island in two.

typedef struct flood_fill_island {
	int32_t count;
	int32_t min_x;
	int32_t max_x;
	int32_t min_y;
	int32_t max_y;
} flood_fill_island_t;

// lowbias32 integer hash, island index and seed to a grey in 0..1
static float flood_fill_hash01(uint32_t i, uint32_t seed) {
	uint32_t x = i * 0x9e3779b9u ^ (seed + 0x7f4a7c15u) * 0x85ebca6bu;
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return (float)(x >> 8) / 16777215.0f;
}

int32_t flood_fill_label(const uint8_t *inside, const flood_fill_params_t *p, float *out) {
	int32_t w = p->width;
	int32_t h = p->height;
	size_t  n = (size_t)w * h;
	memset(out, 0, n * 4 * sizeof(float));
	if (n == 0) {
		return 0;
	}

	int32_t             *labels  = malloc(n * sizeof(int32_t));
	int32_t             *queue   = malloc(n * sizeof(int32_t));
	int16_t             *wrap_x  = p->wrap ? malloc(n * sizeof(int16_t)) : NULL;
	int16_t             *wrap_y  = p->wrap ? malloc(n * sizeof(int16_t)) : NULL;
	int32_t              cap     = 256;
	int32_t              count   = 0;
	flood_fill_island_t *islands = malloc(cap * sizeof(flood_fill_island_t));
	if (labels == NULL || queue == NULL || islands == NULL || (p->wrap && (wrap_x == NULL || wrap_y == NULL))) {
		free(labels);
		free(queue);
		free(wrap_x);
		free(wrap_y);
		free(islands);
		return -1;
	}
	for (size_t i = 0; i < n; ++i) {
		labels[i] = -1;
	}

	static const int32_t dx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
	static const int32_t dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
	int32_t              dirs  = p->connectivity8 ? 8 : 4;

	// Pass 1: label and measure every island
	for (size_t start = 0; start < n; ++start) {
		if (!inside[start] || labels[start] >= 0) {
			continue;
		}
		if (count == cap) {
			cap *= 2;
			flood_fill_island_t *grown = realloc(islands, cap * sizeof(flood_fill_island_t));
			if (grown == NULL) {
				free(labels);
				free(queue);
				free(wrap_x);
				free(wrap_y);
				free(islands);
				memset(out, 0, n * 4 * sizeof(float));
				return -1;
			}
			islands = grown;
		}
		int32_t              id = count++;
		flood_fill_island_t *is = &islands[id];
		int32_t              sx = (int32_t)(start % w);
		int32_t              sy = (int32_t)(start / w);
		*is                     = (flood_fill_island_t){.count = 0, .min_x = sx, .max_x = sx, .min_y = sy, .max_y = sy};
		labels[start]           = id;
		if (wrap_x != NULL) {
			wrap_x[start] = 0;
			wrap_y[start] = 0;
		}
		size_t head   = 0;
		size_t tail   = 0;
		queue[tail++] = (int32_t)start;
		while (head < tail) {
			int32_t i  = queue[head++];
			int32_t x  = i % w;
			int32_t y  = i / w;
			int32_t ux = wrap_x != NULL ? x + wrap_x[i] * w : x; // Unwrapped
			int32_t uy = wrap_y != NULL ? y + wrap_y[i] * h : y;
			is->count++;
			is->min_x = ux < is->min_x ? ux : is->min_x;
			is->max_x = ux > is->max_x ? ux : is->max_x;
			is->min_y = uy < is->min_y ? uy : is->min_y;
			is->max_y = uy > is->max_y ? uy : is->max_y;
			for (int32_t d = 0; d < dirs; ++d) {
				int32_t nx = x + dx[d];
				int32_t ny = y + dy[d];
				int32_t kx = 0;
				int32_t ky = 0;
				if (nx < 0 || nx >= w || ny < 0 || ny >= h) {
					if (!p->wrap) {
						continue;
					}
					kx = nx < 0 ? -1 : nx >= w ? 1 : 0;
					ky = ny < 0 ? -1 : ny >= h ? 1 : 0;
					nx -= kx * w;
					ny -= ky * h;
				}
				int32_t j = ny * w + nx;
				if (!inside[j] || labels[j] >= 0) {
					continue;
				}
				labels[j] = id;
				if (wrap_x != NULL) {
					wrap_x[j] = (int16_t)(wrap_x[i] + kx);
					wrap_y[j] = (int16_t)(wrap_y[i] + ky);
				}
				queue[tail++] = j;
			}
		}
	}

	// Pass 2: drop small islands, normalise area to the largest kept one, write the outputs
	int32_t largest = 0;
	int32_t kept    = 0;
	for (int32_t i = 0; i < count; ++i) {
		if (islands[i].count >= p->min_size) {
			kept++;
			if (islands[i].count > largest) {
				largest = islands[i].count;
			}
		}
	}
	for (size_t i = 0; i < n; ++i) {
		int32_t id = labels[i];
		if (id < 0) {
			continue;
		}
		flood_fill_island_t *is = &islands[id];
		if (is->count < p->min_size) {
			continue;
		}
		int32_t ux = (int32_t)(i % w) + (wrap_x != NULL ? wrap_x[i] * w : 0);
		int32_t uy = (int32_t)(i / w) + (wrap_y != NULL ? wrap_y[i] * h : 0);
		float  *o  = &out[i * 4];
		o[0]       = flood_fill_hash01((uint32_t)id, p->seed);
		o[1]       = (ux - is->min_x + 0.5f) / (float)(is->max_x - is->min_x + 1);
		o[2]       = (uy - is->min_y + 0.5f) / (float)(is->max_y - is->min_y + 1);
		o[3]       = (float)is->count / (float)largest;
	}

	free(labels);
	free(queue);
	free(wrap_x);
	free(wrap_y);
	free(islands);
	return kept;
}
