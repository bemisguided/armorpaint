
#include "../global.h"

static f32             export_texture_gamma       = 1.0 / 2.2;
static string_array_t *export_texture_baked_files = NULL;

static i32  _export_texture_channel(i32 c, i32 bits);
static f32  _export_texture_get_value(buffer_t *b, i32 pixel, i32 channel, i32 bits);
static void export_texture_bake_material_add_texture(char *file);

// RGBA, RGB1, RRR1, GGG1, BBB1, AAA1 from native order pixels to rgba8
static buffer_t *export_texture_pack_pixels(buffer_t *pixels, i32 count, i32 bits, i32 format) {
	buffer_t *rgba = buffer_create(count * 4);
	i32       ch[4];
	for (i32 c = 0; c < 4; ++c) {
		ch[c] = format >= 3 ? _export_texture_channel(format - 3, bits) : _export_texture_channel(c, bits);
	}
	for (i32 i = 0; i < count; ++i) {
		for (i32 c = 0; c < 4; ++c) {
			f32 v = (format != 0 && c == 3) ? 1.0 : _export_texture_get_value(pixels, i, ch[c], bits);
			v     = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
			buffer_set_u8(rgba, i * 4 + c, (u8)math_floor(v * 255.0 + 0.5));
		}
	}
	return rgba;
}

// Size of the textures being written, the atlas or one udim tile cropped out of it
static i32 export_texture_size_x = 0;
static i32 export_texture_size_y = 0;

static void export_texture_write_texture(char *file, buffer_t *pixels, i32 type, i32 off) {
	i32 res_x       = export_texture_size_x;
	i32 res_y       = export_texture_size_y;
	i32 bits_handle = base_bits;
	i32 bits        = bits_handle == TEXTURE_BITS_BITS8 ? 8 : bits_handle == TEXTURE_BITS_BITS16 ? 16 : 32;
	i32 format      = 0; // RGBA
	if (type == 1) {
		format = 2; // RGB1
	}
	if (type == 2 && off == 0) {
		format = 3; // RRR1
	}
	if (type == 2 && off == 1) {
		format = 4; // GGG1
	}
	if (type == 2 && off == 2) {
		format = 5; // BBB1
	}
	if (type == 2 && off == 3) {
		format = 6; // AAA1
	}

	if (g_context->layers_destination == EXPORT_DESTINATION_PACK_INTO_PROJECT) {
		// Packed textures are rgba8, apply the channel layout the file writers do
		buffer_t      *rgba  = export_texture_pack_pixels(pixels, res_x * res_y, bits, format);
		gpu_texture_t *image = gpu_create_texture_from_bytes(rgba, res_x, res_y, GPU_TEXTURE_FORMAT_RGBA32);
		array_free(rgba);
		free(rgba);
		any_map_set(data_cached_textures, file, image);

		// Baking again replaces the packed asset of the same file
		asset_t *asset = NULL;
		for (i32 i = 0; i < g_project->_->assets->length; ++i) {
			if (string_equals(g_project->_->assets->buffer[i]->file, file)) {
				asset = g_project->_->assets->buffer[i];
				break;
			}
		}
		if (asset == NULL) {
			string_array_t *ar   = string_split(file, PATH_SEP);
			char           *name = ar->buffer[ar->length - 1];
			for (i32 i = 0; i < (i32)ar->length - 1; ++i) {
				free(ar->buffer[i]);
			}
			array_free(ar);
			free(ar);
			asset = ALLOC_INIT(asset_t, {.name = name, .file = file, .id = g_project->_->next_asset_id++});
			any_array_push(g_project->_->assets, asset);
			if (g_project->assets == NULL) {
				g_project->assets = any_array_create_from_raw((void *[]){}, 0);
			}
			any_array_push(g_project->assets, asset->file);
		}
		else if (g_project->packed_assets != NULL) {
			for (i32 i = 0; i < g_project->packed_assets->length; ++i) {
				if (string_equals(g_project->packed_assets->buffer[i]->name, file)) {
					array_splice(g_project->packed_assets, i, 1);
					break;
				}
			}
		}
		asset->image            = image;
		asset_t_array_t *assets = any_array_create_from_raw(
		    (void *[]){
		        asset,
		    },
		    1);
		export_arm_pack_assets(g_project, assets);
		export_texture_bake_material_add_texture(file);
		return;
	}

	if (bits == 8 && g_context->format_type == TEXTURE_LDR_FORMAT_PNG) {
		iron_write_png(file, pixels, res_x, res_y, format);
	}
	else if (bits == 8 && g_context->format_type == TEXTURE_LDR_FORMAT_JPG) {
		iron_write_jpg(file, pixels, res_x, res_y, format, math_floor(g_context->format_quality));
	}
	else { // Exr
		buffer_t *b = export_exr_run(res_x, res_y, pixels, bits, type, off);
		iron_file_save_bytes(file, b, b->length);
		array_free(b);
		free(b);
	}
	export_texture_bake_material_add_texture(file);
}

static i32 _export_texture_channel(i32 c, i32 bits) {
#ifdef IRON_BGRA
	if (bits == 8) {
		return c == 0 ? 2 : c == 2 ? 0 : c;
	}
#endif
	return c;
}

static i32 _export_texture_pixel_count(buffer_t *b, i32 bits) {
	return b->length / (4 * (bits / 8));
}

static f32 _export_texture_get_value(buffer_t *b, i32 pixel, i32 channel, i32 bits) {
	i32 pos = pixel * 4 + channel;
	if (bits == 8) {
		return buffer_get_u8(b, pos) / 255.0;
	}
	if (bits == 16) {
		return buffer_get_f16(b, pos * 2);
	}
	return buffer_get_f32(b, pos * 4);
}

static void _export_texture_set_value(buffer_t *b, i32 pixel, i32 channel, i32 bits, f32 value) {
	i32 pos = pixel * 4 + channel;
	if (bits == 8) {
		value = value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value;
		buffer_set_u8(b, pos, (u8)math_floor(value * 255.0 + 0.5));
	}
	else if (bits == 16) {
		buffer_set_u16(b, pos * 2, float_to_half_fast(value));
	}
	else {
		buffer_set_f32(b, pos * 4, value);
	}
}

static void export_texture_to_srgb(buffer_t *to, i32 to_channel, i32 bits) {
	i32 count = _export_texture_pixel_count(to, bits);
	for (i32 i = 0; i < count; ++i) {
		f32 value = _export_texture_get_value(to, i, to_channel, bits);
		value     = value > 0.0 ? math_pow(value, export_texture_gamma) : 0.0;
		if (bits == 8) {
			buffer_set_u8(to, i * 4 + to_channel, math_floor(value * 255));
		}
		else {
			_export_texture_set_value(to, i, to_channel, bits, value);
		}
	}
}

static void export_texture_copy_channel(buffer_t *from, i32 from_channel, buffer_t *to, i32 to_channel, i32 bits, bool linear) {
	from_channel = _export_texture_channel(from_channel, bits);
	to_channel   = _export_texture_channel(to_channel, bits);
	i32 count    = _export_texture_pixel_count(to, bits);
	for (i32 i = 0; i < count; ++i) {
		_export_texture_set_value(to, i, to_channel, bits, _export_texture_get_value(from, i, from_channel, bits));
	}
	if (!linear) {
		export_texture_to_srgb(to, to_channel, bits);
	}
}

static void export_texture_copy_channel_inv(buffer_t *from, i32 from_channel, buffer_t *to, i32 to_channel, i32 bits, bool linear) {
	from_channel = _export_texture_channel(from_channel, bits);
	to_channel   = _export_texture_channel(to_channel, bits);
	i32 count    = _export_texture_pixel_count(to, bits);
	for (i32 i = 0; i < count; ++i) {
		_export_texture_set_value(to, i, to_channel, bits, 1.0 - _export_texture_get_value(from, i, from_channel, bits));
	}
	if (!linear) {
		export_texture_to_srgb(to, to_channel, bits);
	}
}

static void export_texture_extract_channel(buffer_t *from, i32 from_channel, buffer_t *to, i32 to_channel, i32 step, i32 mask, i32 bits, bool linear) {
	from_channel = _export_texture_channel(from_channel, bits);
	to_channel   = _export_texture_channel(to_channel, bits);
	i32 count    = _export_texture_pixel_count(to, bits);
	for (i32 i = 0; i < count; ++i) {
		i32 id = math_floor(_export_texture_get_value(from, i, from_channel, bits) * 255.0 + 0.5);
		_export_texture_set_value(to, i, to_channel, bits, id % step == mask ? 1.0 : 0.0);
	}
	if (!linear) {
		export_texture_to_srgb(to, to_channel, bits);
	}
}

static void export_texture_compute_diff_spec_channel(buffer_t *from_albedo, buffer_t *from_pack, i32 albedo_channel, buffer_t *to, i32 to_channel, i32 bits,
                                                     bool linear, bool is_specular) {
	albedo_channel    = _export_texture_channel(albedo_channel, bits);
	to_channel        = _export_texture_channel(to_channel, bits);
	i32 metal_channel = _export_texture_channel(2, bits);
	i32 count         = _export_texture_pixel_count(to, bits);
	for (i32 i = 0; i < count; ++i) {
		f32 albedo = _export_texture_get_value(from_albedo, i, albedo_channel, bits);
		f32 metal  = _export_texture_get_value(from_pack, i, metal_channel, bits);
		f32 value;
		if (is_specular) {
			value = 0.04 * (1.0 - metal) + albedo * metal;
		}
		else {
			value = albedo * (1.0 - metal);
		}
		_export_texture_set_value(to, i, to_channel, bits, value);
	}
	if (!linear) {
		export_texture_to_srgb(to, to_channel, bits);
	}
}

static void export_texture_set_channel(f32 value, buffer_t *to, i32 to_channel, i32 bits, bool linear) {
	to_channel = _export_texture_channel(to_channel, bits);
	i32 count  = _export_texture_pixel_count(to, bits);
	for (i32 i = 0; i < count; ++i) {
		_export_texture_set_value(to, i, to_channel, bits, value);
	}
	if (!linear) {
		export_texture_to_srgb(to, to_channel, bits);
	}
}

// object_filter (paint id) and tile_filter (udim id) limit every layer to that object or tile, 0 for none.
// Restricted layers are masked to their coverage footprint either way. A tile filter also crops that tile out of the
// flattened atlas at its native size and adds "_<tile>" after the channel name. object_name is the file name label.
static void export_texture_run_layers(char *path, slot_layer_t_array_t *layers, char *object_name, bool bake_material, i32 object_filter, i32 tile_filter) {
	i32 texture_size_x = util_udim_res_x();
	i32 texture_size_y = util_udim_res_y();
#if defined(IRON_ANDROID) || defined(IRON_IOS)
	char *f = sys_title();
#else
	char *f = ui_files_filename;
#endif
	if (string_equals(f, "")) {
		f = string_copy(tr("untitled"));
	}
	texture_ldr_format_t format_type = g_context->format_type;
	i32                  bits        = base_bits == TEXTURE_BITS_BITS8 ? 8 : base_bits == TEXTURE_BITS_BITS16 ? 16 : 32;
	char                *ext         = bits > 8 ? ".exr" : format_type == TEXTURE_LDR_FORMAT_PNG ? ".png" : ".jpg";
	if (ends_with(f, ext)) {
		f = string_copy(substring(f, 0, string_length(f) - 4));
	}

	if (bits > 8 && g_context->layers_destination == EXPORT_DESTINATION_PACK_INTO_PROJECT) {
		ext = ".png"; // Packed as rgba8 png
	}

	// Tile suffix goes after the channel, before the extension
	if (tile_filter > 0) {
		ext = string("_%d%s", tile_filter, ext);
	}

	layers_make_temp_img();
	layers_make_export_img();
	render_target_t *rt    = any_map_get(render_path_render_targets, "empty_white");
	gpu_texture_t   *empty = rt->_image;

	// Append object name, selected layers can cover any set of objects and get none
	bool export_selected = g_context->layers_export == EXPORT_MODE_SELECTED;
	if (!export_selected && !string_equals(object_name, "")) {
		f = string("%s_%s", f, object_name);
	}

	// Clear export layer
	_gpu_begin(layers_expa, NULL, NULL, GPU_CLEAR_COLOR, color_from_floats(0.0, 0.0, 0.0, 0.0), 0.0);
	gpu_end();
	_gpu_begin(layers_expb, NULL, NULL, GPU_CLEAR_COLOR, color_from_floats(0.5, 0.5, 1.0, 0.0), 0.0);
	gpu_end();
	_gpu_begin(layers_expc, NULL, NULL, GPU_CLEAR_COLOR, color_from_floats(1.0, 0.0, 0.0, 0.0), 0.0);
	gpu_end();

	// Flatten layers
	for (i32 i = 0; i < layers->length; ++i) {
		slot_layer_t *l1 = layers->buffer[i];
		if (!export_selected && !slot_layer_is_visible(l1)) {
			continue;
		}
		if (!slot_layer_is_layer(l1)) {
			continue;
		}
		if (slot_layer_has_visible_filter(l1)) {
			continue;
		}

		if (!bake_material && !layers_coverage_applies(l1, object_filter, tile_filter)) {
			continue;
		}

		// The material bake exports the live layer as painted, without masks
		gpu_texture_t *mask = bake_material ? empty : layers_coverage_merge_mask(l1, slot_layer_get_masks(l1, true), false, object_filter, tile_filter);

		if (l1->paint_base || l1->paint_opac) {
			draw_begin(layers_temp_image, false, 0); // Copy to temp
			draw_set_pipeline(pipes_copy);
			draw_image(layers_expa, 0, 0);
			draw_set_pipeline(NULL);
			draw_end();

			_gpu_begin(layers_expa, NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
			gpu_set_pipeline(l1->paint_base ? pipes_merge : pipes_get_merge_a());
			gpu_set_texture(pipes_tex0, l1->texpaint);
			gpu_set_texture(pipes_tex1, empty);
			gpu_set_texture(pipes_texmask, mask);
			gpu_set_texture(pipes_texa, layers_temp_image);
			gpu_set_float(pipes_opac, slot_layer_get_opacity(l1));
			gpu_set_float(pipes_tex1w, empty->width);
			gpu_set_int(pipes_blending, layers->length > 1 ? slot_layer_get_blending(l1) : 0);
			gpu_set_vertex_buffer(const_data_screen_aligned_vb);
			gpu_set_index_buffer(const_data_screen_aligned_ib);
			gpu_draw();
			gpu_end();
		}

		if (l1->paint_nor) {
			draw_begin(layers_temp_image, false, 0);
			draw_set_pipeline(pipes_copy);
			draw_image(layers_expb, 0, 0);
			draw_set_pipeline(NULL);
			draw_end();

			_gpu_begin(layers_expb, NULL, NULL, GPU_CLEAR_NONE, 0, 0.0);
			gpu_set_pipeline(pipes_merge);
			gpu_set_texture(pipes_tex0, l1->texpaint);
			gpu_set_texture(pipes_tex1, l1->texpaint_nor);
			gpu_set_texture(pipes_texmask, mask);
			gpu_set_texture(pipes_texa, layers_temp_image);
			gpu_set_float(pipes_opac, slot_layer_get_opacity(l1));
			gpu_set_float(pipes_tex1w, l1->texpaint_nor->width);
			gpu_set_int(pipes_blending, l1->paint_nor_blend ? 102 : 101);
			gpu_set_vertex_buffer(const_data_screen_aligned_vb);
			gpu_set_index_buffer(const_data_screen_aligned_ib);
			gpu_draw();
			gpu_end();
		}

		if (l1->paint_occ || l1->paint_rough || l1->paint_met || l1->paint_height) {
			draw_begin(layers_temp_image, false, 0);
			draw_set_pipeline(pipes_copy);
			draw_image(layers_expc, 0, 0);
			draw_set_pipeline(NULL);
			draw_end();

			if (l1->paint_occ && l1->paint_rough && l1->paint_met && l1->paint_height) {
				layers_commands_merge_pack(pipes_merge, layers_expc, l1->texpaint, l1->texpaint_pack, slot_layer_get_opacity(l1), mask,
				                           l1->paint_height_blend ? 103 : 101);
			}
			else {
				if (l1->paint_occ)
					layers_commands_merge_pack(pipes_get_merge_r(), layers_expc, l1->texpaint, l1->texpaint_pack, slot_layer_get_opacity(l1), mask, 101);
				if (l1->paint_rough)
					layers_commands_merge_pack(pipes_get_merge_g(), layers_expc, l1->texpaint, l1->texpaint_pack, slot_layer_get_opacity(l1), mask, 101);
				if (l1->paint_met)
					layers_commands_merge_pack(pipes_get_merge_b(), layers_expc, l1->texpaint, l1->texpaint_pack, slot_layer_get_opacity(l1), mask, 101);
			}
		}
	}

	gpu_texture_t *texpaint      = layers_expa;
	gpu_texture_t *texpaint_nor  = layers_expb;
	gpu_texture_t *texpaint_pack = layers_expc;

	export_texture_size_x = texpaint->width;
	export_texture_size_y = texpaint->height;
	texture_size_x        = texpaint->width;
	texture_size_y        = texpaint->height;

	// Crop one udim tile out of the atlas at its native size, the slot rect includes the gutter
	gpu_texture_t *tile_crops[3] = {NULL, NULL, NULL};
	i32            stride        = util_udim_stride();
	i32            slot          = tile_filter > 0 ? util_udim_slot(tile_filter) : -1;
	if (slot >= 0 && stride > 1) {
		i32 tw = texpaint->width / stride;
		i32 th = texpaint->height / stride;
		i32 sx = (slot % stride) * tw;
		i32 sy = (slot / stride) * th;

		gpu_pipeline_t *pipe    = texpaint->format == GPU_TEXTURE_FORMAT_RGBA32   ? pipes_copy
		                          : texpaint->format == GPU_TEXTURE_FORMAT_RGBA64 ? pipes_copy64
		                                                                          : pipes_copy128;
		gpu_texture_t  *srcs[3] = {texpaint, texpaint_nor, texpaint_pack};
		for (i32 i = 0; i < 3; ++i) {
			tile_crops[i] = gpu_create_render_target(tw, th, srcs[i]->format);
			draw_begin(tile_crops[i], true, 0x00000000);
			draw_set_pipeline(pipe);
			draw_scaled_sub_image(srcs[i], sx, sy, tw, th, 0, 0, tw, th);
			draw_set_pipeline(NULL);
			draw_end();
		}
		texpaint              = tile_crops[0];
		texpaint_nor          = tile_crops[1];
		texpaint_pack         = tile_crops[2];
		export_texture_size_x = tw;
		export_texture_size_y = th;
		texture_size_x        = tw;
		texture_size_y        = th;
	}

	buffer_t        *pixpaint      = NULL;
	buffer_t        *pixpaint_nor  = NULL;
	buffer_t        *pixpaint_pack = NULL;
	export_preset_t *preset        = box_export_preset;
	buffer_t        *pix           = NULL;

	for (i32 i = 0; i < preset->textures->length; ++i) {
		export_preset_texture_t *t = preset->textures->buffer[i];
		for (i32 i = 0; i < t->channels->length; ++i) {
			char *c = t->channels->buffer[i];
			if ((string_equals(c, "base_r") || string_equals(c, "base_g") || string_equals(c, "base_b") || string_equals(c, "opac") ||
			     string_equals(c, "diff_r") || string_equals(c, "diff_g") || string_equals(c, "diff_b") || string_equals(c, "spec_r") ||
			     string_equals(c, "spec_g") || string_equals(c, "spec_b")) &&
			    pixpaint == NULL) {
				pixpaint = gpu_get_texture_pixels(texpaint);
			}
			if ((string_equals(c, "nor_r") || string_equals(c, "nor_g") || string_equals(c, "nor_g_directx") || string_equals(c, "nor_b") ||
			     string_equals(c, "emis") || string_equals(c, "subs")) &&
			    pixpaint_nor == NULL) {
				pixpaint_nor = gpu_get_texture_pixels(texpaint_nor);
			}
			if ((string_equals(c, "occ") || string_equals(c, "rough") || string_equals(c, "metal") || string_equals(c, "height") ||
			     string_equals(c, "smooth") || string_equals(c, "diff_r") || string_equals(c, "diff_g") || string_equals(c, "diff_b") ||
			     string_equals(c, "spec_r") || string_equals(c, "spec_g") || string_equals(c, "spec_b")) &&
			    pixpaint_pack == NULL) {
				pixpaint_pack = gpu_get_texture_pixels(texpaint_pack);
			}
		}
	}

	for (i32 i = 0; i < preset->textures->length; ++i) {
		export_preset_texture_t *t        = preset->textures->buffer[i];
		string_array_t          *c        = t->channels;
		char                    *tex_name = !string_equals(t->name, "") ? string("_%s", t->name) : "";
		bool  single_channel = string_equals(c->buffer[0], c->buffer[1]) && string_equals(c->buffer[1], c->buffer[2]) && string_equals(c->buffer[3], "1.0");
		char *out_path       = string("%s%s%s%s%s", path, PATH_SEP, f, tex_name, ext);
		if (string_equals(c->buffer[0], "base_r") && string_equals(c->buffer[1], "base_g") && string_equals(c->buffer[2], "base_b") &&
		    string_equals(c->buffer[3], "1.0") && string_equals(t->color_space, "linear")) {
			export_texture_write_texture(out_path, pixpaint, 1, 0);
		}
		else if (string_equals(c->buffer[0], "nor_r") && string_equals(c->buffer[1], "nor_g") && string_equals(c->buffer[2], "nor_b") &&
		         string_equals(c->buffer[3], "1.0") && string_equals(t->color_space, "linear")) {
			export_texture_write_texture(out_path, pixpaint_nor, 1, 0);
		}
		else if (string_equals(c->buffer[0], "occ") && string_equals(c->buffer[1], "rough") && string_equals(c->buffer[2], "metal") &&
		         string_equals(c->buffer[3], "1.0") && string_equals(t->color_space, "linear")) {
			export_texture_write_texture(out_path, pixpaint_pack, 1, 0);
		}
		else if (single_channel && string_equals(c->buffer[0], "occ") && string_equals(t->color_space, "linear")) {
			export_texture_write_texture(out_path, pixpaint_pack, 2, 0);
		}
		else if (single_channel && string_equals(c->buffer[0], "rough") && string_equals(t->color_space, "linear")) {
			export_texture_write_texture(out_path, pixpaint_pack, 2, 1);
		}
		else if (single_channel && string_equals(c->buffer[0], "metal") && string_equals(t->color_space, "linear")) {
			export_texture_write_texture(out_path, pixpaint_pack, 2, 2);
		}
		else if (single_channel && string_equals(c->buffer[0], "height") && string_equals(t->color_space, "linear")) {
			export_texture_write_texture(out_path, pixpaint_pack, 2, 3);
		}
		else if (single_channel && string_equals(c->buffer[0], "opac") && string_equals(t->color_space, "linear")) {
			export_texture_write_texture(out_path, pixpaint, 2, 3);
		}
		else {
			if (pix == NULL) {
				pix = buffer_create(texture_size_x * texture_size_y * 4 * math_floor(bits / 8.0));
			}
			for (i32 i = 0; i < 4; ++i) {
				char *c = t->channels->buffer[i];
				if (string_equals(c, "base_r")) {
					export_texture_copy_channel(pixpaint, 0, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "base_g")) {
					export_texture_copy_channel(pixpaint, 1, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "base_b")) {
					export_texture_copy_channel(pixpaint, 2, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "height")) {
					export_texture_copy_channel(pixpaint_pack, 3, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "metal")) {
					export_texture_copy_channel(pixpaint_pack, 2, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "nor_r")) {
					export_texture_copy_channel(pixpaint_nor, 0, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "nor_g")) {
					export_texture_copy_channel(pixpaint_nor, 1, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "nor_g_directx")) {
					export_texture_copy_channel_inv(pixpaint_nor, 1, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "nor_b")) {
					export_texture_copy_channel(pixpaint_nor, 2, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "occ")) {
					export_texture_copy_channel(pixpaint_pack, 0, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "opac")) {
					export_texture_copy_channel(pixpaint, 3, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "rough")) {
					export_texture_copy_channel(pixpaint_pack, 1, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "smooth")) {
					export_texture_copy_channel_inv(pixpaint_pack, 1, pix, i, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "emis")) {
					export_texture_extract_channel(pixpaint_nor, 3, pix, i, 3, 1, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "subs")) {
					export_texture_extract_channel(pixpaint_nor, 3, pix, i, 3, 2, bits, string_equals(t->color_space, "linear"));
				}
				else if (string_equals(c, "diff_r")) {
					export_texture_compute_diff_spec_channel(pixpaint, pixpaint_pack, 0, pix, i, bits, string_equals(t->color_space, "linear"), false);
				}
				else if (string_equals(c, "diff_g")) {
					export_texture_compute_diff_spec_channel(pixpaint, pixpaint_pack, 1, pix, i, bits, string_equals(t->color_space, "linear"), false);
				}
				else if (string_equals(c, "diff_b")) {
					export_texture_compute_diff_spec_channel(pixpaint, pixpaint_pack, 2, pix, i, bits, string_equals(t->color_space, "linear"), false);
				}
				else if (string_equals(c, "spec_r")) {
					export_texture_compute_diff_spec_channel(pixpaint, pixpaint_pack, 0, pix, i, bits, string_equals(t->color_space, "linear"), true);
				}
				else if (string_equals(c, "spec_g")) {
					export_texture_compute_diff_spec_channel(pixpaint, pixpaint_pack, 1, pix, i, bits, string_equals(t->color_space, "linear"), true);
				}
				else if (string_equals(c, "spec_b")) {
					export_texture_compute_diff_spec_channel(pixpaint, pixpaint_pack, 2, pix, i, bits, string_equals(t->color_space, "linear"), true);
				}
				else if (string_equals(c, "0.0")) {
					export_texture_set_channel(0.0, pix, i, bits, true);
				}
				else if (string_equals(c, "1.0")) {
					export_texture_set_channel(1.0, pix, i, bits, true);
				}
			}
			export_texture_write_texture(out_path, pix, 3, 0);
		}
	}

	for (i32 i = 0; i < 3; ++i) {
		if (tile_crops[i] != NULL) {
			gpu_delete_texture(tile_crops[i]);
		}
	}

	// Release staging memory allocated in gpu_get_texture_pixels()
	// texpaint->pixels = NULL;
	// texpaint_nor->pixels = NULL;
	// texpaint_pack->pixels = NULL;
}

static void export_texture_bake_material_add_texture(char *file) {
	if (export_texture_baked_files != NULL) {
		any_array_push(export_texture_baked_files, string_copy(file));
	}
}

static i32 export_texture_asset_index(char *file) {
	for (i32 i = 0; i < g_project->_->assets->length; ++i) {
		if (string_equals(g_project->_->assets->buffer[i]->file, file)) {
			return i;
		}
	}
	return -1;
}

static void export_texture_link(ui_node_canvas_t *canvas, ui_node_t *from, i32 from_socket, ui_node_t *to, i32 to_socket) {
	ui_node_link_t *l =
	    ALLOC_INIT(ui_node_link_t,
	               {.id = ui_next_link_id(canvas->links), .from_id = from->id, .from_socket = from_socket, .to_id = to->id, .to_socket = to_socket});
	any_array_push(canvas->links, l);
}

static i32 export_texture_output_socket(char *name) {
	if (string_equals(name, "base")) {
		return 0;
	}
	if (string_equals(name, "occ")) {
		return 2;
	}
	if (string_equals(name, "rough")) {
		return 3;
	}
	if (string_equals(name, "metal")) {
		return 4;
	}
	if (string_equals(name, "nor")) {
		return 5;
	}
	return -1;
}

static void export_texture_create_baked_material(slot_material_t *source) {
	export_preset_t *preset = box_export_preset;
	string_array_t  *files  = export_texture_baked_files;

	// Bring the written files in as texture assets
	if (g_context->layers_destination != EXPORT_DESTINATION_PACK_INTO_PROJECT) {
		for (i32 i = 0; i < files->length; ++i) {
			i32 index = export_texture_asset_index(files->buffer[i]);
			if (index >= 0) {
				project_reimport_texture_load(files->buffer[i], g_project->_->assets->buffer[index]);
			}
			else {
				import_texture_run(files->buffer[i], false);
			}
		}
	}

	slot_material_t *m = slot_material_create(g_project->_->materials->buffer[0]->data, NULL);
	any_array_push(g_project->_->materials, m);
	g_context->material      = m;
	ui_node_canvas_t *canvas = m->canvas;
	canvas->name             = string_copy(slot_material_unique_name(canvas, string("%s_baked", source->canvas->name)));
	node_group_t group       = {.nodes = m->nodes, .canvas = canvas};

	ui_node_t *nout = NULL;
	for (i32 i = 0; i < canvas->nodes->length; ++i) {
		ui_node_t *n = canvas->nodes->buffer[i];
		if (string_equals(n->type, "OUTPUT_MATERIAL_PBR")) {
			nout = n;
			break;
		}
	}
	for (i32 i = 0; i < canvas->nodes->length; ++i) {
		ui_node_t *n = canvas->nodes->buffer[i];
		if (string_equals(n->name, "Color")) {
			ui_remove_node(n, canvas);
			break;
		}
	}

	i32 node_y = 100;
	for (i32 i = 0; i < files->length && i < preset->textures->length; ++i) {
		i32 socket      = export_texture_output_socket(preset->textures->buffer[i]->name);
		i32 asset_index = export_texture_asset_index(files->buffer[i]);
		if (socket < 0 || asset_index < 0) {
			continue;
		}
		ui_node_t *img                         = nodes_material_create_node("TEX_IMAGE", &group);
		img->buttons->buffer[0]->default_value = f32_array_create_x(asset_index);
		img->x                                 = nout->x - 300;
		img->y                                 = node_y;
		node_y += 180;
		export_texture_link(canvas, img, 0, nout, socket);
	}

	make_material_parse_paint_material(true);
	util_render_make_material_preview();
	ui_dock_redraw(TAB_AREA_SIDEBAR1);
	history_new_material();
}

static void export_texture_run_bake_material(char *path) {
	if (render_path_paint_live_layer == NULL) {
		render_path_paint_live_layer = slot_layer_create("_live", LAYER_SLOT_TYPE_LAYER, NULL);
	}

	tool_type_t _tool         = g_context->tool;
	g_context->tool           = TOOL_TYPE_FILL;
	util_udim_paint_all_tiles = true; // The material is baked on every tile, whatever the active layer covers
	make_material_parse_paint_material(true);
	util_udim_paint_all_tiles    = false;
	mesh_object_t *_paint_object = g_context->paint_object;
	mesh_object_t *planeo        = scene_get_child(".Plane")->ext;
	planeo->base->visible        = true;
	g_context->paint_object      = planeo;
	g_context->pdirty            = 1;

	u8_array_t *_visibles = u8_array_create_from_raw((u8[]){}, 0);
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		mesh_object_t *p = g_project->_->paint_objects->buffer[i];
		u8_array_push(_visibles, p->base->visible);
		p->base->visible = false;
	}

	render_path_paint_use_live_layer(true);
	render_path_paint_commands_paint(false);
	render_path_paint_use_live_layer(false);

	g_context->tool = _tool;
	make_material_parse_paint_material(true);
	g_context->pdirty       = 0;
	planeo->base->visible   = false;
	g_context->paint_object = _paint_object;

	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		g_project->_->paint_objects->buffer[i]->base->visible = _visibles->buffer[i];
	}

	slot_layer_t_array_t *layers = any_array_create_from_raw(
	    (void *[]){
	        render_path_paint_live_layer,
	    },
	    1);
	slot_material_t *source = g_context->material;
	if (g_context->bake_create_material) {
		export_texture_baked_files = any_array_create_from_raw((void *[]){}, 0);
	}
	export_texture_run_layers(path, layers, "", true, 0, 0);
	if (export_texture_baked_files != NULL) {
		export_texture_create_baked_material(source);
		export_texture_baked_files = NULL;
	}
}

// Paint ids of the objects some object restricted layer of the list covers
static i32_array_t *export_texture_object_ids(slot_layer_t_array_t *layers) {
	i32_array_t *ids = i32_array_create(0);
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		mesh_object_t *o = g_project->_->paint_objects->buffer[i];
		for (i32 j = 0; j < layers->length; ++j) {
			slot_layer_t *l = layers->buffer[j];
			if (slot_layer_is_layer(l) && layer_objects_restricted(l) && layer_covers_object(l, o)) {
				i32_array_push(ids, o->paint_id);
				break;
			}
		}
	}
	return ids;
}

// Whether the mesh of the object has geometry on the tile
static bool export_texture_object_uses_tile(mesh_object_t *o, i32 tile) {
	mesh_data_t *md = o->data;
	util_udim_ensure(md);
	if (!util_udim_has_tiles(md)) {
		return tile == 1001;
	}
	for (i32 i = 0; i < md->_->udim_tile->length; ++i) {
		if (md->_->udim_tile->buffer[i] + 1001 == tile) {
			return true;
		}
	}
	return false;
}

// Some layer of the list applies to the object and tile
static bool export_texture_any_layer(slot_layer_t_array_t *layers, i32 object_filter, i32 tile_filter) {
	for (i32 i = 0; i < layers->length; ++i) {
		slot_layer_t *l = layers->buffer[i];
		if (slot_layer_is_layer(l) && slot_layer_is_visible(l) && !slot_layer_has_visible_filter(l) && layers_coverage_applies(l, object_filter, tile_filter)) {
			return true;
		}
	}
	return false;
}

// One texture set per project tile a mesh uses, and per object too when objects is set
static void export_texture_run_tiles(char *path, bool objects) {
	slot_layer_t_array_t *layers = g_project->_->layers;
	i32_array_t          *ids    = i32_array_create(0); // Per Object & Tile: every object on the tile
	for (i32 i = 0; objects && i < g_project->_->paint_objects->length; ++i) {
		i32_array_push(ids, g_project->_->paint_objects->buffer[i]->paint_id);
	}
	for (i32 s = 0; s < util_udim_count(); ++s) {
		i32 tile = util_udim_tile(s);
		if (!util_udim_tile_used(tile)) {
			continue;
		}
		if (ids->length == 0) {
			export_texture_run_layers(path, layers, "", false, 0, tile);
			continue;
		}
		for (i32 i = 0; i < ids->length; ++i) {
			mesh_object_t *o = project_object_by_id(ids->buffer[i]);
			if (export_texture_object_uses_tile(o, tile) && export_texture_any_layer(layers, o->paint_id, tile)) {
				export_texture_run_layers(path, layers, o->base->name, false, o->paint_id, tile);
			}
		}
	}
}

// Mode the export box opens with, per tile when the project has several tiles
export_mode_t export_texture_default_mode() {
	return util_udim_count() > 1 ? EXPORT_MODE_PER_UDIM_TILE : EXPORT_MODE_VISIBLE;
}

void export_texture_run(char *path, bool bake_material) {

	// Exporting without opening the export box
	if (box_export_files == NULL) {
		box_export_fetch_presets();
		i32 i                   = string_array_index_of(box_export_files, "generic");
		box_export_preset_index = i > 0 ? i : 0;
	}
	if (box_export_preset == NULL) {
		box_export_parse_preset();
	}

	// Tile modes need a project with tiles other than a lone 1001
	if (!util_udim_active() && g_context->layers_export > EXPORT_MODE_PER_OBJECT) {
		g_context->layers_export = EXPORT_MODE_VISIBLE;
	}

	if (bake_material) {
		export_texture_run_bake_material(path);
	}
	else if (g_context->layers_export == EXPORT_MODE_PER_UDIM_TILE || g_context->layers_export == EXPORT_MODE_PER_OBJECT_TILE) {
		export_texture_run_tiles(path, g_context->layers_export == EXPORT_MODE_PER_OBJECT_TILE);
	}
	else if (g_context->layers_export == EXPORT_MODE_PER_OBJECT) {
		// Objects some object restricted layer covers, each gets its own texture set
		i32_array_t *ids = export_texture_object_ids(g_project->_->layers);
		if (ids->length > 0) {
			for (i32 i = 0; i < ids->length; ++i) {
				mesh_object_t *o = project_object_by_id(ids->buffer[i]);
				export_texture_run_layers(path, g_project->_->layers, o->base->name, false, o->paint_id, 0);
			}
		}
		else {
			export_texture_run_layers(path, g_project->_->layers, "", false, 0, 0);
		}
	}
	else { // Visible or selected
		slot_layer_t_array_t *layers;
		if (g_context->layers_export == EXPORT_MODE_SELECTED) {
			if (slot_layer_is_group(g_context->layer)) {
				layers = slot_layer_get_children(g_context->layer);
			}
			else {
				layers = any_array_create_from_raw(
				    (void *[]){
				        g_context->layer,
				    },
				    1);
			}
		}
		else {
			layers = g_project->_->layers;
		}
		export_texture_run_layers(path, layers, "", false, 0, 0);
	}

#ifdef IRON_IOS
	if (config_is_iphone()) {
		console_info(string("%s (\"Files/On My iPhone/%s\")", tr("Textures exported"), manifest_title));
	}
	else {
		console_info(string("%s (\"Files/On My iPad/%s\")", tr("Textures exported"), manifest_title));
	}
#elif defined(IRON_ANDROID)
	console_info(string("%s (\"Files/Internal storage/Pictures/%s\")", tr("Textures exported"), manifest_title));
#else
	console_info(tr("Textures exported"));
#endif
	ui_files_last_path = "";
}
