<!-- Written by a design agent before the review. DECISIONS.md records what was accepted or changed; where they differ, DECISIONS.md wins. -->

# UDIM model: Object and Tile as independent layer selectors

Status: design only. Base: `main` @ 22097dfdd. All line numbers are from `main` unless they say otherwise.

## 0. Corrections to the brief (verified in code)

| Brief says | Code says |
|---|---|
| "Current checkout is `main`" | The working tree is on `custom` (a4b049b5b = main + c68da65dc iron_obj uvtiles fix). This doesn't matter for the design. c68da65dc becomes obsolete once the iron_obj UDIM block is rewritten (§2.3). |
| "Mesh delete remaps masks in ui/tab_meshes.c" | On main, `tab_meshes_draw_context_menu_delete` (tab_meshes.c:640-683) does **not** remap `object_mask`. It calls `tab_meshes_sort_hierarchy()` after `array_remove`, so `old_order` is captured after the removal and only reorders get remapped (tab_meshes.c:271-318). Masks above the deleted index silently move to the next object, or become an "atlas" mask when `> N`. The remap-on-delete exists only on the archive branch (0ddf0528d). Main has the bug. |
| "undo of the delete restores coverage" | Mesh delete has **no undo at all** on main. There is no mesh `HISTORY_ACTION_*` in history.c:6-86. The design makes coverage restore automatic *if* mesh-delete undo is ever added (the restored object keeps its persistent id). Adding mesh-delete undo is out of scope (§10, open question). |
| "`make_mesh_write_object_mask()` in render/make_mesh.c" | That function exists only on the archive branch (make_mesh.c:94 there). On main the OR chain is inline at make_mesh.c:299-317 and baked with **runtime** `object_t.uid` values (engine.c:127, a session counter). |
| "per-tile project resolution POC: `util_mesh_atlas_res_x/y()`" | Archive only (8a18795bc). On main the project resolution *is* the atlas resolution. Tiles get res/stride each, and tile-object layers are stored tile-local at full res (`layers_udim_tile_slot`, util_layer.c:1264-1271), so main mixes resolutions between layers. |
| obj UDIM import "splits per tile" | True, but the UDIM branch of `import_obj_parse` (import_obj.c:15-56) parses **only the first object**: there is no `has_next` loop. It also assigns a tile to each *uv index* by its last-visited triangle (iron_obj.c:672-684), so a uv shared across a tile border gets one wrong tile. Non-UDIM obj import wraps each vertex with `uv - (int)uv` (iron_obj.c:693-700). That breaks straddling triangles. Non-UDIM fbx import casts `uv*32767` to short with no wrap (io_fbx.c ~l.100), so UVs > 1 overflow. |
| Export file names `.1001` | Main writes `<f>_<channel>.1001.png` (export_texture.c:260-263 puts `object_name` ".1001" into `ext`). The archive commit 3cae084b8 changes it to `_1001`. |

Other facts this design relies on (verified):
- Vertex data is one interleaved buffer. The stride comes from the **pipeline** input layout (metal_gpu.m:509-538, same in vulkan/d3d12/webgpu). So a mesh vertex array that a pipeline doesn't declare corrupts that pipeline. Optional attributes (`col`, `tex1`) are only safe because every relevant `make_*` adds them conditionally (make_mesh.c:115-121, make_paint.c:178-184). `make_depth.c` and `make_sculpt.c` do not. A new vertex attribute would have to be threaded through every paint-object pipeline. That is why §2 avoids one.
- `armpack_decode` is **positional**: keys are skipped (iron_armpack.c:225), so struct layout == file field order. `project_t`, `layer_data_t` and `mesh_data_t` can only change with a version bump. Old files load through the key-based `import_legacy.c` path (`import_arm_is_old`, import_legacy.c:100). `mesh_data_t` (engine.h:99-106) is also decoded from engine scene files, so **no fields may be added to it**. Runtime additions go into `mesh_data_runtime_t` (`md->_`).
- The merged object is not in `scene_meshes` (util_mesh.c:509). The viewport always draws individual paint objects. The merged object is used for painting (`render_path_paint_target`, render_path_paint.c:137-145) and path tracing.
- There are no texture arrays in iron_gpu (iron_gpu.h), and the 16384 max texture side is enforced in the POC.
- Shaders are regenerated whenever a layer mask changes (tab_layers.c:248 → `make_material_parse_mesh_material`). Baking selections into shader code is the established pattern.

---

## 1. Summary of the recommendation

1. **Tile is a per-triangle property of the geometry.** It is computed at import from the triangle's UV centroid and stored per vertex (vertices are split so a triangle never shares a vertex with another tile). Straddling triangles are clamped into their centroid tile and reported.
2. **GPU `tex` holds atlas-space UVs** for every paint object, and a separate copy of the tile-local UV plus the tile index is kept in `mesh_data_runtime_t`. The tile of any fragment is `slot = floor(tex * stride)`. The per-object `_atlas_transform` uniform, the tile-local layer storage and the "udim layer vs tile layer" split all disappear. No new vertex attribute, so no pipeline changes. Precision equals today's merged mesh (≥ 2 sub-steps per atlas texel at 16384).
3. **One atlas per project, keyed by an explicit, persisted, sorted project tile list.** It keeps today's compact stride×stride packing (max 8×8 = 64 tiles). The project resolution means **per tile**: atlas = res × stride, clamped to 16384. All layers share the layout. The tile list grows on import and never shrinks implicitly. A relayout (copying, not scaling) runs when it changes.
4. **Layer coverage = (object ∈ objects ∨ all_objects) ∧ (tile ∈ tiles ∨ all_tiles).** Objects are stored by a **persistent paint id** (monotonic per project, saved), tiles by **UDIM id** (1001…). Viewport: per-draw `_object_pid` uniform plus baked id/slot range tests. Paint: merged subset of covered objects, plus a slot discard. Flatten, path trace and export: a UV *footprint mask* per restricted layer.
5. **Export:** Visible / Selected / Per Object / Per Tile / Per Object & Tile. Tile files are `<f>_<channel>_1001.png`, cropped at native tile size. Export > Atlases is removed. Old atlas groups migrate into layer object sets.
6. **Save format "24"** with new layer and project fields. Legacy files (≤ 23) migrate in `import_legacy.c`: object_mask, atlas groups and name-based tile objects (with tile-local layer textures remapped into the atlas and the resolution divided by stride).

---

## 2. Where the tile comes from

### 2.1 Per-triangle tile, per-vertex storage

- `tile(tri) = (floor(cu), floor(cv))` where `(cu, cv)` is the UV centroid in UDIM space (u ∈ [0,10), v ≥ 0), so `id = 1001 + tu + 10·tv`. This is the rule `get_tile` already uses (iron_obj.c:178-188). Keep it and clamp `tu` to 0..9 and `tv` to 0..99. Negative UVs fall into 1001.
- Per corner: `local = uv − (tu, tv)`, then `tex_local = (local.u, 1 − local.v)` (the v flip importers already do), clamped to [0,1].
- A vertex used by triangles of two tiles is **duplicated** so each copy has one tile. Corner-based importers (obj before weld, fbx) get this for free if the weld key includes the tile. Indexed importers (gltf) need the helper in §2.3 to split such vertices.
- **Straddling triangles** (corners in more than one integer tile): assigned to the centroid tile, local UVs clamped to [0,1] (the texture is distorted on those triangles only). The count goes to the console: "N triangles cross UDIM tile borders and were clamped". Clamping guarantees no triangle ever rasterizes into a neighbour slot. Substance and Mari also treat islands across tile borders as unsupported.

### 2.2 In-memory representation (recommended option "A: atlas-space tex")

Per `mesh_data_t`, in `mesh_data_runtime_t` (engine.h:84-97; runtime-only, safe to extend):
```c
i16_array_t *udim_tex;  // tile-local uv (short, 0..32767), NULL = mesh never needed atlas remap
u8/i16_array_t *udim_tile; // per-vertex tile index = id - 1001, NULL = all 1001
```
- `vertex_arrays["tex"]` (what the GPU and every CPU consumer reads) = **atlas-space UV**: `(slot_xy + local) / stride` in the same short2norm encoding. This is exactly what `util_mesh_build_merged_data` already writes for the merged mesh (util_mesh.c:396-405).
- Non-UDIM project (tile list = {1001}, stride 1): `tex == local`, the side arrays stay NULL, and nothing changes versus upstream.
- A single tile other than 1001 (for example everything in 1002): stride 1, `tex == local`, tile list = {1002}. Only export naming sees the tile.
- `util_udim_ensure(md)`: if `udim_tile` is NULL but the project is UDIM, derive it from the atlas `tex` (triangle centroid → slot → tile id; local = tex·stride − slot). This is the fallback for any code that builds new geometry without propagating the side arrays: mesh nodes (nodes_mesh.c:150-223), subdivide/modifiers (`_util_mesh_subdivide`), sculpt remesh (make_sculpt.c:1479), join. Because the atlas tex encodes the slot, tile info is always recoverable, just quantized.

Why not the alternatives:
- *New vertex attribute "tile":* full precision, but every pipeline that draws paint objects must declare it, including the ones that don't even declare `col` today (make_depth.c, make_sculpt.c, decal, particle, picking). One miss means corrupted geometry on some backends. Rejected as too risky.
- *Absolute UDIM UVs in `tex` with `scale_tex`:* 32767/tiles_u steps per tile (≈ 3276 for 10 across) is worse than the atlas, and every shader would need `fract`/`floor` logic. Rejected.
- *Per-object tile (main today):* violates requirement 1.

Cost of A: when the tile list changes (stride or slot assignment), every mesh's `tex` is rebuilt from `udim_tex` + `udim_tile`, the GPU buffers are rebuilt (`mesh_data_build_vertices`), the merged mesh is rebuilt, the util_uv caches are invalidated, and the layer textures are relaid out (§4.3). This only happens on import or when a tile is explicitly removed.

### 2.3 Imports

A new shared helper, `util_udim_split_raw(raw_mesh_t *raw, float *uv /*per vertex, UDIM space*/, mode)`, in a new file `paint/sources/util/util_udim.c` (add it to main.c's unity includes and to functions.h). `raw_mesh_t` (iron_obj.h:9-30) gains `struct u8_array *tilea` (per vertex tile index), and `udims/udims_u/udims_v` go away.

- **Mode** (replaces "Split By: UDIM Tile", box_import_mesh.c:33-45, `SPLIT_TYPE_UDIM` enums.h:39): import option "UDIM tiles: Auto / On / Off", default Auto.
  - Off: today's wrap behaviour.
  - On: tiles as in §2.1.
  - Auto: On if more than one tile is used **and** no triangle straddles a tile border (epsilon 1e-4). Otherwise Off. This keeps tiling-UV meshes (UVs 0..4 meant to repeat) on the old wrap path.
  - Keep enum value 2 unused, or map it to Object, so old configs don't break.
- **OBJ** (iron_obj.c:629-702): replace the `udim` block. Compute the per-triangle tile and the per-corner local UV while writing `texa` (vertices are still per corner at that point: `pos_indices.length` corners, iron_obj.c:567-583). Add the tile to the `obj_weld` key (iron_obj.c ~230-280: `p2/q2/t2` comparison, add `tile`). Delete the per-tile index splitting (`part->udims`, iron_obj.c:647-666, 290-297) and the `uvtiles` normalisation (iron_obj.c:669-688; c68da65dc's fix becomes dead). Fix the multi-object case: `import_obj_parse` no longer needs a separate UDIM branch (import_obj.c:15-56 removed), so the normal `has_next` loop runs for every mode.
- **FBX** (io_fbx.c): delete `build_udim_split` (l.215-~300, `MAX_UDIM_TILES`) and the `plugins_split_by == 2` branches (l.518-524). The per-corner float UVs (`tf->data`, l.~100) go through the same helper. Also fix the overflow in Off mode (wrap like obj).
- **glTF** (io_gltf/cgltf.c) and blend (import_blend_mesh.c): optional in the first pass (they stay on Off behaviour), then the same helper with vertex splitting.
- `import_mesh_raw_mesh` (import_mesh.c:338-358) moves `raw->tilea` and the local `texa` into `md->_->udim_tile` / `md->_->udim_tex`, then calls `util_udim_tiles_add(...)` (§4.1). `tex` gets atlas values via `util_udim_apply_layout(md)`.
- Mesh **export** (export_obj.c, export_mesh.c, fbx export if any) must write UDIM UVs: `udim_tex` + tile offset, or `tex` if NULL. Otherwise exported meshes would carry atlas UVs.

### 2.4 Code to remove

`util_mesh_udim_tile(name)` (util_mesh.c:151-168) stays **only** for legacy migration (§3.5). `_util_mesh_udim_build_tiles` (name scan, l.170-196), `_util_mesh_atlas_slot_for_object` (l.220-225), `util_mesh_atlas_slot(object)` (l.236-244), `util_mesh_udim_layer` (l.212-218), `layers_udim_tile_slot` (util_layer.c:1263-1271), the `_atlas_transform` uniform (uniforms.c:226-236) and its users (make_mesh.c:142-149, make_paint.c:257-262/374, make_texcoord.c:146-153 simplify to `input.tex`), the per-object slot remap in the merged build (util_mesh.c:396-405 becomes a plain copy) and in bake_maps (bake_maps.c:648-659). Upstream's f1fa2d8c9 conditions `|| util_mesh_udim_active()` (render_path_raytrace.c:561, util_layer.c:642) are replaced by the subset-merge logic of §5.3.

---

## 3. Data model and save format

### 3.1 Runtime

`mesh_object_t` (engine.h; runtime-only, built by `mesh_object_create`) gains `i32 paint_id;` (0 = not a paint object). Alternative if base edits are unwanted: an `i32_map` uid→pid in `project_runtime`. The struct field is simpler and survives reorder.

`slot_layer_t` (types.h:51-91): replace `i32 object_mask;` (l.65) with
```c
bool         all_objects;  // default true
i32_array_t *objects;      // persistent paint ids, never NULL (empty array)
bool         all_tiles;    // default true
i32_array_t *tiles;        // UDIM ids (1001..), never NULL
```
- Masks and filters inherit from the parent, as `slot_layer_get_object_mask` does today (slot_layer.c:738-740). That becomes `slot_layer_coverage_owner(l)`.
- Layer groups keep their own sets, which apply to the group row only, the same as main today (open question §10: whether children should intersect with the group).
- Requirement 4: an empty list never means all. Requirement 6: the list is never cleared when objects disappear. Requirement 7: new objects or tiles are never inserted into lists.

`project_t` (types.h:~640-705): remove `atlas_objects`, `atlas_names` (l.690-691) and append, in this order (positional!):
```c
i32                    next_object_id; // monotonic, never reused
struct i32_array      *mesh_ids;       // parallel to mesh_datas / paint_objects: persistent id
struct i32_array      *udim_tiles;     // sorted project tile list, NULL/empty = {1001}
struct any_array      *mesh_tiles;     // parallel to mesh_datas: u8/i16 tile index per vertex or NULL
```
Put **all** of these in the first format change (step A2) so the v24 layout is final and dev files don't break between steps.

`history_step_t` (types.h:~560-600): replace `i32 layer_object` (l.582) with the four coverage fields, deep-copied arrays freed in `history_step_free`. Rename `HISTORY_ACTION_LAYER_OBJECT` → `HISTORY_ACTION_LAYER_COVERAGE` (or keep the name). `history_push` (history.c:920-933) copies them. `history_swap_layer_props` (history.c:202-214) swaps them. `slot_layer` duplicate (slot_layer.c:390) and every layer struct copy/swap (`slot_layer_swap`, `history_swap_merge`) must deep-copy the arrays, not alias them.

### 3.2 Persistent object ids

- New object (import add/replace, `import_mesh_add_mesh` / `import_mesh_make_mesh`, `util_mesh_duplicate_object` util_mesh.c:2766-2785, mesh-node outputs, primitives at startup): `paint_id = ++g_project->next_object_id`.
- Duplicate: new id, **not** added to any explicit set (requirement 7). `_util_mesh_shift_object_masks` (l.2750-2763) is deleted.
- Reorder or hierarchy sort: nothing to do. `tab_meshes_remapped_mask` and its loop (tab_meshes.c:269-275, 313-318) are deleted.
- Delete: nothing to do. The id dangles. Layers whose sets end up with no live id cover nothing (requirement 6).
- Join (`util_mesh_merge_geometry_objects`, util_mesh.c:789-842): the main object keeps its id. For every layer, if the set contains any removed id, add the main id (union, so coverage of the merged geometry is preserved). Remove the positional rewrite (l.831-838) and the `atlas_objects` splice (l.822-824).
- Merge-all (util_mesh.c:~760-788, currently forces every layer to Shared at l.781-783): same union rule with the surviving id instead of resetting to Shared.
- `history_step_t.object` (positional selection, history.c:915) can stay. It is unrelated to coverage.

### 3.3 Save format "24"

- `globals.h:10` `manifest_version_project = "24"`.
- `layer_data_t` (types.h:~730-760): replace `object_mask` (l.741) with `all_objects, objects, all_tiles, tiles`, in this order in both the struct and `util_encode` (util_encode.c:395-396). Write: export_arm.c:258. Read: import_arm.c:1077. Defaults on create: slot_layer.c:78.
- Project fields as in §3.1. Encode them in util_encode.c after the existing project fields (and delete the atlas pair at l.505-508). Write `mesh_ids` from `paint_objects[i]->paint_id`. Write `mesh_tiles[i] = data->_->udim_tile`.
- `util_encode_mesh_datas` (util_encode.c:18-48): for the `tex` array, write `md->_->udim_tex` when non-NULL, so the **file stores tile-local UVs** and the layout lives only in `udim_tiles`. Linked mesh datas (export_arm.c:14-20) store NULL tiles.
- Load (import_arm.c): set `paint_id` from `mesh_ids`, `next_object_id`, then the tile list, then each mesh's side arrays, then `util_udim_apply_layout` for all meshes **before** layer textures are created, so the atlas size (res × stride) is known.

### 3.4 Undo

- Coverage edits use one history step that snapshots all four fields (§3.1). Toggling a fill layer's coverage keeps today's "refill on next frame" behaviour (history.c:208-213, tab_layers.c:249-252).
- Coverage is stored by id, so undoing a layer delete or merge, or (future) a mesh delete, restores coverage automatically.
- **Relayout of the atlas invalidates undo textures** (`texpaint_undo*` hold the old layout). Call `history_reset()` (history.c:872) on relayout, with a notice. Relayout only happens on import or tile removal.

### 3.5 Legacy migration (`import_arm_from_map_to_arm`, import_legacy.c)

Do this in the legacy path (it already reads by key, l.196, l.259-260). Then finish what needs live objects in a post-load hook in import_arm.c (after objects exist, around l.1120):
1. `mesh_ids = 1..N` in order, `next_object_id = N`.
2. Per layer `object_mask` (old N = number of paint objects):
   - `0` → all/all.
   - `1..N` → `all_objects=false, objects={id[mask-1]}`, all tiles.
   - `>N` → the atlas group: reproduce `project_get_used_atlases` (project.c:551-575) from `atlas_objects`/`atlas_names`, `objects` = ids of objects in that group, all tiles. If the group can't be resolved, use all/all.
3. **Name-based UDIM projects**: if more than one paint object name matches `util_mesh_udim_tile` (>1 distinct ids), then
   - set `udim_tiles` = the sorted distinct ids (identical to the old slot order, util_mesh.c:170-196),
   - give each such object's mesh `udim_tile = id-1001` for all vertices and `udim_tex = tex`.
   - The old atlas layout equals the new one (same sorted ids, same `_util_mesh_atlas_stride_for`), so **shared layer pixels are already valid**.
   - The old project resolution was the atlas size, so set the per-tile res = old_res / stride. The atlas texture size is then unchanged.
   - Layers whose old mask was a single tile object were stored **tile-local at full res** (util_layer.c:1263-1271). Remap them into their slot with the existing `layers_udim_remap(..., to_tile=false)` (util_layer.c:1251-1261, which downsamples by stride) and set `objects={that id}`.
   - Optionally offer "Join UDIM tile objects" (Meshes tab menu) that joins `X.1001..X.1004` into `X` with the union rule of §3.2.
4. `mesh_nodes_objects` (types.h:705) is also positional. Leave it as is (out of scope, noted).

---

## 4. Texture storage

### 4.1 Project tile list and layout (`util_udim.c`, replacing util_mesh.c:135-245)

```c
i32  util_udim_count();                 // tiles in project list (>=1)
i32  util_udim_tile(i32 slot);          // id of slot
i32  util_udim_slot(i32 tile_id);       // slot of id, -1 if absent
i32  util_udim_stride();                // ceil(sqrt(count)), max 8
bool util_udim_active();                // count > 1 || tile != 1001  (UI/export naming)
i32  util_udim_res_x/y();               // per-tile res * stride, clamped 16384 (from 8a18795bc)
void util_udim_tiles_add(i32_array_t *ids);   // union, sorted; relayout if changed
void util_udim_tile_remove(i32 id);     // explicit, destructive, only when unused
void util_udim_apply_layout(mesh_data_t *md); // tex = atlas(udim_tex, udim_tile)
```
- The list is **persisted and never shrinks implicitly**. Deleting the last mesh on tile 1003 keeps slot 1003 (the content survives a re-import). The Tiles panel (§7.4) shows unused tiles and offers "Remove".
- More than 64 tiles: the import warns, and triangles in excess tiles are clamped to the last tile (main does the same with `tiles_spent`, util_mesh.c:186-209), with a visible error.

### 4.2 Resolution and memory: options compared

| Option | Atlas size | Quality | Memory per layer (3 RGBA8 textures) | Complexity |
|---|---|---|---|---|
| a) Shared res (main) | project res | each tile = res/stride (4 tiles @ 4k → 2k per tile) | same as non-UDIM | lowest |
| **b) Per-tile res (POC 8a18795bc)** | res×stride ≤ 16384 | native tile res up to 16384/stride | 4 tiles @ 2k → 4k atlas: 192 MB; @ 4k → 8k atlas: 768 MB | low, reuses POC |
| c) Per-tile textures / texture arrays | n × res² | native, no 16384 cap | exactly n tiles | no texture-array support in iron_gpu. Every paint, blend, flatten and undo pass would need per-tile loops. Rejected. |
| d) Per-layer sub-atlas (only that layer's tiles) | varies per layer | native | saves memory on tile-restricted layers | breaks every texel-to-texel layer op (merge down, apply mask, blend, undo). Rejected for now. |

**Recommendation: (b).** The resolution combo is labelled "Resolution (per tile)" when `util_udim_count() > 1`. Show the effective atlas size and an estimated memory figure (layers × 3 × atlas² × bpp, plus undo steps) in the project/layer resolution UI. Warn when the atlas would exceed 16384 and show the clamped per-tile res. Cherry-pick the call-site replacements from 8a18795bc (config_get_texture_res_x/y → util_udim_res_x/y in slot_layer.c, render_path_paint.c, render_path_raytrace*.c, bake_texture_node.c, ui_view2d.c, util_uv.c, util_select.c, util_cursor.c, tab_timeline.c, ui_header.c, uniforms.c, project.c, import_arm.c, export_texture.c). **Do not take its `layers_resize()` on stride change** (8a18795bc util_mesh.c `_util_mesh_atlas_resize_layers`). Scaling is wrong when tiles change slots.

Precision check: 32767 short steps over an atlas of at most 16384 texels gives ≥ 2 steps per texel, the same as today's merged-mesh painting.

### 4.3 Relayout (`util_udim_relayout(old_tiles, old_stride)`)

For each layer texture (texpaint, nor, pack, masks, plus sculpt if it is UV-space; it is not, it is per vertex):
1. Allocate the new atlas (`util_udim_res`).
2. Clear it.
3. For each tile in the old list that is also in the new list, blit the old slot rect to the new slot rect at 1:1, or scaled if the per-tile res changed because of the 16384 clamp. Generalise `_layers_udim_copy` (util_layer.c:1232-1248) to slot→slot.
4. Swap the textures and rebuild their render-target registrations (the same mechanics as `slot_layer_resize_and_set_bits`, slot_layer.c:~459).

Then `util_udim_apply_layout` on all meshes, a merged rebuild, a util_uv cache reset and `history_reset()`.

---

## 5. Rendering

### 5.1 Coverage helpers (util_udim.c / util_layer.c)

```c
bool layer_covers_object(slot_layer_t *l, mesh_object_t *o);
bool layer_covers_tile(slot_layer_t *l, i32 tile_id);
bool layer_is_restricted(slot_layer_t *l);       // !all_objects || !all_tiles
bool layer_covers_nothing(slot_layer_t *l);      // no live id in objects (when !all_objects) or no project tile in tiles (when !all_tiles)
mesh_object_t_array_t *layer_coverage_objects(slot_layer_t *l); // live paint objects in set (all if all_objects)
void layer_write_coverage_test(node_shader_t *kong, slot_layer_t *l); // emits "if (...) {" ; returns false if unrestricted
```

### 5.2 Viewport (make_mesh.c)

- New int uniform `_object_pid` in `uniforms_ext_i32_link` (uniforms.c:4-12) returns `((mesh_object_t*)object->ext)->paint_id`, or 0 when the object is not a paint object.
- If any visible layer is tile-restricted, emit once per fragment: `int udim_slot = int(floor(tex_coord.x * atlas_stride)) + int(floor(tex_coord.y * atlas_stride)) * int(atlas_stride);` using the existing `_atlas_stride` float link (uniforms.c:99-101). On a non-UDIM project stride = 1 and the test is skipped entirely.
- Replace make_mesh.c:299-317 with `layer_write_coverage_test`:
  - object part: OR of **ranges** of persistent ids, `(object_pid >= a && object_pid <= b) || …`, built from the sorted live ids in the set. This means no bitmask limit (the 32/64 object question goes away) and short code even for "all but one".
  - tile part: ranges of slots `udim_slot == s || …`.
  - The shader is already regenerated on every coverage change. Persistent ids don't change when objects are recreated, so no regeneration is needed on undo or reload, unlike the current runtime-uid baking.
- `layer_covers_nothing(l)` → the layer emits no code at all (skipped).
- Translucency: `transluc_opac` is updated inside the coverage block (make_mesh.c:363-364), so an object a translucent bottom layer doesn't cover stays opaque. That is the desired behaviour.
- `tex_coord_layer`/`tex_coord_atlas` distinction (make_mesh.c:142-149, 320): gone, everything uses `tex_coord` (atlas) or `tex_coord1` for `uv_map == 1`. `tex1` layers keep their own 0-1 space. The tile **test** still uses primary-UV tile, which is a property of the geometry.

### 5.3 Painting, fill, bake tool, symmetry

- `layers_set_object_mask` (util_layer.c:578-620) becomes `layers_update_paint_set()`:
  - if the active layer covers nothing, painting is disabled (status hint "Layer covers no objects");
  - else if all_objects, use the normal full merge;
  - else `util_mesh_merge(layer_coverage_objects(l) ∩ visible)`. This reuses the existing subset-merge path (`merged_object_is_atlas`, util_mesh.c:472; rename it `merged_object_is_subset`).
  - Drop the single-object non-merged special case (`context_object_mask_used`, context.c:354-356). Painting always goes through the merged subset. This removes a whole class of "object mask used" branches (util_uv.c:25-28 and l.121-125, uniforms.c:105-117, import_arm.c:1122/1261).
  - Cache the last merge key (sorted ids + mesh version) so switching between layers with the same set doesn't re-merge.
- make_paint.c: after `tpos`, if the layer is tile-restricted, emit the slot discard on the interpolated `tex` (atlas) using the same slot-range code. This covers brush, fill (mesh, submesh, face, UV island), decal, particle, the blur/smudge accumulation and the bake tool, since they all go through make_paint.
- Mesh fill picking (`render_path_paint_draw_fill_object`, render_path_paint.c:182-200) already requires the picked object to be in the merged target. That is correct for sets.
- Symmetry (render_path_paint.c:744-~790) redraws the same target with mirrored transforms. The coverage test is per fragment, so nothing changes.
- 2D view painting draws `.Plane` (render_path_paint.c:131-133). It has `object_pid` 0, so only the tile test applies. Object restriction can't apply to pure UV painting, the same as main.
- Path-trace mode: the raytracer uses the merged object only when it isn't partial (render_path_raytrace.c:561-562). With subset merges for painting, replace upstream's `|| util_mesh_udim_active()` with an explicit full merge for the BVH, or make sure the existing partial fallback path runs (verify during B3).

### 5.4 Sculpt

Today the restriction is one contiguous vertex range (`_sculpt_mask_offset/_count`, uniforms.c:105-117, make_sculpt.c:513-518). Replace it with a per-layer **R8 coverage texture in sculpt-texel layout** (one texel per vertex, the same layout as `sculpt_object_texel_offsets`, make_sculpt.c:13-38). Build it on the CPU from (object id ∈ set) ∧ (vertex `udim_tile` ∈ set) whenever coverage, objects or tiles change. The shader samples it and discards. This fixes "sculpting on a tile-restricted layer covers the whole texture".

### 5.5 Footprint masks: viewport == export == path trace

The viewport tests coverage per fragment. Flatten (`layers_flatten`, util_layer.c:1308-1450, used by path trace render_path_raytrace.c:466/485 and lightmap bake render_path_raytrace_bake.c:119) and export (`export_texture_run_layers`, export_texture.c:238-…) work in texture space, where objects that share a tile overlap. On main they ignore object masks entirely. New:

- `layers_coverage_mask(l, object_filter, tile_filter)` renders into the shared temp R8 mask (`pipes_temp_mask_image`, multiplied into it if the layer already has masks).
  - tile-only restriction: fill the selected slot rects (exact, includes the padding gutter);
  - object restriction: rasterize the UV-space triangles of the covered objects (the make_paint `tpos` trick, or `util_uv`'s triangle-map pipeline) with the tile discard, then **dilate** by `g_config->dilate_radius`, reusing the existing dilate pass, so exports keep their padding;
  - `layer_covers_nothing` → skip the layer.
- Flatten and export call it for every restricted layer and pass the result as the `mask` already threaded through the merge pipelines (util_layer.c:1360-1430). Don't cache it: an R8 cache at 16k is 256 MB per entry. Regenerating means a few mesh draws per restricted layer per flatten.
- Known inherent mismatch: where two objects overlap **in UV within the same tile**, a texture can't distinguish them. Viewport (per object) and export or path trace (shared texels) differ there. Document it in the layer tooltip and here. Translucency edge: an uncovered area flattens to alpha 0, which the path tracer reads as transparent while the viewport shows it opaque. This already happens on main for unpainted areas (open question §10).

---

## 6. Bakes

- **Core bake through the paint pipeline** (make_paint, bake tool): atlas-space automatically, coverage via §5.3.
- **Raytraced and lightmap bakes** (render_path_raytrace_bake.c:225-262 rasterizes `ta * scale_tex * res` per object): atlas-space automatically. Size the targets with `util_udim_res` (8a18795bc does it).
- **Bake Maps plugin** (paint/plugins/bake_maps/bake_maps.c):
  - CPU path: delete the per-object slot remap (l.648-659). `bake_rays_read_vertex` (l.197-216) already reads atlas `tex`.
  - `bake_maps_start` (l.1652): `w,h` from the script are per tile, so the atlas = `w*stride` clamped (POC abde6fde8 `bake_maps_tile_stride` is reusable, but read stride from `util_udim_stride()`).
  - GPU path: `bake_maps_gpu_res`/`bake_maps_gpu_start` (l.1757-1771) use `util_udim_res_x/y` (POC).
  - `bake_maps_save` (l.1859-1888) writes the **atlas** file `<name>_<map>.png` (imported as a project texture for in-app use) **and** per-tile crops `<name>_<map>_1001.png`, not imported. That is POC abde6fde8's crop loop with `_%d` naming (3cae084b8) and the tile id from `util_udim_tile(slot)`.
  - Factor out `bake_maps_write_tiles(char *atlas_file)` so `bake_maps_inpaint_apply` (l.2529) re-crops the tiles after editing the atlas.
  - Memory note for the CPU path: `bake_rays_texmap` (i32/texel) plus the RGBA buffers at a 16k atlas are about 2 GB. Warn above 8k. A later optimisation is baking per tile (rasterize only that tile's triangles with local UV, BVH stays global).
- **Plugin script** (paint/assets/plugins/bake_maps.c): `update_material` (l.137-170) and `map_path` (l.267) keep using the atlas file, so the script barely changes. The problem is that a TEX_IMAGE node samples with the material texcoord, which for UDIM is tile-local-ish (`atlas_uv * stride`, make_texcoord.c:146-153 today). An atlas image would repeat per tile.
  - Add a TEX_IMAGE option "UV: Atlas" in the TEX_IMAGE parser (parser_material.c / nodes_material tex image) that samples with raw `tex_coord` (atlas) when the node's vector input is unlinked, and set it from `update_material` via a script call (`script_material_set_button`, or a new `script_material_set_atlas_uv(node)`).
  - Main is already broken here for UDIM: per-object tile-local UVs sample a per-tile region of nothing.
- Normal and Height CPU bakes being flat without a high poly is expected (as the brief says).

---

## 7. Export and UI

### 7.1 Export modes (enums.h:108-113, box_export.c:~130-140, export_texture.c:723-830)

| Mode | Iteration | Layers included | Mask | File |
|---|---|---|---|---|
| Visible | once | visible, not covering nothing | footprint for restricted layers | `<f>_<ch>.ext` (atlas when UDIM) |
| Selected | once | selected (group children) | footprint | `<f>_<ch>` (no object suffix any more; sets aren't single objects) |
| Per Object | each paint object o covered by ≥1 layer | layers covering o | footprint(o ∩ set ∩ tiles) for every layer, so the output is clean per object | `<f>_<object>_<ch>` |
| Per Tile (default when `util_udim_count()>1`) | each project tile t used by a mesh | layers with t ∈ tiles (or all) | footprint for object-restricted layers; crop slot t at native size | `<f>_<ch>_1001` |
| Per Object & Tile (new = 4) | each o, each tile o uses | layers covering o and t | footprint(o ∩ …) then crop | `<f>_<object>_<ch>_1001` |

- Crop code: cherry-pick from 8a18795bc (export_texture.c "Crop one udim tile out of the atlas at its native size", plus the `export_texture_size_x/y` statics).
- Naming: the tile suffix is appended after the channel, as `ext = "_1001" + ext` (3cae084b8). Per-tile export no longer depends on object names (export_texture.c:730-748, 312-320, 279-288 removed).
- Remove the atlas export branch (export_texture.c:773-821) and the Atlases tab UI (box_export.c:392-410).
- **What is lost with Atlases:** a named output grouping of several objects into one file set ("Atlas 1"), and the atlas entries in the layer filter. Layer object multi-select covers "which layers apply to which objects" but not "which objects share an output file".
  - In the UDIM model the idiomatic answer is: separate texture sets = separate tiles, so Per Tile export covers it, plus Per Object for one set per object.
  - If users need named multi-object groups on non-UDIM 0-1 meshes, add a later, small, **uid-based** "Export groups" list (name → ids). It is not needed for this branch (open question).

### 7.2 Layer panel (tab_layers.c:217-256, `tab_layers_combo_object`)

- Replace the single combo with two **multi-select buttons**: "Objects: All" / "Body, Eyes" / "3 objects", and "Tiles: All" / "1001, 1002" / "4 tiles". Each opens a popup (`ui_menu_draw` pattern as in box_export.c:343) with an "All" check at the top, then one check per live paint object (tab_layers_object_items logic l.193-215 without the atlas part), or one per project tile (show "1003 (unused)" for tiles no mesh uses).
  - Ticking items while All is on keeps the list but ignores it (lists are never cleared). Unticking All reactivates the list.
  - Dangling ids aren't listed. If only dangling ids remain, the button reads "No objects".
  - The Tiles button is hidden when `util_udim_count() == 1`.
- One history step per popup close, or per toggle. On change: `make_material_parse_mesh_material()`, a fill-layer refill (existing), `layers_update_paint_set()`.

### 7.3 Layer list

- Filter combo (tab_layers.c:1071-1072, `tab_layers_apply_filter` util_layer.c:622-647): items "All", objects (by pid), then tiles ("Tile 1001").
  - An object filter keeps today's behaviour of hiding the other objects in the viewport, and hides layers that don't cover the object.
  - A tile filter hides layers that don't cover the tile and doesn't change viewport visibility.
  - Store `g_context->layer_filter_object` (pid) and `layer_filter_tile` (id) instead of an index.
  - New layers created while a filter is active (util_layer.c:667, 791, 886) get `objects={pid}` or `tiles={id}`.
- `tab_layers_slot_hidden` (tab_layers.c:853) uses the coverage helpers.
- **No-objects indicator:** in the layer row, when `layer_covers_nothing(l)`, draw a small warning glyph (or dim the name) with the tooltip "Covers nothing: its selected objects or tiles no longer exist. Reselect or tick All."
- Merge down (util_layer.c:993-996, currently forces Shared): the result takes the **union** of objects and tiles (all if either side is all).

### 7.4 Meshes tab and tile list

- Row tooltip: the object's tiles ("1001-1004"), computed from `udim_tile`.
- Delete, duplicate, sort and join behave as §3.2. No mask remap code remains.
- New "Tiles" section (bottom of the Meshes tab, or a small panel under the resolution setting) lists the project tiles with: id, number of objects using it, per-tile res, and "Remove" for unused tiles (relayout + undo reset, confirmation box).
- 2D view (ui_view2d.c): draw a slot grid overlay with tile ids when UDIM is active.
- Scripts: `minic_api.c:681` (`MINIC_I(object_mask)`) and `script_layer_set_object_mask` (minic_impl.c:~890-922) become `layer_set_objects(ids,n,all)` / `layer_set_tiles(ids,n,all)`. Keep a compatibility `object_mask` setter (0 = all, k = {pid of object k}). Update `paint/assets/plugins/dev/test.c:635-637`.

---

## 8. File-by-file change list and implementation order

Every step builds and runs. Steps A* are useful without UDIM.

**A1 Persistent object ids** (no format change; ids assigned in order at load)
- base/sources/engine.h: `mesh_object_t.paint_id`.
- paint/sources/project.c / types.h: `project_t.next_object_id` (runtime only for now).
- io/import_mesh.c, util/util_mesh.c (duplicate, join, merge-all), startup.c, nodes_mesh.c (new objects): assign ids.
- uniforms.c: `_object_pid` int link.

**A2 Coverage sets + format v24** (big step, one coherent change)
- types.h (slot_layer_t, layer_data_t, project_t: add every new field including the tile ones as NULL, history_step_t), globals.h "24", util_encode.c, export_arm.c, import_arm.c, import_legacy.c (object_mask → sets, atlas groups → sets), slots/slot_layer.c (defaults, duplicate deep copy, `slot_layer_coverage_owner`).
- New util/util_udim.c (coverage helpers, tile list stub = {1001}); main.c include; functions.h.
- render/make_mesh.c (range test), make_sculpt.c + uniforms.c (sculpt coverage texture, replacing offset/count), util/util_layer.c (`layers_update_paint_set`, filter, new layer defaults, merge union), context.c (remove `context_object_mask_used`), util/util_uv.c, render/render_path_paint.c, history.c, ui/tab_layers.c (multi-select popups, indicator, filter), ui/tab_meshes.c (delete remap code removed), util/util_mesh.c (shift and remap removed, join union), minic_api.c / minic_impl.c, assets/plugins/dev/test.c.
- io/export_texture.c: Per Object uses sets (footprint masks can come in B4; until then just filter layers).

**A3 Remove Export > Atlases**
- box_export.c:392-410, export_texture.c:773-821, project.c:551-600 (`project_get_used_atlases`, `project_is_atlas_object`, `project_get_atlas_objects`), functions.h:783, util_mesh.c:822-824, tab_layers.c atlas items, util_layer.c:609-612/626-632.

**B1 Tile extraction at import** (format unchanged: v24 already has `udim_tiles`/`mesh_tiles`)
- base/sources/iron_obj.c/.h (per-corner tile + local UV, weld key, remove udims), paint/plugins/io_fbx/io_fbx.c (remove build_udim_split, use helper), util_udim.c (`util_udim_split_raw`), io/import_obj.c (single path), io/import_mesh.c (side arrays), ui/box_import_mesh.c + enums.h + context.c + plugins.c (UDIM Auto/On/Off instead of SPLIT_TYPE_UDIM), util_encode.c + import_arm.c (save/load `mesh_tiles`, write local tex), io/export_obj.c / export_mesh.c (write UDIM UVs).
- In this step the tile list is still {1001}, so meshes with more than one tile render overlapped. **Guard:** until B2 lands, Auto/On only stores tiles and keeps the layout at stride 1. Acceptable on the working branch.

**B2 Project tile list, atlas layout, per-tile res**
- util_udim.c (list, stride, `apply_layout`, `relayout`, `ensure`, `res_x/y`).
- util_mesh.c (delete the name-based tile code l.135-245; merged build copies tex l.396-405).
- uniforms.c (delete `_atlas_transform`).
- make_mesh.c / make_paint.c / make_texcoord.c (atlas branches → `input.tex`; texcoord keeps `tex*stride` for tile-repeating materials).
- util_layer.c (delete `layers_udim_tile_slot`, `util_mesh_udim_layer` users; generalise `_layers_udim_copy` to slot→slot).
- The resolution call sites from 8a18795bc.
- render_path_raytrace.c:561 and util_layer.c:642 (revert f1fa2d8c9 conditions to subset logic).
- history.c (reset on relayout).

**B3 Tile coverage in shaders + Tiles UI**
- make_mesh.c (udim_slot test), make_paint.c (discard), sculpt coverage includes tiles, tab_layers.c Tiles popup and filter, tab_meshes.c Tiles section, ui_view2d.c grid.

**B4 Footprint masks** (util_layer.c flatten, export_texture.c, dilate reuse). This gives viewport == path trace == export.

**B5 Export modes** (enums.h new mode 4, box_export.c, export_texture.c crop + `_1001` naming from 8a18795bc/3cae084b8).

**B6 Bakes** (bake_maps.c: remove remap, atlas res, `bake_maps_write_tiles`, inpaint re-crop; TEX_IMAGE atlas-UV option in nodes_material/parser_material.c; assets/plugins/bake_maps.c sets it in `update_material`).

**B7 Legacy UDIM migration** (import_legacy.c / import_arm.c post-load: tile arrays from names, res/stride, tile-local layer remap) and the optional "Join UDIM tile objects".

### Reusable from `archive/custom-name-based-udim`

| Commit | Reuse |
|---|---|
| 8a18795bc per-tile res | **Yes:** `_util_mesh_atlas_res`, `util_mesh_udim_tile_id`, all `config_get_texture_res` → atlas res call-site swaps, the export crop block. **No:** `_util_mesh_atlas_resize_layers` (it scales instead of relaying out). |
| abde6fde8 bake per tile | **Yes:** `bake_maps_tile_stride`, `bake_maps_import_file`, the crop loop (change the name format to `_%d`, read stride from util_udim). |
| 3cae084b8 `_1001` naming | **Yes** (simplify: the tile is passed as an int, no dotted-name parsing). |
| 0f90eb6b2 / 5a004d01c obj/fbx split by tile | **No** (splitting objects is exactly what we're removing). Small pieces (the fbx per-corner UV loop, locale strings) may help. |
| 046d923f3, 29a6020b9, decbfefe9, 31bc6845f, f7576f435, eb92a675f | **No as code** (negative tile ids and name-based groups). Use them as a checklist of touch points: merged paint set follows visible members (→ §5.3), make_mesh group condition (→ §5.2), restrict mesh fill and bakes (→ §5.3), export guards (→ §7.1), combo entries (→ §7.2). |
| 0ddf0528d delete remap | **No** (stable ids make it unnecessary). It does document the main bug in §0. |
| f1a5d9faa, fcbafd812 duplicate naming | **No** (names carry no meaning any more). |
| c68da65dc (custom) | Obsolete after B1. |

---

## 9. Risks

1. **Atlas-space `tex` leaks into consumers that expect 0-1 per object**: mesh export (handled in B1), mesh nodes (`util_udim_ensure` fallback), any plugin reading `tex`, the UV overlay (fine: atlas is what is painted), `make_mesh_preview`. Grep every `"tex"` / `vertex_arrays->buffer[2]` read during B2.
2. **Relayout correctness**: every layer texture kind (masks R8, 8/16/32 bit formats, `texpaint_preview`, fill layers), render-target map entries and undo. Mitigated by history reset plus a manual test that re-imports with an extra tile.
3. **Memory** at per-tile res (768 MB per layer at 4×4k). Needs the UI estimate, a 16384 clamp and a default per-tile res of 2k for new UDIM imports.
4. **Positional armpack**: encoder order must match the struct exactly. A mistake corrupts saves. Test round-trips after A2 and B1.
5. **Subset-merge cost** when switching layers with different object sets (O(vertices) rebuild). Mitigated by the merge key cache.
6. **Shader code size** for very fragmented id sets. Ranges keep it small. Worst case (alternating ids) is ~N/2 comparisons. Acceptable up to hundreds of objects. A coverage lookup texture is the fallback if this ever shows up in profiles.
7. **Slot-boundary fragments**: `floor(tex*stride)` exactly on a slot edge can pick the neighbour slot for that one fragment column, and bilinear sampling bleeds across slot borders (already true on main). Visually negligible. Dilation fills gutters.
8. **Footprint vs per-fragment mismatch** where objects overlap in UV within a tile (inherent).
9. **Path tracer fallback** when the merged mesh is a subset (verify in B3).

## 10. Open questions

1. Should a layer inside a layer group intersect its coverage with the group's sets? Main: no. The design keeps no.
2. Add mesh-delete undo (requirement 6 assumes it)? The design supports it: restore the object with its old `paint_id`.
3. Keep a small uid-based "Export groups" feature to replace named Atlases for non-UDIM multi-texture-set workflows?
4. Import of UDIM *texture sets* (`foo_1001.png…`) as one atlas asset for TEX_IMAGE nodes? This is the natural companion of the TEX_IMAGE "Atlas UV" flag.
5. Translucency plus path tracer: should uncovered or unpainted areas flatten with alpha 1 when translucency is on?
6. `mesh_nodes_objects` (types.h:705) is positional too. Convert it to ids in the same format bump?
7. The default per-tile resolution for new UDIM projects (2k suggested).

## 11. Manual test plan

The written test plans, test meshes and the test worksheet live in the sibling armorpaint-testing repo (`plans/`, `testdata/`, `worksheet/`), not in this repo.

Build after every step and load an old v23 project at A2, B1 and B7.

1. **Non-UDIM regression:** default cube, paint, fill, layers, masks, export Visible, save/reload, path trace. The result must be identical to main (same export pixels).
2. **Object sets:** 3 objects A, B, C. Layer L1 = {A, C}, L2 = {B}, L3 = All. Check the viewport, painting across all objects (only covered objects receive paint), mesh fill on B with L1 active (nothing), symmetry, sculpt on L2 (only B deforms).
3. **Stable ids:** reorder meshes (sort by name, reparent), duplicate A (the copy isn't in L1; with L3 All it is covered), join A+B (L1 now covers the joined mesh, L2 too), delete C (L1 still covers A). Delete A as well, so L1 shows "No objects", is skipped in the viewport and export, and ticking All reactivates it. Save/reload keeps everything.
4. **Undo:** coverage changes undo and redo. Undoing layer delete and merge restores sets.
5. **UDIM import:** obj with 4 tiles (1001, 1002, 1011, 1012) in **one** object, and a second object sharing 1002. Auto mode detects UDIM. A mesh with tiling UVs (0..4, straddling) stays non-UDIM. The straddling warning appears for a deliberately broken mesh. Same for fbx. Multi-object obj imports all objects.
6. **Tiles:** layer with tiles {1002}. Paint across the border between 1001 and 1002: only 1002 receives paint, on both objects in 1002. Check the 2D view grid.
7. **Relayout:** append a mesh using tile 1003 (stride 2 stays 2) and then 1021 (stride 3). Existing paint stays in place on every tile and undo is reset with a notice. Remove an unused tile.
8. **Resolution:** per-tile 2k, 4 tiles → 4k atlas. 8k per tile × 9 tiles clamps to 16384 with a warning. The memory readout is plausible.
9. **Export:** Per Tile gives `x_base_color_1001.png…` at native tile size. Per Object, and Per Object & Tile. A restricted layer is absent outside its footprint. Compare against viewport screenshots. Compare the path-trace render against the viewport for restricted layers.
10. **Bake Maps:** CPU AO/curvature and GPU AO on a UDIM mesh write the atlas plus `_1001…` files. The "Baked" material displays correctly (atlas UV flag). Inpaint updates the tile files.
11. **Legacy:** an upstream v23 name-based UDIM project (objects `Body.1001…`, shared layers, one tile-object layer, atlas groups) loads with the same look. Its resolution is per tile = old/stride, the tile-object layer sits in its slot, and atlas-group layers become object sets. Old object_mask projects without UDIM load correctly.
12. **Scripts:** dev/test.c layer object test with the compatibility setter.
