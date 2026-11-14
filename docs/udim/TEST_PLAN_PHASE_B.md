# UDIM phase B: manual test plan

Branch `feat/udim-object-tile-selection`, phase B (B1 UDIM tile import, B2 tile list and atlas relayout, B3 tile coverage in viewport/paint/sculpt, B4 footprint masks, B5 export modes, B6 bakes, B7 migration of name-based UDIM projects), commits `ba6723e84` to `722bba729`.
Every step below is to be checked by hand on the current tip. Fill in `Result:` and `Notes:` for each step, then tick the section in the summary.

On macOS, ctrl in shortcuts means cmd. "Layer row" means a row in the **Layers** tab. Every layer and group row has an **Objects button** (reads `Objects: All`, an object name, `Name1, Name2`, `N objects` or `No objects`). With more than one tile the row also has a **Tiles button** to its right (reads `Tiles: All`, `1012`, `1001, 1002`, `N tiles` or `No tiles`). Clicking either opens a popup with an `All` check, a separator and one check per object or tile. The popup stays open while you toggle.
Console messages show in the **Console** tab (and briefly in the status bar).
"Tile" means a UDIM tile id: `1001 + floor(u) + 10 * floor(v)`. The project atlas has `stride x stride` slots, `stride` = smallest s with s*s >= tile count, slots are filled in sorted tile order row by row, so in the 2D view tile 1001 is at the top left, the next tile to its right.

## Summary

| # | Section | Result |
|---|---------|--------|
| 0 | Prerequisites (build, test data, old project) | n/a |
| 1 | Non-UDIM regression | PASS / FAIL |
| 2 | Import: UDIM tiles Auto / On / Off, OBJ and FBX, Split By | PASS / FAIL |
| 3 | Tile list, Meshes tab, Tiles panel, Remove | PASS / FAIL |
| 4 | Grid growth, memory budget, clamp, append | PASS / FAIL |
| 5 | Layer Tiles popup | PASS / FAIL |
| 6 | Painting, fills and sculpt restricted by tile / object / both | PASS / FAIL |
| 7 | Layer filter by tile | PASS / FAIL |
| 8 | Viewport vs path trace vs export agreement | PASS / FAIL |
| 9 | Overlap tooltip | PASS / FAIL |
| 10 | Export modes | PASS / FAIL |
| 11 | Bakes (Bake Maps plugin) | PASS / FAIL |
| 12 | Save and reopen | PASS / FAIL |
| 13 | B7 migration of an old name-based UDIM project | PASS / FAIL |
| 14 | Undo / redo across tile edits | PASS / FAIL |
| 15 | Mesh export | PASS / FAIL |
| 16 | Known limits (expected, not failures) | n/a |

Phase B passes when sections 1 to 15 all pass. Any crash, lost paint after relayout/save/reload, paint on an uncovered tile or object, or an export tile that does not match the viewport is a FAIL.

## 0. Prerequisites

0.1 Build:
```
cd paint && ../base/make
cd build && xcodebuild -project ArmorPaint.xcodeproj -scheme ArmorPaint -configuration Release -derivedDataPath <dd> build
```
The app already built for this plan is at
`/private/tmp/claude-501/-Users-martin-crawford-Development-Workspaces-Game-armorpaint/b8bfd9f3-2c7e-4d01-9c91-d7daa0a9e8e7/scratchpad/dd/Build/Products/Release/ArmorPaint.app`
(it lives in a temp folder and can vanish; rebuilding anywhere else with another `<dd>` is fine).

0.2 Test data, all in `/Users/martin.crawford/Development/Workspaces/Game/armorpaint/docs/udim/testdata/` (tiles per object checked with `python3 check_tiles.py <obj>`):
- `udim_2x2_multi.obj` and `.fbx`: 5 objects, tiles 1001, 1002, 1011, 1012. `Body` (x=-4.5) spans 1001 (bottom half, 3 faces) and 1002 (3 faces). `Head` (x=-1.5) is on 1011. `Arm` (x=1.5) is on 1012 left half. `Leg` (x=4.5) is on 1012 right half. `Prop` (x=7.5) is on 1001 top half. Project tile count 4, stride 2.
- `udim_straddle.obj` and `.fbx`: object `Strip`, tiles 1001 and 1002, exactly 1 triangle crosses the u=1 border (centroid in 1001).
- `udim_tiling_uv.obj`: object `Floor`, a 2x2 quad plane, UVs 0..4 meant to repeat a texture, 8 straddling triangles. Auto must decide Off.
- `udim_single_1002.obj`: object `Crate`, entirely in tile 1002.
- `udim_12_tiles.obj`: 12 cubes `T1001`..`T1006` and `T1011`..`T1016`, each on its own tile (6x2 layout in 3D, row 0 = 1001-1006, row 1 = 1011-1016). Stride 4.
- `udim_seam_shared_vertex.obj`: object `Seam`, two quads sharing an edge in 3D, left quad in 1001, right quad in 1002, with separate vt per side.
- `three_cubes_separate_uv.obj`: cubes `A`, `B`, `C` in disjoint quadrants of 1001. `three_cubes_overlapping_uv.obj`: same cubes, all three use the whole 0-1 tile.
- `udim_body_two_tiles.obj`: object `Body`, left cube in 1001, right cube in 1002 (used for migration).
- Regenerate: `cd docs/udim/testdata && python3 gen_phase_b.py .` (the phase B meshes) and `python3 gen_obj.py .` (the phase A cubes and `udim_body_two_tiles.obj`). FBX copies: `blender -b --factory-startup --python obj_to_fbx.py -- in.obj out.fbx`.
- Check expected tiles: `python3 check_tiles.py udim_2x2_multi.obj` prints `Body: tiles=[1001, 1002] ...`, `Head: tiles=[1011]`, `Arm: [1012]`, `Leg: [1012]`, `Prop: [1001]`, `straddling=0` for every object. `python3 check_tiles.py udim_straddle.obj` prints `Strip: straddling=1`; `udim_tiling_uv.obj` prints `Floor: ... straddling=8`.

0.3 How to open the dialogs used below:
- **Import Mesh**: File > Import > Mesh..., pick the file. The **Import Mesh** box shows **Split By** (radio `Object` / `Material`), **UDIM tiles** (radio `Auto` / `On` / `Off`, tooltip `Auto uses tiles when the UVs span more than one tile and no triangle crosses a tile border`) for OBJ and FBX only, then `Cancel`, `Import`, and a chevron button whose menu has `Append`. File > Import > Mesh... replaces the project's meshes and clears its layers (a fresh import). `Append` keeps meshes and layers and adds the new objects.
- **Export Textures**: File > Export > Textures... (box with Resolution, Color bits, **Layers** combo, Preset, Destination, Padding, Export).
- **Export Mesh**: File > Export > Mesh... (tab `Export Mesh`: Format, Meshes, Export).
- **Layers tab Edit menu**: the `Edit` button in the Layers tab header, first line is the resolution combo (`Resolution` or `Resolution (per tile)`).
- **Plugins**: sidebar tab **Plugins**; the panel `Bake Maps` has a button `Bake Mesh Maps...`. If the panel is missing, enable `bake_maps` in the Plugins tab > `Preferences` button.
- **2D view**: shift+tab, or the `2D View` button at the top of the Layers tab. The slot grid with tile ids is drawn when the project has more than one tile.
- Path tracing: the `Viewport Mode` combo of the viewport, entry `Path Traced`.

0.4 Old v23 project with name-based UDIM tiles (for section 13). Make it with an upstream build:
- `git worktree add /tmp/ap-upstream 22097dfdd`.
- Build it the same way in the worktree: `cd /tmp/ap-upstream/paint && ../base/make`, then `cd build && xcodebuild -project ArmorPaint.xcodeproj -scheme ArmorPaint -configuration Release -derivedDataPath /tmp/ap-upstream-dd build`. Run `/tmp/ap-upstream-dd/Build/Products/Release/ArmorPaint.app`.
- In that upstream app: File > Import > Mesh... > `/Users/martin.crawford/Development/Workspaces/Game/armorpaint/docs/udim/testdata/udim_body_two_tiles.obj`, set **Split By** = `UDIM Tile` (the third radio, upstream only), click **Import**.
- Meshes tab: objects `Body.1001` and `Body.1002`.
- Note the project resolution (Layers tab Edit menu > Resolution, default `2048`; write it here: ______). Name it R.
- Layer `Layer 1` (the default, rename it `Shared`): its object combo (the combo in the layer row that reads `Shared`) stays on `Shared`. Paint with the Brush a bold stroke of a distinct colour (for example red) across both cubes, so it crosses both tiles. Also paint one stroke near the top of each cube.
- **New > Paint Layer**, name it `OnlyB2`. In its object combo (the combo that reads `Shared`) pick `Body.1002`. Paint a bold stroke of another colour (for example blue) on the right cube. It shows only on the right cube.
- Take screenshots of the viewport (front view, no camera change after) and of the 2D view (layer type) for `Shared` and for `OnlyB2`. Note which cube the 2D view shows each stroke in.
- File > Save As... `old_udim.arm` (anywhere, for example next to the worktree). Quit the app.
- `git worktree remove --force /tmp/ap-upstream` and delete `/tmp/ap-upstream-dd`.

## 1. Non-UDIM regression (upstream behaviour unchanged)

- [ ] 1.1 Start the new build (default cube), Layers tab Edit menu.
  Expected: the combo label reads `Resolution` (not `(per tile)`), no `tiles, atlas` line, no Tiles button on the layer row, no `Tiles` panel in the Meshes tab, no `Tile ...` entries in the Layers tab `Filter` combo.
  Result: PASS / FAIL
  Notes:
- [ ] 1.2 Paint with the Brush across the cube. **New > Fill Layer**. Add a mask to a layer. Add a second paint layer and hide it with its eye.
  Expected: everything works as on upstream, no console errors.
  Result: PASS / FAIL
  Notes:
- [ ] 1.3 File > Save As... `plain.arm` into a scratch folder, File > Export > Textures..., check the **Layers** combo, set it to `Visible`, export.
  Expected: the combo lists `Visible`, `Selected`, `Per Object` only (no tile modes). The Resolution label reads `Resolution`. Files written exactly as upstream: `plain_base.png`, `plain_nor.png`, `plain_occ.png`, `plain_rough.png`, `plain_metal.png` (preset `generic`), at the project resolution (2048 x 2048), no `_1001` suffix and no object name.
  Result: PASS / FAIL
  Notes:
- [ ] 1.4 File > Open... `plain.arm`.
  Expected: the same layers and paint, no console errors.
  Result: PASS / FAIL
  Notes:
- [ ] 1.5 Viewport Mode > `Path Traced`, wait for a few samples, then back to the normal viewport mode.
  Expected: the paint shows in the path traced view as in the raster view, no errors, the raster view returns.
  Result: PASS / FAIL
  Notes:
- [ ] 1.6 File > Import > Mesh... > `three_cubes_separate_uv.obj`, **Split By** `Object`, **UDIM tiles** `Auto`, Import. Layer 1 Objects popup: untick `All`, tick `B`. Paint a stroke across all three cubes.
  Expected: Meshes tab lists A, B, C. Only cube B shows the stroke. Edit menu still reads `Resolution`. No Tiles button, no `Tiles` panel (the project has the single tile 1001). The Auto import is silent (no console message about tiles).
  Result: PASS / FAIL
  Notes:
- [ ] 1.7 File > Export > Textures..., **Layers** `Per Object`, export (project saved as `three.arm`).
  Expected: one set per object some object-restricted layer covers: files with `three_B_base.png` etc. only for `B`. With layer 1 unrestricted and a second layer `{B}`, only B is exported. No tile suffix.
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 2. Import: UDIM tiles Auto / On / Off

For every import in this section use File > Import > Mesh... (fresh import), **Split By** `Object` unless said otherwise, then check the Meshes tab (object list, row tooltip), the Tiles panel at the bottom of the Meshes tab, the Layers tab Edit menu and the Console tab.

- [ ] 2.1 `udim_2x2_multi.obj`, **UDIM tiles** `Auto`, Import.
  Expected: tiles are on without any prompt. Meshes tab shows `Body`, `Head`, `Arm`, `Leg`, `Prop` (5 objects). `Tiles` panel lists `1001`, `1002`, `1011`, `1012`. Edit menu combo reads `Resolution (per tile)` with `2048`, and the line `4 tiles, atlas 4096 px, about 192 MB`. 2D view (layer type) shows a 2x2 grid with `1001` top left, `1002` top right, `1011` bottom left, `1012` bottom right. No console warning.
  Result: PASS / FAIL
  Notes:
- [ ] 2.2 Same file, **UDIM tiles** `Off`, Import.
  Expected: the old wrap behaviour: no `Tiles` panel, Edit menu reads `Resolution`, no Tiles button. All 5 objects are listed. Painting a stroke on `Body` also appears on `Head`, `Arm`, `Leg`, `Prop` at the same UV spots (everything overlaps in 0..1), as it does in upstream.
  Result: PASS / FAIL
  Notes:
- [ ] 2.3 `udim_tiling_uv.obj`, **UDIM tiles** `Auto`, Import.
  Expected: tiles stay off (wrap): no `Tiles` panel, Edit menu reads `Resolution`, no console warning about tiles or clamped triangles. A fill or stroke on `Floor` repeats 4 times along each direction of the plane (the 0..4 UVs wrap).
  Result: PASS / FAIL
  Notes:
- [ ] 2.4 `udim_straddle.obj`, **UDIM tiles** `Auto`, Import.
  Expected: Auto decides Off because a triangle crosses the border: no `Tiles` panel, no console warning, wrap behaviour.
  Result: PASS / FAIL
  Notes:
- [ ] 2.5 `udim_straddle.obj`, **UDIM tiles** `On`, Import. Open the Console tab.
  Expected: the message `1 triangles cross UDIM tile borders and were clamped` appears once. `Tiles` panel lists `1001` and `1002`, Meshes row tooltip of `Strip` reads `Strip` then `Tiles: 1001-1002`. The 2D view shows a 2x2 grid with `1001` and `1002` in the top row. Paint a stroke across the middle of `Strip`: only the one clamped triangle is distorted, the other quads look right.
  Result: PASS / FAIL
  Notes:
- [ ] 2.6 `udim_single_1002.obj`, **UDIM tiles** `On`, Import.
  Expected: the Tiles panel lists `1002` only (the row reads `1002`, `1 objects`, `2048 x 2048`, no Remove button). Edit menu reads `Resolution` (only one tile) and there is no `tiles, atlas` line. Paint a stroke on `Crate`, File > Export > Textures...: the Layers combo lists 5 entries and starts on a tile mode; choose `Per Tile`, export: files `<f>_base_1002.png` etc. (no `_1001`), 2048 x 2048. The mesh appears painted on all faces as drawn.
  Result: PASS / FAIL
  Notes:
- [ ] 2.7 Same file with **UDIM tiles** `Auto`, Import.
  Expected: Auto also turns tiles on for a lone 1002 tile (the UVs use a tile other than 1001, no straddling): Tiles panel `1002`.
  Result: PASS / FAIL
  Notes:
- [ ] 2.8 `udim_2x2_multi.obj`, **UDIM tiles** `On`, **Split By** `Object`, then again with **Split By** `Material`. (The file has no `usemtl`, so Material gives the same objects as without materials.)
  Expected: both imports give the tile list `1001, 1002, 1011, 1012`. Record the object count for Material. A multi-object OBJ imports every `o` object in every mode, so the count is 5 for both. Tiles On works with either Split By.
  Result: PASS / FAIL
  Notes:
- [ ] 2.9 `udim_seam_shared_vertex.obj`, **UDIM tiles** `On`. Paint a stroke across the shared edge of the two quads on both sides. Open the 2D view and look at the slots `1001` / `1002`.
  Expected: the two quads show the stroke continuously across the 3D seam, no triangle is stretched across the atlas from slot `1001` to slot `1002`. In the 2D view the stroke appears in both slots near their facing borders (the left quad in `1001`, the right in `1002`). No console clamp message (no straddling).
  Result: PASS / FAIL
  Notes:
- [ ] 2.10 FBX: `udim_2x2_multi.fbx`, **UDIM tiles** `Auto`, then `udim_straddle.fbx` with `Auto` and with `On`.
  Expected: `udim_2x2_multi.fbx`: tiles `1001, 1002, 1011, 1012` with objects `Body`, `Head`, `Arm`, `Leg`, `Prop` (names may carry Blender suffixes, record them), same Meshes row tooltips as 2.1. `udim_straddle.fbx` Auto: tiles off, no console message. `udim_straddle.fbx` On: tiles `1001, 1002` and the console message `1 triangles cross UDIM tile borders and were clamped`.
  Result: PASS / FAIL
  Notes:
- [ ] 2.11 Fresh multi-tile import resets the resolution. Start from a default project, set Edit menu > Resolution `4096`, then import `udim_2x2_multi.obj` (fresh, Auto).
  Expected: after the import the Edit menu combo reads `2048` (per tile) and the line reads `4 tiles, atlas 4096 px, about 192 MB`. Importing a plain single-tile mesh afterwards does not change the resolution again.
  Result: PASS / FAIL
  Notes:
- [ ] 2.12 glTF and Blend imports stay Off. Import any `.glb` or `.blend` with UVs above 1, if available (optional).
  Expected: no **UDIM tiles** radio in the box, wrap behaviour, no `Tiles` panel.
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 3. Tile list, Meshes tab and Remove

Setup: import `udim_2x2_multi.obj` (fresh, Auto).

- [ ] 3.1 Hover each object row in the Meshes tab.
  Expected: tooltip second line: `Body` -> `Tiles: 1001-1002`, `Head` -> `Tiles: 1011`, `Arm` -> `Tiles: 1012`, `Leg` -> `Tiles: 1012`, `Prop` -> `Tiles: 1001`.
  Result: PASS / FAIL
  Notes:
- [ ] 3.2 Scroll to the `Tiles` panel at the bottom of the Meshes tab.
  Expected: four rows, each with the id, `N objects`, `2048 x 2048`: `1001` `2 objects` (Body, Prop), `1002` `1 objects`, `1011` `1 objects`, `1012` `2 objects` (Arm, Leg). No `Remove` button on any row (all tiles are used).
  Result: PASS / FAIL
  Notes:
- [ ] 3.3 Paint a stroke on `Head` and one on `Arm`. Note the colours. In the Meshes tab, right-click `Head` > Delete.
  Expected: the Head disappears. In the `Tiles` panel the row `1011` now reads `0 objects` and shows a `Remove` button; the other rows have none. Layer `Tiles` popup (if you open it) lists `1011 (unused)`.
  Result: PASS / FAIL
  Notes:
- [ ] 3.4 Click `Remove` on row `1011`.
  Expected: a box titled `Remove tile` with `Remove tile 1011? Its pixels are deleted and undo history is cleared.` and the buttons `Cancel` and `Remove`. `Cancel` closes it and nothing changes.
  Result: PASS / FAIL
  Notes:
- [ ] 3.5 Click `Remove` on row `1011` again, then `Remove` in the box.
  Expected: the Console shows `Tile layout changed, undo history was cleared`. The Tiles panel lists `1001`, `1002`, `1012`. The Edit menu line reads `3 tiles, atlas 4096 px, about 192 MB` (stride stays 2). The 2D view shows `1001`, `1002`, `1012` (the 1012 slot moved to bottom left). The Edit menu's Undo entry is empty or greyed (history is empty): Edit > Undo has no step. The stroke on `Arm` is still in place on the viewport and in the 2D view slot `1012`. Painting still works.
  Result: PASS / FAIL
  Notes:
- [ ] 3.6 Try to remove a used tile: the rows `1001`, `1002`, `1012` show no `Remove` button.
  Expected: confirmed, no button. With only one tile left in a project the button never shows.
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 4. Grid growth, memory budget, clamp, append

- [ ] 4.1 Import `udim_12_tiles.obj` (fresh, Auto). Open the Layers tab Edit menu.
  Expected: combo `Resolution (per tile)` reads `2048`, line `12 tiles, atlas 8192 px, about 768 MB` (stride 4, one layer). No clamp line. 2D view shows a 4x4 grid with tile ids in slot order (row 1: 1001..1004, row 2: 1005, 1006, 1011, 1012, row 3: 1013..1016). Tiles panel lists all 12, each `1 objects`.
  Result: PASS / FAIL
  Notes:
- [ ] 4.2 Edit menu > Resolution: choose `4096`.
  Expected: line reads `12 tiles, atlas 16384 px, about 3072 MB`, no clamp line (4 x 4096 = 16384).
  Result: PASS / FAIL
  Notes:
- [ ] 4.3 Choose `8192` (needs several GB of GPU memory, skip if the machine cannot take it and mark Notes).
  Expected: the clamp line `Clamped to 4096 px per tile (atlas limit 16384)` appears, atlas stays `16384 px`, `Tiles` panel shows `4096 x 4096` per tile. The paint is not corrupted.
  Result: PASS / FAIL
  Notes:
- [ ] 4.4 Set Resolution back to `2048`. Delete `T1004`, `T1005`, `T1006` (Meshes tab, right-click > Delete) and `Remove` their tiles 1004 to 1006 in the Tiles panel (three confirmations). Now 9 tiles (stride 3). Set Resolution `8192`.
  Expected: `9 tiles, atlas 16383 px, about 3072 MB` and `Clamped to 5461 px per tile (atlas limit 16384)` (16384 / 3 = 5461). Tiles panel shows `5461 x 5461`. Set it back to `2048` afterwards.
  Result: PASS / FAIL
  Notes:
- [ ] 4.5 The label is `Resolution (per tile)` only with more than one tile: check the Edit menu and the Export Textures box in a plain default cube project, in a `udim_single_1002.obj` project and in `udim_2x2_multi.obj`.
  Expected: `Resolution` for the first two, `Resolution (per tile)` for the third, in both the Edit menu and the Export Textures box.
  Result: PASS / FAIL
  Notes:
- [ ] 4.6 Appending a new tile keeps paint in place. Import `udim_2x2_multi.obj` (fresh). Paint a distinct stroke on `Body`, `Head`, `Arm`, `Prop`. Take a viewport screenshot (fixed camera). Open the Import Mesh box for `udim_12_tiles.obj`, click the chevron button next to `Import` and choose `Append`.
  Expected: the Console shows `Tile layout changed, undo history was cleared`. The tile list grows to `1001`-`1006`, `1011`-`1016` (12 tiles), Tiles panel lists them, Edit line `12 tiles, atlas 8192 px, about 768 MB`. The five original objects look identical to the screenshot (strokes in the same places), the new `T10xx` objects are empty (default colour). The 2D view shows the old tiles at their new slots with the strokes intact (`1001` and `1002` still top left, `1011` moved to row 2, column 3, `1012` to row 2, column 4). Edit > Undo is empty.
  Result: PASS / FAIL
  Notes:
- [ ] 4.7 Appending to a plain project. Default cube project, paint a stroke on the cube. Import `udim_single_1002.obj` with `Append`.
  Expected: tile list `1001`, `1002`. The cube keeps its stroke (it is in tile 1001, slot 1), `Crate` is empty and sits on `1002`. Edit menu reads `Resolution (per tile)`.
  Result: PASS / FAIL
  Notes:
- [ ] 4.8 Append a plain single-tile mesh (for example `three_cubes_separate_uv.obj`) to a project with tiles.
  Expected: the new objects go to tile 1001, list unchanged, no layout-changed message, existing paint unchanged.
  Result: PASS / FAIL
  Notes:
- [ ] 4.9 Optional (about 3 GB GPU memory): set `udim_2x2_multi.obj` (fresh) to Resolution `8192` (atlas 16384, no clamp), then `Append` `udim_12_tiles.obj`.
  Expected: Console shows `UDIM atlas too large: 12 tiles use 4096 px per tile (16384 px atlas)` and the Edit menu shows the clamp line `Clamped to 4096 px per tile (atlas limit 16384)`.
  Result: PASS / FAIL
  Notes:
- [ ] 4.10 Relayout keeps all layer data. In a 4-tile project make a mask on a layer (**Add Mask** on the layer, paint black on it), add a second layer, paint on both, then `Append` `udim_12_tiles.obj`. Repeat in a project with 16bit colour (Edit menu > `Bits` `16bit`).
  Expected: both layers and the mask keep their pixels on the original tiles (compare screenshots). New tiles: layer empty, mask white (not black). Material channels (roughness, metal) of the old paint unchanged.
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 5. Layer Tiles popup

Setup: `udim_2x2_multi.obj` fresh import.

- [ ] 5.1 Look at a layer row in a plain default cube project and in a `udim_single_1002.obj` project.
  Expected: no Tiles button (shown only with more than one tile).
  Result: PASS / FAIL
  Notes:
- [ ] 5.2 In the 4-tile project, the layer row of `Layer 1`.
  Expected: two buttons side by side, `Objects: All` and `Tiles: All`. Hovering the Tiles button shows `Limit this layer to the selected UDIM tiles.`
  Result: PASS / FAIL
  Notes:
- [ ] 5.3 Click `Tiles: All`.
  Expected: popup with `All` (ticked), a separator, then `1001`, `1002`, `1011`, `1012`, all ticked and greyed out (clicking does nothing, no undo step).
  Result: PASS / FAIL
  Notes:
- [ ] 5.4 Untick `All`, tick `1002` and `1012`.
  Expected: popup stays open; the button reads `1002, 1012`; tick `1011` also: `3 tiles`; untick `1011` and `1002`: `1012`. Untick everything: `No tiles` and the layer name is dimmed with the tooltip `No tiles are selected for this layer or its group, so it covers nothing. Select tiles or choose All to use it.`
  Result: PASS / FAIL
  Notes:
- [ ] 5.5 Tick `All` again and untick it again.
  Expected: the popup shows the stored list again (rows enabled, the earlier ticks kept).
  Result: PASS / FAIL
  Notes:
- [ ] 5.6 Delete `Head` so tile `1011` is unused (see 3.3), open the Tiles popup.
  Expected: the row reads `1011 (unused)`, other rows plain ids.
  Result: PASS / FAIL
  Notes:
- [ ] 5.7 **New > Group**, drag a layer into it. Group Tiles popup: untick `All`, tick `1012` only. Open the child layer's Tiles popup.
  Expected: the child's rows `1001`, `1002`, `1011` are greyed and unticked, hovering one shows `Excluded by the group`. `1012` is enabled. The group set back to `All`: nothing greyed, the child's own list unchanged. A group set to `1001` and a child set to `1012` only: the child's name is dimmed (nothing in common) with the tiles tooltip from 5.4.
  Result: PASS / FAIL
  Notes:
- [ ] 5.8 The Tiles and Objects sets are independent. Layer `Objects: Body` and `Tiles: 1002`.
  Expected: ticking in one popup never changes the other button's text.
  Result: PASS / FAIL
  Notes:
- [ ] 5.9 Mini layers panel (collapse the sidebar to mini mode if it is available): right-click a layer.
  Expected: the context menu has `Objects` and, with more than one tile, `Tiles` sub-menus with the same entries.
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 6. Painting, fills and sculpt restricted by tile / object / both

Setup: `udim_2x2_multi.obj` fresh import, front camera so `Body` (faces on 1001 and 1002) is clearly visible. `Body`'s 3 faces on 1001 and 3 faces on 1002 are on different cube faces: check with a preliminary stroke on a spare layer that you can tell them apart.

- [ ] 6.1 Layer 1 `Tiles: 1002` only. Brush stroke dragged across the faces of `Body`.
  Expected: only the faces of `Body` that sit on tile 1002 get paint, the faces on tile 1001 stay unpainted, also where the stroke crosses them. Other objects on 1001 (`Prop`) get nothing, objects on 1011 / 1012 (`Head`, `Arm`, `Leg`) get nothing.
  Result: PASS / FAIL
  Notes:
- [ ] 6.2 Layer with `Tiles: 1012` only: Brush stroke across `Head`, `Arm` and `Leg`.
  Expected: `Arm` and `Leg` are painted, `Head` is not. Record whether a stroke that starts on `Head` adds an undo step (tile restriction is a shader discard, no pick, so one may be added).
  Result: PASS / FAIL
  Notes:
- [ ] 6.3 Layer with `Tiles: All`, `Objects: Body` only. Brush stroke across `Body`, `Head`, `Prop`.
  Expected: only `Body` is painted, on both its tiles. (A stroke starting on `Head` is skipped without an undo step, as in phase A.)
  Result: PASS / FAIL
  Notes:
- [ ] 6.4 Layer with `Objects: Body` AND `Tiles: 1002`. Stroke across `Body`.
  Expected: only the 1002 faces of `Body` are painted (both restrictions apply as an intersection). Switch the layer to `Tiles: 1001`: the next stroke paints only the 1001 faces of `Body`, never `Prop` (not covered by Objects). Switch to `Objects: Prop`, `Tiles: 1002`: it covers nothing, name dimmed.
  Result: PASS / FAIL
  Notes:
- [ ] 6.5 Layer with `No tiles`: Brush stroke.
  Expected: nothing is painted and the Console tab title shows `Layer covers no tiles` for a few seconds (not `Layer covers no objects`).
  Result: PASS / FAIL
  Notes:
- [ ] 6.6 **Fill** tool with a fill mode, layer `Tiles: 1002`, click on `Body` and on `Prop`.
  Expected: fill mode `Mesh` / `Object` only fills the covered part of tile 1002. Texels on tiles 1001, 1011, 1012 stay untouched. **New > Fill Layer** with `Tiles: 1011` set: only `Head` shows the fill right away, the layer refills when the set changes.
  Result: PASS / FAIL
  Notes:
- [ ] 6.7 2D view painting. Layer `Tiles: 1002`. Open the 2D view (layer type) and paint a stroke on slot `1001` (top left), then one on slot `1002` (top right).
  Expected: the stroke on slot `1001` does nothing. The stroke on slot `1002` paints. The viewport shows only the 1002 stroke on `Body`.
  Result: PASS / FAIL
  Notes:
- [ ] 6.8 Other tools on a tile-restricted layer (Eraser, Clone, Blur, Decal, Text): click or stroke on tile 1001 with layer `Tiles: 1002`.
  Expected: nothing changes outside tile 1002 for each tool.
  Result: PASS / FAIL
  Notes:
- [ ] 6.9 Tile restriction covering every project tile. Layer `Tiles: 1001, 1002, 1011, 1012` (all ticked by hand, `All` unticked).
  Expected: behaves like `All` (paint reaches every object), no visible difference in speed or look.
  Result: PASS / FAIL
  Notes:
- [ ] 6.10 Sculpt. **New > Sculpt Layer** (sculpt tools mode). Tiles `1012` only. Sculpt (Brush or Grab) across `Arm`, `Leg` and `Head`.
  Expected: `Arm` and `Leg` deform, `Head` does not. Set tiles `1002`: only the vertices on `Body`'s 1002 faces deform, including the shared edge vertices at the 1001/1002 border (the surface does not tear, no cracks along the border).
  Result: PASS / FAIL
  Notes:
- [ ] 6.11 Seam vertices. Import `udim_seam_shared_vertex.obj` (On), **New > Sculpt Layer**, Tiles `1002` only. Grab or inflate on the shared edge.
  Expected: the shared edge vertices move together with the 1002 quad, the surface stays closed (the left quad is not left behind with a gap). With Tiles `1001` the same: no tearing.
  Result: PASS / FAIL
  Notes:
- [ ] 6.12 Sculpt layer with `No tiles`.
  Expected: nothing deforms, the Console tab title shows `Layer covers no tiles`, no undo step is added.
  Result: PASS / FAIL
  Notes:
- [ ] 6.13 Brush and material previews and decals. Open a material in the Materials tab, look at the preview, and apply a decal on a tile-restricted layer (covered tile).
  Expected: previews show the material normally (not cut by any tile restriction). The decal paints only on the covered tile.
  Result: PASS / FAIL
  Notes:
- [ ] 6.14 Tile borders. Paint a bold stroke along the border between two faces on the same tile slot, then look closely at the tile edge in the 2D view slot.
  Expected: no bleed of one slot into the neighbouring slot visible at the seams of the mesh, no dark/lighter line along tile borders of an unrestricted layer.
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 7. Layer filter by tile

Setup: `udim_2x2_multi.obj`. Layers: `L_all` (Tiles All), `L1012` (Tiles `1012`), `L1001` (Tiles `1001`).

- [ ] 7.1 Layers tab `Filter` combo.
  Expected: entries `All`, the 5 objects, then `Tile 1001`, `Tile 1002`, `Tile 1011`, `Tile 1012`. In a project with only tile 1001 there are no `Tile ...` entries.
  Result: PASS / FAIL
  Notes:
- [ ] 7.2 Choose `Tile 1012`.
  Expected: the layer list hides `L1001` (does not cover 1012) and shows `L_all` and `L1012`. All objects stay visible in the viewport (unlike the object filter, which hides other objects).
  Result: PASS / FAIL
  Notes:
- [ ] 7.3 With the filter on `Tile 1012`, **New > Paint Layer**.
  Expected: its Tiles button reads `1012` (and its Objects button reads `Objects: All`).
  Result: PASS / FAIL
  Notes:
- [ ] 7.4 Choose an object (for example `Head`) then `Tile 1011` then `All`.
  Expected: only one of object or tile filter is active at a time. Choosing an object clears the tile filter and the reverse. `All` shows every layer.
  Result: PASS / FAIL
  Notes:
- [ ] 7.5 Filter on `Tile 1011`, delete `Head`, remove tile 1011 (3.3 to 3.5).
  Expected: the filter falls back to `All` the next time the combo draws (the stale tile filter resets), no crash.
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 8. Viewport vs path trace vs export agreement

Setup: `udim_2x2_multi.obj` fresh, three layers: `Base` (Fill, grey, all), `T1002` (Fill red, Tiles `1002`), `ObjHead` (Fill blue, Objects `Head`), plus `Mix` (Fill yellow, Objects `Body`, Tiles `1001`). Save the project as `agree.arm`. Fix the camera so every object is visible and take a screenshot (S1).

- [ ] 8.1 Viewport: S1.
  Expected: `Body` shows red on its 1002 faces and yellow on its 1001 faces, `Head` blue, other objects grey (Base only). `Mix` not on `Prop` (even though `Prop` is on 1001).
  Result: PASS / FAIL
  Notes:
- [ ] 8.2 Viewport Mode > `Path Traced`, same camera, wait for convergence, screenshot (S2).
  Expected: S2 shows the same coverage as S1 (same objects red/yellow/blue/grey, lighting aside). Where the footprint includes dilate padding, paint reaches a few pixels beyond a UV island, never visibly onto a different object.
  Result: PASS / FAIL
  Notes:
- [ ] 8.3 File > Export > Textures..., **Layers** `Per Tile`, folder `tile_out`, export. Open `agree_base_1001.png`, `agree_base_1002.png`, `agree_base_1011.png`, `agree_base_1012.png`.
  Expected: `_1002` contains red in the 1002 island area only; `_1001` has yellow in the `Body` faces on 1001 and none over the `Prop` island; `_1011` contains blue in the `Head` island; `_1012` has only grey in its islands. Each file is 2048 x 2048.
  Result: PASS / FAIL
  Notes:
- [ ] 8.4 Export `Visible` and import it back as a texture on a fill layer (or open the image) and compare with the 2D view.
  Expected: the exported atlas `agree_base.png` (4096 x 4096) matches the 2D view of the flattened layers (2x2 slots in the same positions).
  Result: PASS / FAIL
  Notes:
- [ ] 8.5 Per Object export: set `ObjHead` to `Objects: Head`, `Mix` to `Objects: Body`. Export `Per Object`.
  Expected: files `agree_Head_*.png` and `agree_Body_*.png` (objects covered by an object-restricted layer). Each is a full atlas (4096 x 4096). `Body`'s file contains the yellow and red layers in the Body islands, plus `Base`; no blue. `Head`'s file contains `Base` and the blue layer in the Head island.
  Result: PASS / FAIL
  Notes:
- [ ] 8.6 Path tracing cost. In path traced mode, add and toggle a layer set `Objects: Head` several times.
  Expected: the view refreshes without a long stall or a crash (a footprint mask is built per object-restricted layer). Record rough stall times.
  Result: PASS / FAIL
  Notes:
- [ ] 8.7 Non-UDIM project with an object-restricted layer (`three_cubes_separate_uv.obj`, layer `{B}` painted over everything): Per Object export and path trace.
  Expected: B's file/texture holds the layer only inside B's UV quadrant (footprint plus padding); A and C quadrants hold only unrestricted layers. Path trace shows the layer on B only.
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 9. Overlap tooltip

- [ ] 9.1 Import `three_cubes_overlapping_uv.obj` (fresh, Auto). Layer 1: Objects popup untick `All`, tick `A`. Hover the Objects button.
  Expected: tooltip first line `Limit this layer to the selected objects.` then a second line naming B or C, for example `A shares UV space with B on tile 1001. In exports and path tracing this layer also applies to B.` (the named object is the first uncovered object that overlaps A on tile 1001).
  Result: PASS / FAIL
  Notes:
- [ ] 9.2 Layer `Objects: All` in the same project.
  Expected: only the first line of the tooltip, no overlap sentence.
  Result: PASS / FAIL
  Notes:
- [ ] 9.3 Import `three_cubes_separate_uv.obj`, layer `{A}`. Hover the Objects button.
  Expected: only the first line, no overlap sentence (disjoint quadrants).
  Result: PASS / FAIL
  Notes:
- [ ] 9.4 `udim_2x2_multi.obj` layer `Objects: Arm`. Hover the Objects button.
  Expected: no sentence (`Arm` and `Leg` are on different halves of tile 1012). Layer `Objects: Body`: `Body` and `Prop` share tile 1001 but different halves (v bottom/top), so no sentence either. Edge contact does not count.
  Result: PASS / FAIL
  Notes:
- [ ] 9.5 With the overlapping cubes, restrict a layer to `A`, paint a stroke on A. Export `Per Object`.
  Expected (documented limit, not a failure): the exported A set holds the stroke, and the same texels are used by B and C because they share UV space (paint also appears on B and C in exports and path tracing, but only on A in the raster viewport).
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 10. Export modes

Setup: `udim_2x2_multi.obj` fresh import, project saved as `ex.arm`, preset `generic` (channels `base`, `nor`, `occ`, `rough`, `metal`). Layers: `L1` (fill green, All), `L2` (stroke, red, Tiles `1002`), `L3` (stroke, blue, Objects `Head`). `<f>` below is the project name `ex`.

- [ ] 10.1 File > Export > Textures...
  Expected: the **Layers** combo lists `Visible`, `Selected`, `Per Object`, `Per Tile`, `Per Object & Tile` and starts on `Per Tile` (more than one tile). The resolution combo reads `Resolution (per tile)`.
  Result: PASS / FAIL
  Notes:
- [ ] 10.2 **Layers** `Per Tile`, folder `out_tile`, Export.
  Expected: 20 files (4 tiles x 5 channels): `ex_base_1001.png`, `ex_base_1002.png`, `ex_base_1011.png`, `ex_base_1012.png`, and the same for `nor`, `occ`, `rough`, `metal`. Every file is 2048 x 2048 (native per-tile size for a 2k project). `ex_base_1002.png` contains the red stroke from `L2`; `ex_base_1001.png`, `_1011.png`, `_1012.png` contain no red. The blue `L3` stroke appears only in the `Head` island of `_1011`. No `ex_base.png` atlas, no object names. Every used tile is written even if empty.
  Result: PASS / FAIL
  Notes:
- [ ] 10.3 **Layers** `Per Object`, folder `out_obj`, Export.
  Expected: files for the objects covered by an object-restricted layer only: `ex_Head_base.png`, `ex_Head_nor.png`, ... (5 files), 4096 x 4096 (atlas). `L1` appears inside `Head`'s footprint only; `L2` (tile-restricted, covers 1002) is absent from Head's set since Head is on 1011. No tile suffix.
  Result: PASS / FAIL
  Notes:
- [ ] 10.4 **Layers** `Per Object & Tile`, folder `out_both`, Export.
  Expected: names `ex_<object>_<ch>_<tile>.png` for every object on each tile it uses, skipping pairs that no visible layer applies to. With `L1` unrestricted all pairs apply: `ex_Body_base_1001.png`, `ex_Body_base_1002.png`, `ex_Head_base_1011.png`, `ex_Arm_base_1012.png`, `ex_Leg_base_1012.png`, `ex_Prop_base_1001.png` (+ the 4 other channels each = 30 files), 2048 x 2048. The red stroke is in `ex_Body_base_1002.png` only; nothing of `Prop` appears in `ex_Body_base_1001.png` (footprint mask of Body).
  Result: PASS / FAIL
  Notes:
- [ ] 10.5 Hide `L1` (eye off) and export `Per Object & Tile` again.
  Expected: pairs without any applying visible layer are skipped: only `Head` / `1011` (from `L3`) and `Body` / `1002` (from `L2`) are written.
  Result: PASS / FAIL
  Notes:
- [ ] 10.6 **Layers** `Visible`.
  Expected: one set, `ex_base.png` etc, 4096 x 4096, the whole atlas (2x2 slots), no suffix. **Layers** `Selected` (select one layer): same, no object name appended.
  Result: PASS / FAIL
  Notes:
- [ ] 10.7 Native size crop check (off-by-one). Open `ex_base_1002.png` and compare with the 2D view slot `1002`.
  Expected: exactly 2048 x 2048, the same content as the slot (no one-pixel shift, no 1 px line of the neighbouring tile at the edges).
  Result: PASS / FAIL
  Notes:
- [ ] 10.8 Single-1001 project (default cube or `three_cubes_separate_uv.obj`): open the Export Textures box.
  Expected: the **Layers** combo lists `Visible`, `Selected`, `Per Object` only. If a tile mode was selected before opening, it falls back to `Visible`.
  Result: PASS / FAIL
  Notes:
- [ ] 10.9 `udim_single_1002.obj` project: the box lists all five modes, `Per Tile` exports `<f>_base_1002.png` etc. only.
  Expected: confirmed; no `_1001` files.
  Result: PASS / FAIL
  Notes:
- [ ] 10.10 Other formats and depth: set Color `16bit` (exr) and export `Per Tile`; then format `jpg` with `8bit`.
  Expected: file names `ex_base_1001.exr` and `ex_base_1001.jpg` (tile suffix before the extension), same sizes.
  Result: PASS / FAIL
  Notes:
- [ ] 10.11 CLI and Selected shortcut paths start in `Per Tile` too when the project has several tiles (use the export shortcut, File > Export > Textures As... if it exists, or ctrl+shift+e / the keymap's file_export_textures_as).
  Expected: output names carry `_1001` etc. for a project with more than one tile.
  Result: PASS / FAIL
  Notes:
- [ ] 10.12 Destination `Pack into Project`, `Per Tile`.
  Expected: textures with tile suffixes are added to the Textures tab (names such as `ex_base_1001`), no crash.
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 11. Bakes (Bake Maps plugin)

Setup: `udim_2x2_multi.obj` fresh import, project saved as `bk.arm`, Resolution `2048` per tile (atlas 4096). Plugins tab, panel `Bake Maps`, button `Bake Mesh Maps...`. Box tabs: `Maps`, `Settings`, `About`. In `Maps` click `Select None`, then tick `Ambient Occlusion`. In `Settings` > `Common` set `Resolution` `2048` (per tile; the combo does not say so). In `Settings` > `Output` set `Name` `bkao` and click the `...` folder button and pick an empty folder `bake_out`. Click `Bake`. The tiles in the project are 1001, 1002, 1011, 1012 (not 1001..1012).

- [ ] 11.1 Bake the AO map (GPU default).
  Expected: Console `Baked 1 maps` then `Baked maps linked in material bkao Baked`. Files in `bake_out`: `bkao_ao.png` (the atlas, 4096 x 4096) and the tile crops `bkao_ao_1001.png`, `bkao_ao_1002.png`, `bkao_ao_1011.png`, `bkao_ao_1012.png`, each 2048 x 2048. Only the atlas file is imported into the project Textures tab (`bkao_ao`), the tile crops are not.
  Result: PASS / FAIL
  Notes:
- [ ] 11.2 Open the 4 tile crops and the atlas.
  Expected: each crop equals the matching slot of the atlas (`1001` top left, `1002` top right, `1011` bottom left, `1012` bottom right). AO shading shows on the islands of the objects of that tile.
  Result: PASS / FAIL
  Notes:
- [ ] 11.3 Materials tab: material `bkao Baked`. Open the node editor (the `Nodes` view) and select the image texture node.
  Expected: an image node linked to Occlusion with the button `UV` = `Atlas` (the two options of the button read `Default` and `Atlas`).
  Result: PASS / FAIL
  Notes:
- [ ] 11.4 Plugins panel `Add Baked Layer`.
  Expected: a fill layer with the `bkao Baked` material; the baked AO displays correctly on all objects of all tiles (no repeats, no tile showing the whole atlas squeezed in).
  Result: PASS / FAIL
  Notes:
- [ ] 11.5 In the image node switch `UV` from `Atlas` to `Default`.
  Expected: the map no longer fits: the texture repeats per tile (each tile shows the whole atlas shrunk or a seam of 2x2 repeats). Switch back to `Atlas`, the correct map returns. (Old canvases with only two buttons behave as Default.)
  Result: PASS / FAIL
  Notes:
- [ ] 11.6 The `View` buttons `AO` / `Normal` / `Cavity` of the plugin panel.
  Expected: the viewport shows the baked AO on all tiles correctly (no seams or empty tiles).
  Result: PASS / FAIL
  Notes:
- [ ] 11.7 Inpaint re-crops tiles. Note the modification times of `bkao_ao_1001.png` etc (`ls -l@ bake_out`). Pick the plugin tool `Inpaint` (Tools list), mode Copy, paint a short stroke on the island of `Head` (tile 1011) and release the mouse.
  Expected: Console `Inpaint: 1 maps repaired`. `bkao_ao.png` and the tile crops are rewritten (newer timestamps for the crops of the tiles that exist, in particular `bkao_ao_1011.png`), and the content of `bkao_ao_1011.png` equals slot 1011 of the new atlas (the repaired area visible). Tiles that were not touched keep the same pixels.
  Result: PASS / FAIL
  Notes:
- [ ] 11.8 Bake at 4096 per tile on 4 tiles (atlas 8192) and at 1024 (atlas 2048).
  Expected: atlas 8192 x 8192 with tile crops of 4096 x 4096; atlas 2048 x 2048 with crops of 1024 x 1024. Names `bkao_ao_...` unchanged.
  Result: PASS / FAIL
  Notes:
- [ ] 11.9 Per-tile bake resolution with a changed project resolution. Set the project Edit menu resolution to `4096` per tile, run the bake again at script Resolution `2048`.
  Expected: the bake size follows the script Resolution (per tile 2048), not the project resolution. Record the sizes.
  Result: PASS / FAIL
  Notes:
- [ ] 11.10 CPU bake warning. Import `udim_12_tiles.obj`, Edit menu Resolution `2048`. In the plugin, Settings > `Raytracing` `Bake on` `CPU`, `Resolution` `4096` (12 tiles, atlas 4096 x 4 = 16384), click `Bake`. Press `Stop` after the message appears (the CPU bake needs about 2 GB).
  Expected: the Console shows `CPU bake of a 16384 px atlas needs a lot of memory, consider the GPU bake or a lower resolution` before the bake starts. At atlas 8192 or below (for example `Resolution` `2048` with 12 tiles = 8192) no such warning is shown.
  Result: PASS / FAIL
  Notes:
- [ ] 11.11 1002-only project (`udim_single_1002.obj`): bake AO.
  Expected: `bkao_ao.png` (the atlas) plus a `bkao_ao_1002.png` copy of the same size (single tile), no `_1001` file.
  Result: PASS / FAIL
  Notes:
- [ ] 11.12 Plain single-tile project (default cube), bake AO.
  Expected: only `bkao_ao.png`, no tile files, and the Baked material's image node still works.
  Result: PASS / FAIL
  Notes:
- [ ] 11.13 Relayout after a bake. In the 4-tile project append `udim_12_tiles.obj` while the bake window is closed, then bake again.
  Expected: the bake uses the new layout (12 tiles, stride 4, per tile files for every used tile `_1001`..`_1006`, `_1011`..`_1016`, tile files of removed or no longer used tiles are not deleted).
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 12. Save and reopen

- [ ] 12.1 `udim_2x2_multi.obj` fresh import. Paint strokes on several tiles. Layers: `L2` Tiles `1002` + Objects `Body`, `L3` Tiles `1011, 1012`, group with `Objects: Head`. Set resolution `4096` per tile (atlas 8192). Note a screenshot and the 2D view. File > Save As... `rt.arm`, close the app, reopen it, File > Open... `rt.arm`.
  Expected: same tile list (`Tiles` panel `1001`, `1002`, `1011`, `1012`), Edit menu `4096` per tile with `4 tiles, atlas 8192 px, about ...`, same Tiles/Objects buttons on every layer and the group, the viewport and the 2D view are pixel-identical to before saving. Meshes row tooltips still read the same `Tiles: ...`.
  Result: PASS / FAIL
  Notes:
- [ ] 12.2 Save again as `rt2.arm`, close and reopen it.
  Expected: loads without errors, everything still the same (save -> reopen -> save works repeatedly).
  Result: PASS / FAIL
  Notes:
- [ ] 12.3 Remove a tile first (3.3 to 3.5), save, reopen.
  Expected: the removed tile is gone, the other tiles' paint intact, the 2D view slots match.
  Result: PASS / FAIL
  Notes:
- [ ] 12.4 Linked duplicates. Meshes tab: right-click `Arm` > Duplicate. Paint on the copy, Save, reopen.
  Expected: the duplicate still shows its tiles (`Tiles: 1012` in its tooltip), paints on the right tile and has the same layout as before saving. Also Duplicate `Body` (two tiles) and check after reopening.
  Result: PASS / FAIL
  Notes:
- [ ] 12.5 Mesh-only .arm import. File > Import > Mesh... on a `.arm` saved from `rt.arm` (mesh-only) into a project with tiles (and into a plain project).
  Expected: imported meshes go to tile 1001 (no tile data), no crash, existing paint untouched.
  Result: PASS / FAIL
  Notes:
- [ ] 12.6 Project with tiles saved with 16bit colour, reopen.
  Expected: same as 12.1.
  Result: PASS / FAIL
  Notes:
- [ ] 12.7 Single tile project saved and reopened: open `plain.arm` from section 1 and `udim_single_1002` saved.
  Expected: plain project has no tile UI; the 1002-only project still shows the `Tiles` panel with `1002` and exports `_1002`.
  Result: PASS / FAIL
  Notes:
- [ ] 12.8 Phase A project (a saved format "99" file from before, for example `p99.arm` from the phase A plan) opens.
  Expected: opens without error, coverage buttons unchanged, tile list `{1001}`, no Tiles button.
  Result: PASS / FAIL
  Notes:

- [ ] 12.9 Sculpt in a multi-tile project (review fix). `udim_2x2_multi.obj` fresh import (2048 per tile, atlas 4096). Layers tab: add a sculpt layer, sculpt a visible bump on `Body` in tile 1001 and on `Head`. Save `sc.arm`, close, reopen.
  Expected: both bumps are back exactly as sculpted, no scrambled spikes or flattened areas. Sculpt again after reopening: works without jumps.
  Result: PASS / FAIL
  Notes:
- [ ] 12.10 Mesh-only .arm into a 4-tile project, then paint right away (review fix). With `udim_2x2_multi.obj` open, File > Import > Mesh... a mesh-only `.arm` (exported earlier with File > Export > Mesh, format `.arm`, of the default cube). Without switching layers, paint a stroke on the imported cube and on `Head`.
  Expected: the cube lands in tile 1001 (2D view slot 1001); strokes appear under the cursor on both objects, never in another slot.
  Result: PASS / FAIL
  Notes:
- [ ] 12.11 Footprint with vertex colours (review fix). Import an OBJ that has vertex colours (or a mesh with a second UV map) with `UDIM tiles` `Off`, restrict a layer to one of its objects with the Objects button, enable path tracing (viewport mode `Path Traced`) and export `Visible`.
  Expected: the layer shows only on that object in the path traced view and in the exported files, same footprint as the raster viewport (no garbage shapes, no missing areas).
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 13. B7 migration of the old name-based UDIM project

Uses `old_udim.arm` from 0.4 (`Body.1001` / `Body.1002`, project resolution R, layer `Shared` crossing both tiles, layer `OnlyB2` restricted to `Body.1002`).

- [ ] 13.1 File > Open... `old_udim.arm` in the new build.
  Expected: opens without a crash or errors. Meshes tab: objects keep their names `Body.1001` and `Body.1002`. `Tiles` panel lists `1001` and `1002` (2 tiles) with per-tile resolution `R/2 x R/2` (for R = 2048: `1024 x 1024`). Edit menu line reads `2 tiles, atlas <R> px, ...` (atlas = old resolution).
  Result: PASS / FAIL
  Notes:
- [ ] 13.2 Compare the viewport with the upstream screenshot (same camera).
  Expected: viewport pixels unchanged: the `Shared` stroke across both cubes is where it was, the `OnlyB2` stroke only on the right cube (`Body.1002`).
  Result: PASS / FAIL
  Notes:
- [ ] 13.3 Open the 2D view for `Shared`.
  Expected: the atlas positions are the same as before (`1001` in the first slot at the top left, `1002` to its right, same pixels). Grid labels `1001`, `1002`.
  Result: PASS / FAIL
  Notes:
- [ ] 13.4 Layers tab, row `OnlyB2`.
  Expected: the former single-tile layer now sits in its slot (2D view: its stroke is in slot `1002`, the rest of the atlas empty; its pixels are drawn into the slot, they were tile-local upstream). Its Objects button reads `Body.1002`, its Tiles button `Tiles: All`. `Shared` reads `Objects: All`, `Tiles: All`. Paint a new stroke on `OnlyB2`: it paints only the right cube.
  Result: PASS / FAIL
  Notes:
- [ ] 13.5 If you added a mask to `OnlyB2` upstream (optional): look at it in the 2D view.
  Expected: the mask is drawn into its slot too and white outside it (mask outside the slot is white), the layer shows as before.
  Result: PASS / FAIL
  Notes:
- [ ] 13.6 Meshes row tooltips.
  Expected: `Body.1001` -> `Tiles: 1001`, `Body.1002` -> `Tiles: 1002`.
  Result: PASS / FAIL
  Notes:
- [ ] 13.7 Painting after migration: new strokes on `Shared` across both cubes, **New > Fill Layer** with `Tiles: 1002`.
  Expected: normal behaviour as in section 6, the fill appears only on the right cube.
  Result: PASS / FAIL
  Notes:
- [ ] 13.8 File > Save As... `migrated.arm`, reopen.
  Expected: loads as a normal phase B project, all values same as 13.1 to 13.4. No more name-based handling: renaming `Body.1002` to `Foo` leaves the paint and Tiles as they were.
  Result: PASS / FAIL
  Notes:
- [ ] 13.9 Export `Per Tile` of the migrated project.
  Expected: files `<f>_base_1001.png`, `<f>_base_1002.png` at `R/2` px (1024 x 1024 for R = 2048); `OnlyB2`'s content only in the `_1002` file.
  Result: PASS / FAIL
  Notes:
- [ ] 13.10 Phase A leftovers: open an old plain v23 project (no UDIM), if you have one.
  Expected: converted as in the phase A plan (section 13), tile list `{1001}`.
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 14. Undo / redo across tile edits

Setup: `udim_2x2_multi.obj`.

- [ ] 14.1 Layer 1 Tiles popup: untick `All`, tick `1002`. Press ctrl+z.
  Expected: Edit menu shows `Undo Layer Object` (the same history entry is used for tile and object coverage), the button returns to `Tiles: All` and the paint reappears on every tile. ctrl+shift+z: back to `1002`.
  Result: PASS / FAIL
  Notes:
- [ ] 14.2 Each tick or untick in the Tiles popup is one undo step; the same for a group's Tiles popup.
  Expected: confirmed, with the exact sets restored.
  Result: PASS / FAIL
  Notes:
- [ ] 14.3 Strokes before and after a coverage change.
  Expected: undo/redo of strokes and coverage interleave correctly, pixels return exactly.
  Result: PASS / FAIL
  Notes:
- [ ] 14.4 Layout changes clear history. Paint several strokes (history has steps), then `Remove` an unused tile or `Append` a mesh with a new tile.
  Expected: Console `Tile layout changed, undo history was cleared`, the Edit menu's Undo and Redo entries are empty. After the relayout, new strokes undo normally.
  Result: PASS / FAIL
  Notes:
- [ ] 14.5 Redo after relayout is not possible (the redo list is empty). Changing resolution: Edit menu > Resolution `4096` then `2048` back.
  Expected: no crash, the paint is kept (scaled uniformly, tiles keep their slots), undo history behaves as upstream does for resolution changes.
  Result: PASS / FAIL
  Notes:
- [ ] 14.6 Undo deleting an object is not available (deferred).
  Expected: no crash; deleting an object leaves layers that covered it covering nothing (phase A behaviour).
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 15. Mesh export

- [ ] 15.1 `udim_2x2_multi.obj` fresh import. File > Export > Mesh..., tab `Export Mesh`: Format `OBJ`, Meshes `All` (the first entry), Export to `mesh_out/mesh.obj`.
  Expected: the file is written. Its `vt` lines hold UDIM UVs with u in 0..2 and v in 0..2 (for example `Head` vt with v between 1.02 and 1.98), not atlas UVs squeezed into 0..1.
  Result: PASS / FAIL
  Notes:
- [ ] 15.2 In a terminal: `python3 /Users/martin.crawford/Development/Workspaces/Game/armorpaint/docs/udim/testdata/check_tiles.py <path>/mesh.obj`.
  Expected: the same tiles per object as the source: tiles `[1001, 1002]` for `Body`, `[1011]` `Head`, `[1012]` `Arm` and `Leg`, `[1001]` `Prop`, `straddling=0` for all.
  Result: PASS / FAIL
  Notes:
- [ ] 15.3 Reimport the exported file (fresh, **UDIM tiles** `Auto`).
  Expected: the same tile list (`1001`, `1002`, `1011`, `1012`) and the same Meshes tooltips as in section 3.
  Result: PASS / FAIL
  Notes:
- [ ] 15.4 Export a single object (Meshes combo, pick `Head`) and a GLB, if the format exists (GLB export writes UDIM UVs too).
  Expected: `Head` only, `vt` v in 1..2. In the GLB the UVs also are UDIM UVs.
  Result: PASS / FAIL
  Notes:
- [ ] 15.5 Export a plain single-tile mesh.
  Expected: unchanged upstream output (UVs 0..1), nothing extra.
  Result: PASS / FAIL
  Notes:

Section result: PASS / FAIL

## 16. Known limits (expected, not failures)

Record any difference you see as a note, these are decided or deferred.

- Overlapping UVs of different objects inside one tile: a texture cannot tell them apart, exports and path tracing can differ from the viewport. Only the Objects button tooltip warns (section 9). Needs per-object texture sets (`DEFERRED.md`).
- Translucency with the path tracer (D5): with translucency on and a bottom layer restricted by object or tile, texels outside its coverage flatten to alpha 0, so path tracing and the exported opacity map show them transparent while the raster viewport shows them opaque. Left as on main (`DEFERRED.md`).
- glTF and Blend import stay on the Off (wrap) behaviour, no **UDIM tiles** option for them.
- Mesh nodes (`mesh_nodes_objects`) are positional: deleting or reordering objects can re-point a canvas.
- Per Object semantics differ from the design table (DECISIONS B5): Per Object iterates the objects covered by an OBJECT-RESTRICTED layer, not every object covered by any layer, so a non-UDIM project with only unrestricted layers exports once without a suffix. Unrestricted layers in each Per Object file are masked to that object's footprint.
- Raytraced lightmap bake squeezes the whole atlas into the script's resolution.
- The Bake Texture node repeats its atlas target per tile.
- The Bake Maps script `Resolution` combo does not say `(per tile)` (the script API has no UDIM query), it is per tile.
- CPU bake memory: a 16k atlas needs about 2 GB; only a warning above an 8192 atlas, per-tile CPU bake is deferred.
- Tile files of removed tiles are not deleted from the bake folder.
- The atlas clamp is not remembered: a project saved while clamped (for example 9 tiles at 8192 -> 5461 per tile) reopens with 5461 as its configured per-tile resolution; removing tiles afterwards keeps 5461. A sculpt layer saved while the atlas was clamped may not load correctly.
- An image node with `UV` `Atlas` used in a fill layer follows that layer's UV scale and angle (scale 1 and angle 0 show the baked map correctly).
- Tools other than the UV editor that change UVs in place (if any) may leave stale tile-local UVs for save; report any UV change that reverts after save and reopen.
- Crop is blocked with more than one tile.
- Beyond about 64 tiles at 2k or 16 at 4k the single atlas stops being practical (tile streaming deferred); the atlas side is capped at 16384.
- Mesh delete has no undo; "Join UDIM tile objects" is not built (use Merge Geometry); timeline keyframes of migrated tile-local layers and group masks over them are not migrated.
- Material and brush previews and the material bake export paint every tile (no tile discard) and do not repeat materials per tile.
- Hidden objects still count for footprint masks (the texture does not depend on visibility).
- Object names in export file names are not sanitized (a `/` or other odd characters in an object name can break the name).
- The `Per Tile` export writes every used tile even if empty.

## Watch for (riskiest spots)

- Relayout (append, Remove tile, 4.6, 4.10, 3.5, 14.4): undo textures of other layers, layer masks (must come back white in new slots), sculpt textures, layer previews, 16 and 32 bit projects, fill layers re-parsing, the merged mesh and UV caches (stale geometry after relayout), and the undo history being really empty.
- Atlas UV border texels: paint near the edge of a tile slot in the 2D view must not appear in the neighbouring slot, and a vertex on a tile border must not resolve to the wrong slot (6.14, 2.9).
- Footprint mask cost in path trace mode: an R8 scratch target (256 MB at a 16k atlas) is held while an object-restricted layer is flattened; check stalls and memory in 8.6.
- B7 tile-local layers and masks (section 13): single-tile layers must land in the correct slot, their masks white outside it, layers with more than one tile object must stay atlas-wide. Also files with more than 64 tile ids (console warning).
- Save round trip of mesh tiles (12.4): linked duplicates whose tile data differs from their source, duplicates with two tiles, mesh-only .arm imports, re-save after load (12.2).
- FBX import (2.10): object names with suffixes, the whole-scene scan for Auto, straddle count, UVs above 1 now wrapping like OBJ.
- Export crop off-by-one (10.7): slot rectangle origin in each tile file, one pixel shifts, atlas sizes that are not a power of two (stride 3, 5461 px per tile).
- Bake tile crops with non-divisible sizes (11.8, 4.4 in a 9-tile project with the bake): atlas = per tile x stride, 16383 vs 16384, tile files must have the per-tile size and the atlas the image node uses must match.
- Sculpt seam coverage (6.10, 6.11): any vertex on a covered tile counts, so seam vertices must move together without cracks.
- Appended projects' tiles (4.6 to 4.9, 12.5): objects appended without tile data must go to 1001, the existing list must stay sorted and distinct, and layers made before an append must keep their `Tiles` sets.
- Sculpt layers in multi-tile projects across save and reopen (12.9), mesh-only .arm import into a UDIM project (12.10), footprints of meshes with vertex colours or a second UV map (12.11): all three were fixed after the final code review and never run.
- Mixing coverage: a layer with both Objects and Tiles restrictions, groups with their own sets (D1 intersection) and the greying in both popups.
