#pragma once

#include "iron_armpack.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct raw_mesh {
	struct i16_array *posa;
	struct i16_array *nora;
	struct i16_array *texa;
	struct i16_array *texa1;
	struct i16_array *cola;
	struct u32_array *inda;
	int               vertex_count;
	int               index_count;
	float             scale_pos;
	float             scale_tex;
	char             *name;
	bool              has_next; // File contains multiple objects
	uint64_t          pos;
	struct i16_array *tilea;         // Per vertex udim tile index (id - 1001), NULL = every vertex in 1001
	struct f32_array *uva;           // obj_parse with keep_uvs: per corner uvs (udim space), texa is not written and vertices are not welded yet
	void             *vertex_arrays; // vertex_array_t[]
	struct u32_array *index_array;
	struct u8_array  *blob; // Source file bytes
} raw_mesh_t;

extern bool obj_parse_y_to_z_up;

raw_mesh_t *obj_parse(buffer_t *file_bytes, char split_code, uint64_t start_pos, bool keep_uvs);
void        obj_weld(raw_mesh_t *part);
void        obj_destroy(raw_mesh_t *part);
