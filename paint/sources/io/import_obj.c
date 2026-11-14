
#include "../global.h"

void import_obj_run(char *path, bool replace_existing) {
	buffer_t *b = data_get_blob(path);
	import_obj_parse(b, replace_existing);
	data_delete_blob(path);
}

void import_obj_parse(buffer_t *b, bool replace_existing) {
	i32  split_code = g_context->split_by == SPLIT_TYPE_OBJECT ? 'o' : 'u'; // usemtl
	bool keep_uvs   = g_context->udim_import != UDIM_IMPORT_OFF;            // Tiles are decided once every part is read

	raw_mesh_t_array_t *parts     = any_array_create_from_raw((void *[]){}, 0);
	raw_mesh_t_array_t *discarded = any_array_create_from_raw((void *[]){}, 0);
	raw_mesh_t         *part      = obj_parse(b, split_code, 0, keep_uvs);
	any_array_push(parts, part);
	while (part->has_next) {
		part = obj_parse(b, split_code, part->pos, keep_uvs);
		// This part does not contain faces (may contain lines only)
		if (part->inda->length == 0) {
			any_array_push(discarded, part);
			continue;
		}
		any_array_push(parts, part);
	}
	if (keep_uvs) {
		// Udim tiles over the whole file, then uvs and welding as obj_parse does without keep_uvs
		util_udim_scan_begin();
		for (i32 i = 0; i < parts->length; ++i) {
			f32_array_t *uva = parts->buffer[i]->uva;
			util_udim_scan(uva != NULL ? uva->buffer : NULL, uva != NULL ? uva->length / 2 : 0);
		}
		bool tiles = util_udim_scan_end();
		for (i32 i = 0; i < parts->length; ++i) {
			raw_mesh_t  *p   = parts->buffer[i];
			f32_array_t *uva = p->uva;
			util_udim_split_raw(p, uva != NULL ? uva->buffer : NULL, uva != NULL ? uva->length / 2 : 0, tiles);
			array_delete(uva);
			p->uva = NULL;
			obj_weld(p);
		}
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
		i16_array_t *tilea0;
		i16_array_t *tilea1;
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
					tilea0   = parts->buffer[i]->tilea;
					tilea1   = parts->buffer[j]->tilea;
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

					// Udim tiles, a part with none is in 1001
					i16_array_t *tilea = NULL;
					if (texa != NULL && (tilea0 != NULL || tilea1 != NULL)) {
						i32 vc0 = texa0->length / 2;
						tilea   = i16_array_create(vc0 + texa1->length / 2);
						for (i32 k = 0; tilea0 != NULL && k < tilea0->length; ++k) {
							tilea->buffer[k] = tilea0->buffer[k];
						}
						for (i32 k = 0; tilea1 != NULL && k < tilea1->length; ++k) {
							tilea->buffer[k + vc0] = tilea1->buffer[k];
						}
					}
					array_delete(tilea0);
					array_delete(tilea1);

					for (i32 k = 0; k < inda0->length; ++k) {
						inda->buffer[k] = inda0->buffer[k];
					}
					for (i32 k = 0; k < inda1->length; ++k) {
						inda->buffer[k + inda0->length] = inda1->buffer[k] + voff;
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
					parts->buffer[i]->tilea     = tilea;
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

	replace_existing ? import_mesh_make_mesh(parts->buffer[0]) : import_mesh_add_mesh(parts->buffer[0]);
	for (i32 i = 1; i < parts->length; ++i) {
		import_mesh_add_mesh(parts->buffer[i]);
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
		array_delete(d->uva);
		array_delete(d->tilea);
		free(d->name);
		free(d);
	}
	array_free(discarded);
	free(discarded);
}
