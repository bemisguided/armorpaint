// Test natives: script functions that exist only to drive the app from the automated tests (armorpaint-admin/automation).
// Compiled only when the build defines ARMORPAINT_TEST_NATIVES (base/make --test-natives, automation/fixtures/build_test_app.sh);
// their entries in minic_api_list.h are behind the same flag, so the plain app has none of these names.
// Included from main.c after minic_impl.c, whose static helpers (script_gpu_begin, script_key_press_up, ...) it uses.

#ifdef ARMORPAINT_TEST_NATIVES

// Files, assets and metadata

// The next file or folder dialog does not open and answers with path, once. Open dialogs pass path to their callback.
// Save dialogs, also the folder pickers, take path as the chosen file: the callback gets its folder and
// ui_files_filename its name, so "dir/name.png" picks folder "dir", and "dir/" picks "dir" with no name.
void script_files_next(char *path) {
	ui_files_answer_next(path);
}

// Asset metadata by category (meta_category_t) and index: the position in the matching tray / project array
// (fonts include the default font at 0). Out of range indices are ignored.
void script_meta_set_i32(i32 category, i32 index, i32 key, i32 value) {
	meta_set_i32(meta_get(category, index), key, value);
}

i32 script_meta_get_i32(i32 category, i32 index, i32 key, i32 def) {
	return meta_get_i32(meta_peek(category, index), key, def);
}

void script_meta_add_tag(i32 category, i32 index, char *tag) {
	meta_add_tag(meta_get(category, index), tag);
}

i32 script_meta_has_tag(i32 category, i32 index, char *tag) {
	return meta_has_tag(meta_peek(category, index), tag);
}

i32 script_asset_count(i32 category) {
	return meta_count(category);
}

char *script_asset_name(i32 category, i32 index) {
	return meta_item_name(category, index);
}

// Layer coverage

// Persistent paint ids (mesh_object_t.paint_id), all covers every object and keeps ids for later
void script_layer_set_objects(slot_layer_t *l, i32_array_t *ids, bool all) {
	script_layer_set_coverage(l, false, ids != NULL ? ids->buffer : NULL, ids != NULL ? ids->length : 0, all);
}

// Udim tile ids (1001..), all covers every tile and keeps ids for later
void script_layer_set_tiles(slot_layer_t *l, i32_array_t *ids, bool all) {
	script_layer_set_coverage(l, true, ids != NULL ? ids->buffer : NULL, ids != NULL ? ids->length : 0, all);
}

// Coverage as stored on the layer (slot_layer_t.meta), the lists are copies (empty when none), editing them changes nothing
bool script_layer_all_objects(slot_layer_t *l) {
	return !script_layer_valid(l) || slot_layer_all_objects(l);
}

i32_array_t *script_layer_objects(slot_layer_t *l) {
	return script_layer_valid(l) ? i32_array_create_from_array(slot_layer_objects(l)) : NULL; // NULL for an invalid layer
}

bool script_layer_all_tiles(slot_layer_t *l) {
	return !script_layer_valid(l) || slot_layer_all_tiles(l);
}

i32_array_t *script_layer_tiles(slot_layer_t *l) {
	return script_layer_valid(l) ? i32_array_create_from_array(slot_layer_tiles(l)) : NULL; // NULL for an invalid layer
}

// Mouse

static void script_mouse_move_next(void *data) {
	script_click_t *c = data;
	script_mouse_apply_move(c);
	free(c);
}

static void script_mouse_down_next(void *data) {
	script_click_t *c = data;
	script_mouse_apply_down(c);
	free(c);
}

static void script_mouse_up_next(void *data) {
	script_click_t *c = data;
	script_mouse_apply_move(c);
	iron_internal_mouse_trigger_release(c->button, c->x, c->y);
	free(c);
}

// Move the mouse to window pixel x, y next frame, a button that is down stays down (drags)
void script_mouse_move(i32 x, i32 y) {
	sys_notify_on_next_frame(script_mouse_move_next, script_click_create(x, y, 0));
}

// Move to window pixel x, y and press next frame, button 0 left, 1 right, 2 middle; release with script_mouse_up
void script_mouse_down(i32 x, i32 y, i32 button) {
	if (button < 0 || button > 4) {
		return;
	}
	sys_notify_on_next_frame(script_mouse_down_next, script_click_create(x, y, button));
}

// Move to window pixel x, y and release next frame
void script_mouse_up(i32 x, i32 y, i32 button) {
	if (button < 0 || button > 4) {
		return;
	}
	sys_notify_on_next_frame(script_mouse_up_next, script_click_create(x, y, button));
}

// Project resolution like the Resolution combo of the Layers tab and the export dialog: 0 2048, 1 4096, 2 8192, 3 16384
void script_project_set_resolution(i32 res) {
	if (res < TEXTURE_RES_RES2048 || res > TEXTURE_RES_RES16384) {
		return;
	}
	base_res = res;
	config_set_texture_res(base_res);
	layers_on_resized();
}

// Export dialog Color combo: 0 8bit, 1 16bit, 2 32bit. Rebuilds the layers next frame like the combo
static void script_export_set_bits_next(void *data) {
	layers_set_bits();
}

void script_export_set_bits(i32 bits) {
	if (bits < TEXTURE_BITS_BITS8 || bits > TEXTURE_BITS_BITS32) {
		return;
	}
	base_bits = bits;
	sys_notify_on_next_frame(script_export_set_bits_next, NULL);
}

// Export dialog Format combo: 0 png, 1 jpg (8bit only; 16bit and 32bit always write exr)
void script_export_set_format(i32 format) {
	if (format < TEXTURE_LDR_FORMAT_PNG || format > TEXTURE_LDR_FORMAT_JPG) {
		return;
	}
	g_context->format_type = format;
}

// Export dialog Destination combo: 0 Disk, 1 Pack into Project. Pack writes into the project when export_texture_run gets the path "/"
void script_export_set_destination(i32 destination) {
	if (destination < EXPORT_DESTINATION_DISK || destination > EXPORT_DESTINATION_PACK_INTO_PROJECT) {
		return;
	}
	g_context->layers_destination = destination;
}

// Test only: held keys and the asset tray selection

static i32 script_key_code_of(char *key) {
	for (i32 code = 1; code < 256; ++code) {
		if (string_equals(keyboard_key_code(code), key)) {
			return code;
		}
	}
	return -1;
}

static void script_key_down_next(void *data) {
	iron_internal_keyboard_trigger_key_down((i32)(intptr_t)data);
}

static void script_key_up_next(void *data) {
	iron_internal_keyboard_trigger_key_up((i32)(intptr_t)data);
}

// Press key next frame and keep it down until script_key_up
void script_key_down(char *key) {
	i32 code = script_key_code_of(key);
	if (code != -1) {
		sys_notify_on_next_frame(script_key_down_next, (void *)(intptr_t)code);
	}
}

void script_key_up(char *key) {
	i32 code = script_key_code_of(key);
	if (code != -1) {
		sys_notify_on_next_frame(script_key_up_next, (void *)(intptr_t)code);
	}
}

static char *script_asset_item_name(asset_tray_t tray, void *item) {
	switch (tray) {
	case ASSET_TRAY_TEXTURES:
		return ((asset_t *)item)->name;
	case ASSET_TRAY_MATERIALS:
		return ((slot_material_t *)item)->canvas->name;
	case ASSET_TRAY_BRUSHES:
		return ((slot_brush_t *)item)->canvas->name;
	case ASSET_TRAY_FONTS:
		return ((slot_font_t *)item)->name;
	case ASSET_TRAY_SOUNDS:
		return ((slot_sound_t *)item)->name;
	default:
		return "";
	}
}

static char *script_asset_names(asset_tray_t tray, any_array_t *items) {
	char *names = "";
	for (i32 i = 0; i < items->length; ++i) {
		names = string("%s%s%s", names, i > 0 ? "," : "", script_asset_item_name(tray, items->buffer[i]));
	}
	return names;
}

// Tray 1 textures, 2 materials, 3 brushes, 4 fonts, 5 sounds: "active=<name> selected=<a,b,..>" in selection order
char *script_asset_selection(i32 tray) {
	if (tray <= ASSET_TRAY_NONE || tray >= ASSET_TRAY_COUNT) {
		return "";
	}
	void *active = asset_selection_active(tray);
	return string("active=%s selected=%s", active != NULL ? script_asset_item_name(tray, active) : "", script_asset_names(tray, asset_selection_get(tray)));
}

// Names of the tray items in tray order, comma separated
char *script_asset_tray(i32 tray) {
	if (tray <= ASSET_TRAY_NONE || tray >= ASSET_TRAY_COUNT) {
		return "";
	}
	return script_asset_names(tray, asset_selection_tray_items(tray));
}

// Test only (branch test/all2): Flood Fill and image node state for the scripted test plan

void script_image_node_regenerate() {
	make_image_node_regenerate();
}

// Test only: the Layers combo of the export dialog (0 Visible, 1 Selected, 2 Per Object, 3 Per UDIM Tile, 4 Per Object & Tile)
void script_export_mode(i32 mode) {
	g_context->layers_export = mode;
}

// Test only: true when the objects covered by the layer share UV space (the Texture Slot node label)
bool script_layer_uv_overlap(slot_layer_t *l) {
	return l != NULL && util_uv_overlap_covered(l);
}

// Test only: save the project to path (next frame), undo and redo like the Edit menu
void script_project_save_to(char *path) {
	g_project->_->filepath = script_resolve_path(path);
	project_save(false);
}

// A node editor edit with an undo step, for the tests of the image node capture gating: edit_begin keeps a copy of the canvas,
// the script edits nodes, edit_end pushes the undo step like the editor does after a change (the script's node pointers are stale after undo and redo)
static ui_node_canvas_t *script_material_edit_before = NULL;

// Reimport Textures (ctrl+shift+r): reloads every image asset from its file
void script_asset_reimport() {
	project_reimport_textures();
}

void script_material_edit_begin() {
	script_material_edit_before = util_clone_canvas(g_context->material->canvas);
}

void script_material_edit_end() {
	if (script_material_edit_before == NULL) {
		return;
	}
	ui_nodes_push_undo(script_material_edit_before);
	script_material_edit_before = NULL;
	ui_nodes_canvas_changed();
}

void script_history_undo() {
	history_undo();
}

void script_history_redo() {
	history_redo();
}

static gpu_texture_t *script_flood_fill_result(ui_node_t *node) {
	return node != NULL ? make_image_node_result(node) : NULL;
}

bool script_flood_fill_pending(ui_node_t *node) {
	return node != NULL && make_image_node_is_pending(node);
}

i32 script_flood_fill_islands(ui_node_t *node) {
	return node != NULL ? make_image_node_result_count(node) : -1;
}

i32 script_flood_fill_width(ui_node_t *node) {
	gpu_texture_t *image = script_flood_fill_result(node);
	return image != NULL ? image->width : 0;
}

// Channel 0 ID, 1 Position x, 2 Position y, 3 Area of the baked texture at uv (u, v), -1 when not baked
f32 script_flood_fill_pixel(ui_node_t *node, f32 u, f32 v, i32 channel) {
	gpu_texture_t *image = script_flood_fill_result(node);
	if (image == NULL || channel < 0 || channel > 3) {
		return -1.0;
	}
	gpu_texture_t *current;
	bool           in_use;
	script_gpu_begin(&current, &in_use);
	buffer_t *b = gpu_get_texture_pixels(image);
	script_gpu_end(current, in_use);
	i32 x = (i32)math_floor(math_min(math_max(u, 0.0), 0.9999) * image->width);
	i32 y = (i32)math_floor(math_min(math_max(v, 0.0), 0.9999) * image->height);
	if (image->format == GPU_TEXTURE_FORMAT_RGBA128) {
		return buffer_get_f32(b, ((y * image->width + x) * 4 + channel) * 4);
	}
	return buffer_get_f16(b, ((y * image->width + x) * 4 + channel) * 2);
}

void script_flood_fill_bake(ui_node_t *node) {
	if (node != NULL) {
		make_image_node_bake_now(node);
	}
}

// New project on the built-in plane template
void script_flood_fill_new_plane() {
	project_fetch_default_meshes();
	g_context->project_type = string_array_index_of(project_default_mesh_list, "plane");
	script_project_new();
}

// Test only: top view, the plane fills the viewport
void script_flood_fill_view_top() {
	viewport_set_view(0, 0, 1, 0, 0, 0);
}

// Test only: named camera views of the viewport: top, bottom, front, back, left, right (like the compass), corner (reset view
// orbited 45 degrees around Z and 35 degrees up). The camera keys do not move the camera in scripts.
void script_viewport_view(char *name) {
	if (name == NULL || g_context == NULL || g_context->paint_object == NULL) {
		return;
	}
	f32 pi = math_pi();
	if (string_equals(name, "top")) {
		viewport_set_view(0, 0, 1, 0, 0, 0);
	}
	else if (string_equals(name, "bottom")) {
		viewport_set_view(0, 0, -1, pi, 0, pi);
	}
	else if (string_equals(name, "front")) {
		viewport_set_view(0, -1, 0, pi / 2.0, 0, 0);
	}
	else if (string_equals(name, "back")) {
		viewport_set_view(0, 1, 0, pi / 2.0, 0, pi);
	}
	else if (string_equals(name, "left")) {
		viewport_set_view(-1, 0, 0, pi / 2.0, 0, -pi / 2.0);
	}
	else if (string_equals(name, "right")) {
		viewport_set_view(1, 0, 0, pi / 2.0, 0, pi / 2.0);
	}
	else if (string_equals(name, "corner")) {
		viewport_reset();
		viewport_orbit(pi / 4.0, 0.6);
	}
}

// Test only: type one character with its key held: key is the key name (a, space, 1, ...), ch the character the text field
// receives (A for shift + a, a space for the space key). script_key_press sends the key name as the character.
static void script_key_char_down(void *data) {
	char *ch   = (char *)data;
	i32   code = script_key_code_of(ch + 1);
	iron_internal_keyboard_trigger_key_down(code);
	iron_internal_keyboard_trigger_key_press(ch[0]);
	sys_notify_on_next_frame(script_key_press_up, (void *)(intptr_t)code);
	free(ch);
}

void script_key_char(char *key, char *ch) {
	if (key == NULL || ch == NULL || ch[0] == '\0' || script_key_code_of(key) == -1) {
		return;
	}
	size_t n   = strlen(key);
	char  *buf = malloc(n + 2); // The character, then the key name (freed in script_key_char_down)
	buf[0]     = ch[0];
	memcpy(buf + 1, key, n + 1);
	sys_notify_on_next_frame(script_key_char_down, buf);
}

// Shows the node editor dock; only: hides the 3D view so the editor takes the whole window (test builds)
void script_show_node_editor(bool only, f32 zoom, f32 pan_x, f32 pan_y) {
	ui_dock_show_view(TAB_NODES, true);
	if (only) {
		ui_dock_show_view(TAB_VIEW3D, false);
	}
	base_resize();
	ui_nodes_t *nodes = ui_nodes_get_nodes();
	nodes->zoom      = zoom;
	nodes->pan_x     = pan_x;
	nodes->pan_y     = pan_y;
	if (ui_nodes_hwnd != NULL) {
		ui_nodes_hwnd->redraws = 2;
	}
}

// Node editor layout, in rows (UI_LINE_H) so that the zoom does not matter. kind: 0 input, 1 output, 2 button.
// Whether a row is hidden by the rules of its node (a socket with a link is never hidden, see ui_node_item_hidden)
i32 script_node_item_hidden(ui_node_t *node, i32 kind, i32 index) {
	ui_node_canvas_t *canvas = script_material_canvas();
	if (node == NULL || canvas == NULL) {
		return 0;
	}
	return ui_node_item_hidden(canvas, node, (ui_node_item_t)kind, index) ? 1 : 0;
}

// Height of the node as the editor lays it out
f32 script_node_rows(ui_node_t *node) {
	ui_node_canvas_t *canvas = script_material_canvas();
	return node == NULL || canvas == NULL ? 0.0 : UI_NODE_H(canvas, node) / UI_LINE_H();
}

// Whether the 2D view is showing (a double click on a selected node opens it)
bool script_view2d_shown() {
	return ui_view2d_show;
}

// Closes the 2D view when it is showing (test builds, to start a case from a known state)
void script_view2d_hide() {
	if (ui_view2d_show) {
		ui_base_show_2d_view(ui_view2d_type); // The same type toggles it off
	}
}

// Distance of the link end of input (output when is_input is false) from the top of the node
f32 script_node_socket_row(ui_node_t *node, i32 is_input, i32 index) {
	ui_node_canvas_t *canvas = script_material_canvas();
	if (node == NULL || canvas == NULL) {
		return 0.0;
	}
	f32 y = is_input ? UI_INPUT_Y(canvas, node, index) + UI_OUTPUTS_H(canvas, node, -1) + UI_BUTTONS_H(node) : UI_OUTPUT_Y(canvas, node, index);
	return y / UI_LINE_H();
}

// Sections of a node (the list its node type registered, make_image_node_sections): how many, whether one is closed, and set it
i32 script_node_section_count(ui_node_t *node) {
	const ui_node_section_t *sections = node == NULL ? NULL : ui_viewnodes_on_sections(node);
	i32                      n        = 0;
	while (sections != NULL && sections[n].title != NULL) {
		n++;
	}
	return n;
}

i32 script_node_section_closed(ui_node_t *node, i32 section) {
	return section >= 0 && section < script_node_section_count(node) && ui_node_section_closed(node, section) ? 1 : 0;
}

// Sets the state like a click on the header does (the flag bit is toggled away from the default, no undo step)
void script_node_set_section_closed(ui_node_t *node, i32 section, i32 closed) {
	if (section >= 0 && section < script_node_section_count(node) && (script_node_section_closed(node, section) != 0) != (closed != 0)) {
		node->flags ^= UI_NODE_FLAG_SECTION_0 << section;
	}
}

// The section whose header row sits in front of the item (kind 0 input, 2 button), -1 when none
i32 script_node_section_header(ui_node_t *node, i32 kind, i32 index) {
	ui_node_canvas_t *canvas = script_material_canvas();
	return node == NULL || canvas == NULL ? -1 : ui_node_section_header_at(canvas, node, (ui_node_item_t)kind, index);
}

// Test only: enable a plugin like the Preferences dialog does (name as in the plugins folder, e.g. bake_maps.c)
void script_plugin_enable(char *name) {
	if (name != NULL) {
		config_enable_plugin(string_copy(name));
	}
}

#endif
