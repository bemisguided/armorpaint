
#include "../global.h"

void import_obj_run(char *path, bool replace_existing) {
	buffer_t *b = data_get_blob(path);
	import_obj_parse(b, replace_existing);
	data_delete_blob(path);
}

// Import one part split by uv tile (part->udims), each tile becomes '<name>.<1001+...>'
// Frees part->udims, the full index array and the original name, the caller frees the part struct
static void import_obj_add_tiles(raw_mesh_t *part, bool *replace_existing) {
	char        *name      = part->name;
	u32_array_t *inda_full = part->inda;
	bool         first     = true;
	for (i32 i = 0; i < part->udims->length; ++i) {
		u32_array_t *a = part->udims->buffer[i];
		if (a->length == 0) {
			array_free(a);
			free(a);
			continue;
		}
		// Tile index is relative to the grid of this part
		i32 u      = i % part->udims_u;
		i32 v      = math_floor(i / (float)part->udims_u);
		i32 id     = (1000 + v * 10 + u + 1);
		part->name = string("%s.%s", name, i32_to_string(id));
		part->inda = a;
		if (!first) {
			part->posa = i16_array_create_from_array(part->posa);
			part->nora = i16_array_create_from_array(part->nora);
			if (part->texa != NULL) {
				part->texa = i16_array_create_from_array(part->texa);
			}
		}
		first = false;
		if (*replace_existing) {
			*replace_existing = false;
			import_mesh_make_mesh(part);
		}
		else {
			import_mesh_add_mesh(part);
		}
	}
	array_free(inda_full);
	free(inda_full);
	array_free(part->udims);
	free(part->udims);
	part->udims = NULL;
	free(name);
}

static void import_obj_free_udims(raw_mesh_t *p) {
	if (p->udims == NULL) {
		return;
	}
	for (i32 k = 0; k < p->udims->length; ++k) {
		array_free(p->udims->buffer[k]);
		free(p->udims->buffer[k]);
	}
	array_free(p->udims);
	free(p->udims);
	p->udims = NULL;
}

// Merge tile index arrays of part b into part a, b vertices are placed after voff vertices of a
static void import_obj_merge_udims(raw_mesh_t *a, raw_mesh_t *b, i32 voff) {
	i32          nu  = a->udims_u > b->udims_u ? a->udims_u : b->udims_u;
	i32          nv  = a->udims_v > b->udims_v ? a->udims_v : b->udims_v;
	any_array_t *res = any_array_create(nu * nv);
	for (i32 v = 0; v < nv; ++v) {
		for (i32 u = 0; u < nu; ++u) {
			u32_array_t *ta = (u < a->udims_u && v < a->udims_v) ? a->udims->buffer[v * a->udims_u + u] : NULL;
			u32_array_t *tb = (u < b->udims_u && v < b->udims_v) ? b->udims->buffer[v * b->udims_u + u] : NULL;
			i32          la = ta != NULL ? ta->length : 0;
			i32          lb = tb != NULL ? tb->length : 0;
			u32_array_t *t  = u32_array_create(la + lb);
			for (i32 k = 0; k < la; ++k) {
				t->buffer[k] = ta->buffer[k];
			}
			for (i32 k = 0; k < lb; ++k) {
				t->buffer[la + k] = tb->buffer[k] + voff;
			}
			res->buffer[v * nu + u] = t;
		}
	}
	for (i32 k = 0; k < a->udims->length; ++k) {
		array_free(a->udims->buffer[k]);
		free(a->udims->buffer[k]);
	}
	array_free(a->udims);
	free(a->udims);
	for (i32 k = 0; k < b->udims->length; ++k) {
		array_free(b->udims->buffer[k]);
		free(b->udims->buffer[k]);
	}
	array_free(b->udims);
	free(b->udims);
	a->udims   = res;
	a->udims_u = nu;
	a->udims_v = nv;
}

void import_obj_parse(buffer_t *b, bool replace_existing) {
	split_type_t i          = g_context->split_by;
	bool         is_udim    = i == SPLIT_TYPE_UDIM;
	bool         split_udim = g_context->split_udim && !is_udim; // Object or material parts additionally split per tile
	i32          split_code = (i == SPLIT_TYPE_OBJECT || is_udim) ? 'o' : 'u'; // usemtl

	if (is_udim) {
		raw_mesh_t *part = obj_parse(b, split_code, 0, is_udim);
		if (part->udims != NULL) {
			import_obj_add_tiles(part, &replace_existing);
			free(part);
		}
	}
	else {
		raw_mesh_t_array_t *parts     = any_array_create_from_raw((void *[]){}, 0);
		raw_mesh_t_array_t *discarded = any_array_create_from_raw((void *[]){}, 0);
		raw_mesh_t         *part      = obj_parse_split(b, split_code, 0, split_udim, split_udim);
		any_array_push(parts, part);
		while (part->has_next) {
			part = obj_parse_split(b, split_code, part->pos, split_udim, split_udim);
			// This part does not contain faces (may contain lines only)
			if (part->inda->length == 0) {
				any_array_push(discarded, part);
				continue;
			}
			any_array_push(parts, part);
		}
		if (g_context->split_by == SPLIT_TYPE_MATERIAL) {
			i16_array_t *posa0;
			i16_array_t *posa1;
			i16_array_t *nora0;
			i16_array_t *nora1;
			i16_array_t *texa0;
			i16_array_t *texa1;
			u32_array_t *inda0;
			u32_array_t *inda1;
			// Merge to single object per material
			for (i32 i = 0; i < parts->length; ++i) {
				i32 j = i + 1;
				while (j < parts->length) {
					char *iname = parts->buffer[i]->name;
					char *jname = parts->buffer[j]->name;
					if (string_equals(iname, jname)) {
						posa0    = parts->buffer[i]->posa;
						posa1    = parts->buffer[j]->posa;
						nora0    = parts->buffer[i]->nora;
						nora1    = parts->buffer[j]->nora;
						texa0    = parts->buffer[i]->texa != NULL ? parts->buffer[i]->texa : NULL;
						texa1    = parts->buffer[j]->texa != NULL ? parts->buffer[j]->texa : NULL;
						inda0    = parts->buffer[i]->inda;
						inda1    = parts->buffer[j]->inda;
						i32 voff = math_floor(posa0->length / 4.0);
						// Repack merged positions
						f32_array_t *posa32 = f32_array_create(math_floor(posa0->length / 4.0) * 3 + math_floor(posa1->length / 4.0) * 3);
						for (i32 k = 0; k < math_floor(posa0->length / 4.0); ++k) {
							posa32->buffer[k * 3]     = posa0->buffer[k * 4] / 32767.0 * parts->buffer[i]->scale_pos;
							posa32->buffer[k * 3 + 1] = posa0->buffer[k * 4 + 1] / 32767.0 * parts->buffer[i]->scale_pos;
							posa32->buffer[k * 3 + 2] = posa0->buffer[k * 4 + 2] / 32767.0 * parts->buffer[i]->scale_pos;
						}
						for (i32 k = 0; k < math_floor(posa1->length / 4.0); ++k) {
							posa32->buffer[voff * 3 + k * 3]     = posa1->buffer[k * 4] / 32767.0 * parts->buffer[j]->scale_pos;
							posa32->buffer[voff * 3 + k * 3 + 1] = posa1->buffer[k * 4 + 1] / 32767.0 * parts->buffer[j]->scale_pos;
							posa32->buffer[voff * 3 + k * 3 + 2] = posa1->buffer[k * 4 + 2] / 32767.0 * parts->buffer[j]->scale_pos;
						}
						f32 scale_pos = 0.0;
						for (i32 k = 0; k < posa32->length; ++k) {
							f32 f = math_abs(posa32->buffer[k]);
							if (scale_pos < f) {
								scale_pos = f;
							}
						}
						f32          inv  = 32767 * (1 / (float)scale_pos);
						i16_array_t *posa = i16_array_create(posa0->length + posa1->length);
						for (i32 k = 0; k < math_floor(posa->length / 4.0); ++k) {
							posa->buffer[k * 4]     = math_floor(posa32->buffer[k * 3] * inv);
							posa->buffer[k * 4 + 1] = math_floor(posa32->buffer[k * 3 + 1] * inv);
							posa->buffer[k * 4 + 2] = math_floor(posa32->buffer[k * 3 + 2] * inv);
						}
						for (i32 k = 0; k < math_floor(posa0->length / 4.0); ++k) {
							posa->buffer[k * 4 + 3] = posa0->buffer[k * 4 + 3];
						}
						for (i32 k = 0; k < math_floor(posa1->length / 4.0); ++k) {
							posa->buffer[posa0->length + k * 4 + 3] = posa1->buffer[k * 4 + 3];
						}
						// Merge normals and uvs
						i16_array_t *nora = i16_array_create(nora0->length + nora1->length);
						i16_array_t *texa = (texa0 != NULL && texa1 != NULL) ? i16_array_create(texa0->length + texa1->length) : NULL;
						u32_array_t *inda = u32_array_create(inda0->length + inda1->length);

						for (i32 k = 0; k < nora0->length; ++k) {
							nora->buffer[k] = nora0->buffer[k];
						}
						for (i32 k = 0; k < nora1->length; ++k) {
							nora->buffer[k + nora0->length] = nora1->buffer[k];
						}

						if (texa != NULL) {
							for (i32 k = 0; k < texa0->length; ++k) {
								texa->buffer[k] = texa0->buffer[k];
							}
							for (i32 k = 0; k < texa1->length; ++k) {
								texa->buffer[k + texa0->length] = texa1->buffer[k];
							}
						}

						for (i32 k = 0; k < inda0->length; ++k) {
							inda->buffer[k] = inda0->buffer[k];
						}
						for (i32 k = 0; k < inda1->length; ++k) {
							inda->buffer[k + inda0->length] = inda1->buffer[k] + voff;
						}

						// Merge uv tiles, parts without uvs have no tile split
						if (parts->buffer[i]->udims != NULL && parts->buffer[j]->udims != NULL && texa != NULL) {
							import_obj_merge_udims(parts->buffer[i], parts->buffer[j], voff);
						}
						else {
							import_obj_free_udims(parts->buffer[i]);
							import_obj_free_udims(parts->buffer[j]);
						}

						array_free(posa32);
						free(posa32);
						array_free(posa0);
						free(posa0);
						array_free(nora0);
						free(nora0);
						array_free(inda0);
						free(inda0);
						if (texa0 != NULL) {
							array_free(texa0);
							free(texa0);
						}
						array_free(posa1);
						free(posa1);
						array_free(nora1);
						free(nora1);
						array_free(inda1);
						free(inda1);
						if (texa1 != NULL) {
							array_free(texa1);
							free(texa1);
						}
						raw_mesh_t *jpart = parts->buffer[j];
						free(jpart->name);
						free(jpart);

						parts->buffer[i]->posa      = posa;
						parts->buffer[i]->nora      = nora;
						parts->buffer[i]->texa      = texa;
						parts->buffer[i]->inda      = inda;
						parts->buffer[i]->scale_pos = scale_pos;
						array_splice(parts, j, 1);
					}
					else {
						j++;
					}
				}
			}
		}

		for (i32 i = 0; i < parts->length; ++i) {
			raw_mesh_t *p = parts->buffer[i];
			if (p->udims != NULL) {
				// Object without any faces in a tile still ends up here with a single tile, e.g. '.1001'
				import_obj_add_tiles(p, &replace_existing);
			}
			else if (replace_existing) {
				replace_existing = false;
				import_mesh_make_mesh(p);
			}
			else {
				import_mesh_add_mesh(p);
			}
		}

		for (i32 i = 0; i < parts->length; ++i) {
			free(parts->buffer[i]);
		}
		array_free(parts);
		free(parts);
		for (i32 i = 0; i < discarded->length; ++i) {
			raw_mesh_t *d = discarded->buffer[i];
			array_free(d->posa);
			free(d->posa);
			array_free(d->nora);
			free(d->nora);
			if (d->texa != NULL) {
				array_free(d->texa);
				free(d->texa);
			}
			array_free(d->inda);
			free(d->inda);
			free(d->name);
			free(d);
		}
		array_free(discarded);
		free(discarded);
	}
}
