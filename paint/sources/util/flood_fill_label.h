#pragma once

// Connected island labelling for the Flood Fill material node.
// Plain C, standard library only, so it compiles and tests on its own.

#include <stdbool.h>
#include <stdint.h>

typedef struct flood_fill_params {
	int32_t  width;
	int32_t  height;
	bool     connectivity8; // Diagonal neighbours join an island too
	bool     wrap;          // Islands continue across the texture borders
	uint32_t seed;          // Changes the ID greys
	int32_t  min_size;      // Islands with fewer pixels are dropped
} flood_fill_params_t;

// inside: width * height bytes, non zero marks a pixel inside the mask.
// out: width * height * 4 floats, written per pixel as ID, Position x, Position y, Area.
// Pixels outside the mask or in a dropped island get all four zero; Area is above zero in every kept island.
// Returns the number of kept islands, or -1 when memory runs out (out is then all zero).
int32_t flood_fill_label(const uint8_t *inside, const flood_fill_params_t *p, float *out);
