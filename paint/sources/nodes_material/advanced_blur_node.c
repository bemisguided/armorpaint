#include "../global.h"

// Advanced Blur (Filter menu): blurs the Image input. An image node (render/make_image_node.c): Image and Mask are
// evaluated in image space and captured as R32F planes (one float per pixel: Image, then Mask; an unlinked Mask is no
// capture, its value holds everywhere), the blur is computed on the GPU into one RGBA128 render target per node, R = the blurred image, which is
// the result texture itself (result by reference: no readback, no cpu pass, no upload).
// The texture is sampled like an Image Texture at tex_coord, repeating (Tile UV places it on the uv). With Wrap Edges on
// (the default) the image repeats, so the blur wraps around the edges; off, samples outside the image clamp to the nearest edge pixel.
//
// Modes: Gaussian and Box blur along the Angle direction by Radius times Aspect and then across it by Radius
// (Aspect 1 is a round blur, higher stretches it along Angle: anisotropic blur); Directional blurs only along
// Angle by Radius (a line, like a motion blur). Radius is a fraction of Size.
// Angle follows the rotation convention of all image nodes (armorpaint-admin specs/image-nodes/ROTATION_CONVENTION.md):
// degrees, the direction is (cos Angle, sin Angle) on the y down image, so 0 is along x and 90 points down the image
// (the same direction as Warp's Angle); a blur has no sense, 90 and 270 are the same line.
// Radius, Angle and Aspect hold for the whole image (scalar, evaluated at the image centre when linked).
// Mask multiplies the radius per pixel (Non-Uniform Blur): each pass takes the radius at the pixel it writes, so a
// varying radius is an approximation, exact only where the mask is constant. Taps are never more than one pixel apart:
// wide boxes are two passes (a dense box and a comb of boxes), wide gaussians three boxes, so large radii do not alias.
// The result is kept in 32 bit float (a half float steps by 1/2048 near 1, which a Bump node shows on a soft gradient).

// Input socket indices
enum {
	ADVANCED_BLUR_INPUT_IMAGE  = 0,
	ADVANCED_BLUR_INPUT_RADIUS = 1,
	ADVANCED_BLUR_INPUT_ANGLE  = 2,
	ADVANCED_BLUR_INPUT_ASPECT = 3,
	ADVANCED_BLUR_INPUT_MASK   = 4,
};

// Node button indices (settings)
enum {
	ADVANCED_BLUR_BUTTON_SIZE = 0, // Button 1 is the state row
	ADVANCED_BLUR_BUTTON_MODE = 2,
	ADVANCED_BLUR_BUTTON_WRAP = 3, // Wrap Edges
};

enum {
	ADVANCED_BLUR_MODE_GAUSSIAN    = 0,
	ADVANCED_BLUR_MODE_BOX         = 1,
	ADVANCED_BLUR_MODE_DIRECTIONAL = 2,
};

#define ADVANCED_BLUR_MAX_TAPS 65
#define ADVANCED_BLUR_BOX_DENSE 64   // A box up to this width (pixels) is one dense pass, wider ones two
#define ADVANCED_BLUR_GAUSS_DENSE 32 // A gaussian up to this radius (pixels) is one dense pass, larger ones three boxes
#define ADVANCED_BLUR_MAX_RADIUS_SIZES 4.0f // Largest radius of a pass in sizes: the sliders reach 0.25 * 10 = 2.5, this bounds huge linked values
#define ADVANCED_BLUR_MAX_RADIUS 1.0f       // Largest Radius (fraction of Size)
#define ADVANCED_BLUR_MAX_ASPECT 100.0f

typedef struct advanced_blur_params {
	i32 mode;
	f32 radius; // Fraction of Size
	f32 angle;  // Radians
	f32 aspect;
	i32 wrap; // Wrap Edges: samples outside the image wrap around (1) or clamp to the edge (0); part of the node signature
	i32 mask_linked; // The Mask is captured (1, a second plane after the Image) or the constant mask_value (0)
	f32 mask_value;  // The Mask of an unlinked socket
} advanced_blur_params_t;

// Gaussian and Box use Angle and Aspect (anisotropic blur); Directional is one pass along Angle by Radius and has no Aspect.
// A linked Aspect still shows (ui_node_item_hidden).
static bool advanced_blur_node_input_hidden(i32 mode, i32 input) {
	return input == ADVANCED_BLUR_INPUT_ASPECT && mode == ADVANCED_BLUR_MODE_DIRECTIONAL;
}

static bool advanced_blur_node_item_hidden(ui_node_t *node, ui_node_item_t kind, i32 index) {
	return kind == UI_NODE_ITEM_INPUT && advanced_blur_node_input_hidden((i32)make_image_node_button(node, ADVANCED_BLUR_BUTTON_MODE), index);
}

// Width and height of the image: Size 512, 1024, 2048 or 4096
static i32 advanced_blur_node_size(ui_node_t *node) {
	return make_image_node_size(node, ADVANCED_BLUR_BUTTON_SIZE);
}

// A linked Mask is a plane of its own; an unlinked one is a constant and is not captured
static i32 advanced_blur_node_mask_linked(ui_node_t *node) {
	return node->inputs->length > ADVANCED_BLUR_INPUT_MASK && parser_material_get_input_link(node->inputs->buffer[ADVANCED_BLUR_INPUT_MASK]) != NULL;
}

// Captures of one float per pixel: the Image, then a linked Mask
static i32 advanced_blur_node_capture_count(ui_node_t *node) {
	return advanced_blur_node_mask_linked(node) ? 2 : 1;
}

// Captured per pixel: the Image (capture 0) or the Mask (capture 1)
static char *advanced_blur_node_capture(ui_node_t *node) {
	char *v = parser_material_parse_value_input(node->inputs->buffer[make_image_node_capture_index() == 0 ? ADVANCED_BLUR_INPUT_IMAGE : ADVANCED_BLUR_INPUT_MASK], false);
	return string_tmp("float4(%s, %s, %s, 1.0)", v, v, v);
}

static void advanced_blur_node_params(ui_node_t *node, void *params) {
	advanced_blur_params_t *p = params;
	p->mode                   = (i32)make_image_node_button(node, ADVANCED_BLUR_BUTTON_MODE);
	p->radius                 = make_image_node_input_value(node, ADVANCED_BLUR_INPUT_RADIUS);
	p->angle                  = make_image_node_input_value(node, ADVANCED_BLUR_INPUT_ANGLE) * (f32)(3.14159265358979 / 180.0);
	p->aspect                 = make_image_node_input_value(node, ADVANCED_BLUR_INPUT_ASPECT);
	p->wrap                   = make_image_node_button(node, ADVANCED_BLUR_BUTTON_WRAP) != 0.0f ? 1 : 0;
	p->mask_linked            = advanced_blur_node_mask_linked(node);
	p->mask_value             = p->mask_linked ? 1.0f : make_image_node_input_value(node, ADVANCED_BLUR_INPUT_MASK);
}

// The blur runs on the GPU (result by reference, render/make_image_node.c): Image and Mask are captured into R32F render targets (not read back),
// every stage of the blur is one fragment pass of the shader below over a ping-pong pair of R32F targets (dx, dy, radius and the stage as uniforms),
// the last stage writes the RGBA128 result target, which IS the result texture. The stages and their per pixel rules (dense gaussian, boxes,
// the radius times the Mask of the pixel it writes) are those of the former cpu code (kept as the test reference in
// armorpaint-admin/automation/host/advanced_blur_gpu/reference.inc): taps are bilinear samples at pixel positions, four texel loads by hand
// (each exactly at a texel centre, so no sampler precision is lost, and Wrap Edges wraps or clamps the texel index).
//
// Box of width w along (dx, dy): up to ADVANCED_BLUR_BOX_DENSE pixels one pass of ceil(w) taps one pixel apart or closer (stage 0; stage 1
// copies). Wider boxes take two passes: a dense box of width / k (stage 0) and k taps width / k apart (stage 1), which together are the wide box.
// Gaussian of radius r (3 sigma, taps -r..r, weight exp(-4.5 u^2), u in -1..1): up to ADVANCED_BLUR_GAUSS_DENSE pixels one dense pass (stage 0, at
// most ADVANCED_BLUR_MAX_TAPS taps); larger: three boxes of width 2r / 3 (stages 1 to 6, two stages each), stage 0 copies.
#define ADVANCED_BLUR_STAGES_GAUSSIAN 7
#define ADVANCED_BLUR_STAGES_BOX 2

// Stage uniforms: bp = size, dx, dy, radius (pixels, before the mask); bq = stage, gaussian, mask linked, mask value; bw = wrap
static char *advanced_blur_node_shader_body(bool final) {
	char *common = string_tmp(
	    "float sz = constants.bp.x;"
	    "float2 pix = floor(tex_coord * sz);"
	    "float2 cuv = (pix + 0.5) / sz;"
	    "float m = constants.bq.w;"
	    "if (constants.bq.z > 0.5) { m = sample_lod(msk, sampler_point, cuv, 0.0).r; }"
	    "float r = constants.bp.w * max(m, 0.0);"
	    "r = min(r, %d.0 * sz);"
	    "float stage = constants.bq.x;"
	    "float width = 0.0;"
	    "float kind = 0.0;"   // 0 copy, 1 box, 2 dense gaussian
	    "float bstage = 0.0;"
	    "if (constants.bq.y > 0.5) {"
	    "  if (r >= 0.5) {"
	    "    if (r > %d.0) {"
	    "      if (stage > 0.5) { kind = 1.0; width = 2.0 * r / 3.0; bstage = (stage - 1.0) - 2.0 * floor((stage - 1.0) * 0.5); }"
	    "    }"
	    "    else if (stage < 0.5) { kind = 2.0; }"
	    "  }"
	    "}"
	    "else { kind = 1.0; width = 2.0 * r; bstage = stage; }"
	    "float n = 0.0;"
	    "float s = 0.0;"
	    "if (kind > 0.5 && kind < 1.5) {"
	    "  if (width < 1.0) { kind = 0.0; }"
	    "  else if (width <= %d.0) {"
	    "    if (bstage > 0.5) { kind = 0.0; }"
	    "    else { n = ceil(width); s = width / n; }"
	    "  }"
	    "  else {"
	    "    float k = ceil(sqrt(width));"
	    "    float w1 = width / k;"
	    "    if (bstage < 0.5) { n = max(floor(w1 + 0.5), 1.0); s = w1 / n; }"
	    "    else { n = k; s = w1; }"
	    "  }"
	    "}"
	    "if (kind > 1.5) { n = max(min(floor(2.0 * r) + 1.0, %d.0), 3.0); }"
	    "float acc = 0.0;"
	    "float wsum = 0.0;"
	    "if (kind < 0.5) { acc = sample_lod(src, sampler_point, cuv, 0.0).r; wsum = 1.0; }"
	    "else {"
	    "  float2 dir = float2(constants.bp.y, constants.bp.z);"
	    "  int cnt = int(n);"
	    "  for (int j = 0; j < cnt; j += 1) {"
	    "    float jf = float(j);"
	    "    float t = (jf - (n - 1.0) * 0.5) * s;"
	    "    float wg = 1.0;"
	    "    if (kind > 1.5) {"
	    "      float u = -1.0 + 2.0 * jf / (n - 1.0);"
	    "      t = u * r;"
	    "      wg = exp(-4.5 * u * u);"
	    "    }"
	    "    float2 p = pix + dir * t;"
	    "    float2 f = floor(p);"
	    "    float2 tt = p - f;"
	    "    float2 a = f;"
	    "    float2 b = f + 1.0;"
	    "    if (constants.bw > 0.5) { a = a - floor(a / sz) * sz; b = b - floor(b / sz) * sz; }"
	    "    else { float2 lo = float2(0.0, 0.0); float2 hi = float2(sz - 1.0, sz - 1.0); a = min(max(a, lo), hi); b = min(max(b, lo), hi); }"
	    "    float2 ca = (a + 0.5) / sz;"
	    "    float2 cb = (b + 0.5) / sz;"
	    "    float v00 = sample_lod(src, sampler_point, float2(ca.x, ca.y), 0.0).r;"
	    "    float v10 = sample_lod(src, sampler_point, float2(cb.x, ca.y), 0.0).r;"
	    "    float v01 = sample_lod(src, sampler_point, float2(ca.x, cb.y), 0.0).r;"
	    "    float v11 = sample_lod(src, sampler_point, float2(cb.x, cb.y), 0.0).r;"
	    "    float top = v00 * (1.0 - tt.x) + v10 * tt.x;"
	    "    float bot = v01 * (1.0 - tt.x) + v11 * tt.x;"
	    "    acc += wg * (top * (1.0 - tt.y) + bot * tt.y);"
	    "    wsum += wg;"
	    "  }"
	    "}"
	    "float v = acc / wsum;",
	    (i32)ADVANCED_BLUR_MAX_RADIUS_SIZES, ADVANCED_BLUR_GAUSS_DENSE, ADVANCED_BLUR_BOX_DENSE, ADVANCED_BLUR_MAX_TAPS);
	return string_tmp("%s%s", common, final ? "float c = clamp(v, 0.0, 1.0); output = float4(c, c, c, 1.0);" : "output = float4(v, 0.0, 0.0, 1.0);");
}

typedef struct advanced_blur_stage {
	f32  dx;
	f32  dy;
	f32  radius; // Pixels, before the mask
	bool gaussian;
	i32  stage;
} advanced_blur_stage_t;

// The stages of one blur along (dx, dy)
static i32 advanced_blur_node_add_stages(advanced_blur_stage_t *list, i32 count, f32 dx, f32 dy, f32 radius, bool gaussian) {
	i32 stages = gaussian ? ADVANCED_BLUR_STAGES_GAUSSIAN : ADVANCED_BLUR_STAGES_BOX;
	for (i32 st = 0; st < stages; ++st) {
		list[count++] = (advanced_blur_stage_t){.dx = dx, .dy = dy, .radius = radius, .gaussian = gaussian, .stage = st};
	}
	return count;
}

// The blur of the captured Image (captures[0], R32F) with the captured Mask (captures[1] when linked): one pass per stage over a ping-pong
// pair, the last into the RGBA128 result (R = G = B = the blurred image clamped to 0..1, A = 1). Returns the result target, NULL when memory runs out.
static gpu_texture_t *advanced_blur_node_gpu_bake(ui_node_t *node, i32 size, void *params, gpu_texture_t **captures, i32 count) {
	advanced_blur_params_t p = *(advanced_blur_params_t *)params;
	bool                   mask_linked = p.mask_linked != 0 && count > 1;
	f32                    radius      = (p.radius < 0.0f ? 0.0f : p.radius > ADVANCED_BLUR_MAX_RADIUS ? ADVANCED_BLUR_MAX_RADIUS : p.radius) * (f32)size;
	f32                    aspect      = p.aspect < 1.0f ? 1.0f : p.aspect > ADVANCED_BLUR_MAX_ASPECT ? ADVANCED_BLUR_MAX_ASPECT : p.aspect;
	f32                    ux          = cosf(p.angle);
	f32                    uy          = sinf(p.angle);
	bool                   gaussian    = p.mode == ADVANCED_BLUR_MODE_GAUSSIAN;
	advanced_blur_stage_t  stages[2 * ADVANCED_BLUR_STAGES_GAUSSIAN];
	i32                    n_stages = 0;
	if (p.mode == ADVANCED_BLUR_MODE_DIRECTIONAL) {
		n_stages = advanced_blur_node_add_stages(stages, n_stages, ux, uy, radius, false);
	}
	else {
		n_stages = advanced_blur_node_add_stages(stages, n_stages, ux, uy, radius * aspect, gaussian);
		n_stages = advanced_blur_node_add_stages(stages, n_stages, -uy, ux, radius, gaussian);
	}
	char *input_names[]   = {"src", "msk"};
	char *uniform_names[] = {"float4 bp", "float4 bq", "float bw"};
	char *mid_format[]    = {"R32"};
	char *final_format[]  = {"RGBA128"};
	shader_context_t *mid   = make_image_node_gpu_shader(advanced_blur_node_shader_body(false), input_names, 2, uniform_names, 3, mid_format, 1);
	shader_context_t *final = make_image_node_gpu_shader(advanced_blur_node_shader_body(true), input_names, 2, uniform_names, 3, final_format, 1);
	make_image_node_ping_pong_t pp = make_image_node_ping_pong_create(size, GPU_TEXTURE_FORMAT_R32);
	gpu_texture_t *result          = gpu_create_render_target(size, size, GPU_TEXTURE_FORMAT_RGBA128);
	if (result != NULL && pp.targets[0] != NULL && pp.targets[1] != NULL) {
		gpu_texture_t *cur  = captures[0];
		gpu_texture_t *mask = mask_linked ? captures[1] : captures[0]; // Never read when the mask is not linked
		for (i32 i = 0; i < n_stages; ++i) {
			bool last = i == n_stages - 1;
			f32  u[9] = {(f32)size, stages[i].dx, stages[i].dy, stages[i].radius, (f32)stages[i].stage, stages[i].gaussian ? 1.0f : 0.0f, mask_linked ? 1.0f : 0.0f, p.mask_value, p.wrap != 0 ? 1.0f : 0.0f};
			gpu_texture_t *inputs[2]  = {cur, mask};
			gpu_texture_t *targets[1] = {last ? result : make_image_node_ping_pong_dst(&pp)};
			make_image_node_gpu_pass(last ? final : mid, targets, 1, inputs, 2, u);
			if (!last) {
				make_image_node_ping_pong_swap(&pp);
				cur = make_image_node_ping_pong_src(&pp);
			}
		}
	}
	else if (result != NULL) {
		gpu_delete_texture(result);
		result = NULL;
	}
	make_image_node_gpu_shader_free(mid);
	make_image_node_gpu_shader_free(final);
	make_image_node_ping_pong_free(&pp, NULL);
	return result;
}

static image_node_type_t advanced_blur_node_image_type = {
    .type          = "TEX_ADVANCED_BLUR",
    .result_format = GPU_TEXTURE_FORMAT_RGBA128,
    .result_by_reference = true,
    .params_size   = sizeof(advanced_blur_params_t),
    .size          = advanced_blur_node_size,
    .capture       = advanced_blur_node_capture,
    .capture_count = advanced_blur_node_capture_count,
    .capture_format = IMAGE_CAPTURE_R32F,
    .smooth_sample = true,
    .params        = advanced_blur_node_params,
    .gpu_bake      = advanced_blur_node_gpu_bake,
};

char *advanced_blur_node_value(ui_node_t *node, ui_node_socket_t *socket) {
	return string_tmp("%s.r", make_image_node_sample(node));
}

char *advanced_blur_node_vector(ui_node_t *node, ui_node_socket_t *socket) {
	return parser_material_to_vec3(advanced_blur_node_value(node, socket));
}

void advanced_blur_node_init() {

	ui_node_t *advanced_blur_node_def =
	    ALLOC_INIT(ui_node_t, {.id     = 0,
	                           .name   = _tr("Advanced Blur"),
	                           .type   = "TEX_ADVANCED_BLUR",
	                           .x      = 0,
	                           .y      = 0,
	                           .color  = NODES_MATERIAL_IMAGE_COLOR,
	                           .inputs = any_array_create_from_raw(
	                               (void *[]){
	                                   make_image_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                                   make_image_node_socket(_tr("Radius"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.01), 0.0, 0.25, 1000),
	                                   make_image_node_socket(_tr("Angle"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(0.0), 0.0, 360.0, 10),
	                                   make_image_node_socket(_tr("Aspect"), "VALUE", NODES_MATERIAL_VALUE_COLOR, f32_array_create_x(1.0), 0.0, 10.0, 100),
	                                   make_image_node_socket(_tr("Mask"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(1.0), 0.0, 1.0, 100),
	                               },
	                               5),
	                           .outputs = any_array_create_from_raw(
	                               (void *[]){
	                                   make_image_node_socket(_tr("Image"), "VALUE", NODES_MATERIAL_IMAGE_COLOR, f32_array_create_x(0.0), 0.0, 1.0, 100),
	                               },
	                               1),
	                           .buttons = any_array_create_from_raw(
	                               (void *[]){
	                                   make_image_node_setting(_tr("Size"), "ENUM", 1, MAKE_IMAGE_NODE_SIZE_ITEMS),
	                                   make_image_node_state_button(),
	                                   make_image_node_setting(_tr("Mode"), "ENUM", 0, "Gaussian\nBox\nDirectional"),
	                                   make_image_node_setting(_tr("Wrap Edges"), "BOOL", 1, NULL),
	                               },
	                               4),
	                           .width = 0,
	                           .flags = 0});

	any_array_push(nodes_material_filter, advanced_blur_node_def);
	any_map_set(parser_material_node_values, "TEX_ADVANCED_BLUR", advanced_blur_node_value);
	any_map_set(parser_material_node_vectors, "TEX_ADVANCED_BLUR", advanced_blur_node_vector);
	any_map_set(ui_nodes_hidden_rules, "TEX_ADVANCED_BLUR", advanced_blur_node_item_hidden);
	make_image_node_sections(advanced_blur_node_def, NULL, 0, 1); // General: the settings and Image
	make_image_node_register(&advanced_blur_node_image_type);
}
