# UDIM phase B: manual test plan

Round 2. In round 1 the tester stopped at section 2. The tests "Ensure a tiling-UV mesh imports without tiles when Auto" (old 2.3), "Ensure a mesh entirely in tile 1002 gets tiles when Auto" (old 2.7) and the Reimport-with-Off test failed and are retested after fixes.
FBX import was also reported broken (see the FBX tests 2.20 to 2.22).
All checkboxes are unchecked for this round.

Branch `feat/udim-integration`, phase B (B1 UDIM tile import, B2 tile list and atlas relayout, B3 tile coverage in viewport/paint/sculpt, B4 footprint masks, B5 export modes, B6 bakes, B7 migration of name-based UDIM projects), commits `ba6723e84` to `722bba729` plus the round 1 fixes.
Every step below is to be checked by hand on the current tip. For each test tick PASS or FAIL, tick every assert and fill in `Notes:`. Then fill in the summary.

On macOS, ctrl in shortcuts means cmd. "Layer row" means a row in the **Layers** tab. Every layer and group row has an **Objects button** (reads `Objects: All`, an object name, `Name1, Name2`, `N objects` or `No objects`). With more than one tile the row also has a **Tiles button** to its right (reads `Tiles: All`, `1012`, `1001, 1002`, `N tiles` or `No tiles`). Clicking either opens a popup with an `All` check, a separator and one check per object or tile. The popup stays open while you toggle.
Console messages show in the **Console** tab (and briefly in the status bar).
"Tile" means a UDIM tile id: `1001 + floor(u) + 10 * floor(v)`. The project atlas has `stride x stride` slots, `stride` = smallest s with s*s >= tile count, slots are filled in sorted tile order row by row, so in the 2D view tile 1001 is at the top left, the next tile to its right.

## Summary

| # | Section | Result |
|---|---------|--------|
| 0 | Prerequisites (build, test data, old project) | n/a |
| 1 | Non-UDIM regression | PASS / FAIL |
| 2 | Import: UDIM tiles Auto / On / Off, Reimport, OBJ and FBX, Split By | PASS / FAIL |
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
| 17 | Bake Texture node per tile (branch `feat/udim-bake-node-per-tile`) | PASS / FAIL |

Phase B passes when sections 1 to 15 all pass. Any crash, lost paint after relayout/save/reload, paint on an uncovered tile or object, or an export tile that does not match the viewport is a FAIL.

## 0. Prerequisites

0.1 Build:
```
cd paint && ../base/make
cd build && xcodebuild -project ArmorPaint.xcodeproj -scheme ArmorPaint -configuration Release -derivedDataPath <dd> build
```
The app built for this round (derivedDataPath `.../scratchpad/dd_integration`) is at
`/private/tmp/claude-501/-Users-martin-crawford-Development-Workspaces-Game-armorpaint/b8bfd9f3-2c7e-4d01-9c91-d7daa0a9e8e7/scratchpad/dd_integration/Build/Products/Release/ArmorPaint.app`
(it lives in a temp folder and can vanish; rebuilding anywhere else with another `<dd>` is fine).

0.2 Test data, all in `/Users/martin.crawford/Development/Workspaces/Game/armorpaint/docs/udim/testdata/` (tiles per object checked with `python3 check_tiles.py <obj>`). The files must be the regenerated ones: each OBJ object writes all `v`, then all `vt`, then all `f`, so each `o` imports as exactly ONE object. (Old copies import every face as its own object: `Floor`, `Floor.001`, `Crate.001`, `Body.001` ...; if you see `.001` copies, regenerate the files.)
- `udim_2x2_multi.obj` and `.fbx`: 5 objects, tiles 1001, 1002, 1011, 1012. `Body` (x=-4.5) spans 1001 (bottom half, 3 faces) and 1002 (3 faces). `Head` (x=-1.5) is on 1011. `Arm` (x=1.5) is on 1012 left half. `Leg` (x=4.5) is on 1012 right half. `Prop` (x=7.5) is on 1001 top half. Project tile count 4, stride 2.
- `udim_straddle.obj` and `.fbx`: object `Strip`, tiles 1001 and 1002, exactly 1 triangle crosses the u=1 border (centroid in 1001).
- `udim_tiling_uv.obj`: object `Floor`, a 2x2 quad plane standing upright facing the default camera, UVs 0..4 (tiling UVs), 8 straddling triangles, corners on whole numbers 0, 2, 4. Auto must decide Off.
- `udim_single_1002.obj`: object `Crate`, entirely in tile 1002.
- `udim_12_tiles.obj`: 12 cubes `T1001`..`T1006` and `T1011`..`T1016`, each on its own tile (6x2 layout in 3D, row 0 = 1001-1006, row 1 = 1011-1016). Stride 4.
- `udim_seam_shared_vertex.obj`: object `Seam`, two quads sharing an edge in 3D, left quad in 1001, right quad in 1002, with separate vt per side.
- `three_cubes_separate_uv.obj`: cubes `A`, `B`, `C` in disjoint quadrants of 1001. `three_cubes_overlapping_uv.obj`: same cubes, all three use the whole 0-1 tile.
- `udim_body_two_tiles.obj`: object `Body`, left cube in 1001, right cube in 1002 (used for migration).
- Regenerate: `cd docs/udim/testdata && python3 gen_phase_b.py .` (the phase B meshes) and `python3 gen_obj.py .` (the phase A cubes and `udim_body_two_tiles.obj`). FBX copies: `blender -b --factory-startup --python obj_to_fbx.py -- in.obj out.fbx`.
- Check expected tiles: `python3 check_tiles.py udim_2x2_multi.obj` prints `Body: tiles=[1001, 1002] ...`, `Head: tiles=[1011]`, `Arm: [1012]`, `Leg: [1012]`, `Prop: [1001]`, `straddling=0` for every object. `python3 check_tiles.py udim_straddle.obj` prints `Strip: straddling=1`; `udim_tiling_uv.obj` prints `Floor: ... straddling=8`.

0.3 How to open the dialogs used below:
- **Import Mesh**: File > Import > Mesh..., pick the file. The **Import Mesh** box shows **Split By** (radio `Object` / `Material`), **UDIM tiles** (radio `Auto` / `On` / `Off`, tooltip `Auto uses tiles when the UVs use a tile other than 1001 and no triangle crosses a tile border`) for OBJ and FBX only, then `Cancel`, `Import`, and a chevron button whose menu has `Append`. File > Import > Mesh... replaces the project's meshes and clears its layers (a fresh import). `Append` keeps meshes and layers and adds the new objects. File > Reimport Mesh opens the same box for the current mesh file and keeps the layers.
- **Export Textures**: File > Export > Textures... (box with Resolution, Color bits, **Layers** combo, Preset, Destination, Padding, Export).
- **Export Mesh**: File > Export > Mesh... (tab `Export Mesh`: Format, Meshes, Export).
- **Layers tab Edit menu**: the `Edit` button in the Layers tab header, first line is the resolution combo (label always `Resolution`; with more than one tile the line under the combo says `4 tiles, atlas 4096 px, about 192 MB`, which is how you see that the value is per tile).
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

### 1.1 Ensure the default project shows no tile UI
[ ] PASS  [ ] FAIL
1. Start the new build (default cube). Open the Layers tab Edit menu.
   - [ ] The combo label reads `Resolution`
   - [ ] There is no `tiles, atlas` line under the combo
2. Look at a layer row.
   - [ ] There is no Tiles button on the row
3. Open the Meshes tab and the Layers tab `Filter` combo.
   - [ ] There is no `Tiles` panel in the Meshes tab
   - [ ] There are no `Tile ...` entries in the `Filter` combo
Notes: ______

### 1.2 Ensure the Brush paints as upstream
[ ] PASS  [ ] FAIL
1. Paint with the Brush across the cube.
   - [ ] The stroke appears on the cube
   - [ ] The Console shows no errors
Notes: ______

### 1.3 Ensure a Fill Layer works as upstream
[ ] PASS  [ ] FAIL
1. Click **New > Fill Layer**.
   - [ ] The fill covers the cube
   - [ ] The Console shows no errors
Notes: ______

### 1.4 Ensure a layer mask works as upstream
[ ] PASS  [ ] FAIL
1. Add a mask to a layer and paint on it.
   - [ ] The mask hides and shows the layer as on upstream
   - [ ] The Console shows no errors
Notes: ______

### 1.5 Ensure hiding a layer works as upstream
[ ] PASS  [ ] FAIL
1. Add a second paint layer and paint on it.
2. Hide the second layer with its eye.
   - [ ] The second layer's paint disappears from the cube
   - [ ] The Console shows no errors
Notes: ______

### 1.6 Ensure the Export Textures box lists no tile modes in a plain project
[ ] PASS  [ ] FAIL
1. File > Save As... `plain.arm` into a scratch folder.
2. File > Export > Textures... and open the **Layers** combo.
   - [ ] The combo lists `Visible`, `Selected`, `Per Object` only (no tile modes)
   - [ ] The resolution label reads `Resolution`
Notes: ______

### 1.7 Ensure a plain Visible export writes the upstream files
[ ] PASS  [ ] FAIL
1. In the Export Textures box set **Layers** to `Visible` and click Export.
   - [ ] Files written: `plain_base.png`, `plain_nor.png`, `plain_occ.png`, `plain_rough.png`, `plain_metal.png` (preset `generic`)
   - [ ] Each file is 2048 x 2048 (the project resolution)
   - [ ] No file has a `_1001` suffix or an object name
Notes: ______

### 1.8 Ensure a plain project reopens unchanged
[ ] PASS  [ ] FAIL
1. File > Open... `plain.arm`.
   - [ ] The same layers and paint are there
   - [ ] The Console shows no errors
Notes: ______

### 1.9 Ensure path tracing shows the paint in a plain project
[ ] PASS  [ ] FAIL
1. Viewport Mode > `Path Traced`, wait for a few samples.
   - [ ] The paint shows in the path traced view as in the raster view
   - [ ] The Console shows no errors
2. Switch back to the normal viewport mode.
   - [ ] The raster view returns
Notes: ______

### 1.10 Ensure a single-tile three-cube import is silent
[ ] PASS  [ ] FAIL
1. File > Import > Mesh... > `three_cubes_separate_uv.obj`, **Split By** `Object`, **UDIM tiles** `Auto`, click Import.
   - [ ] The Meshes tab lists exactly A, B, C (no `.001` copies)
   - [ ] The Edit menu still reads `Resolution`
   - [ ] There is no Tiles button and no `Tiles` panel (the project has the single tile 1001)
   - [ ] The Console shows no message about tiles (the Auto import is silent)
Notes: ______

### 1.11 Ensure an object-restricted layer paints only that object
[ ] PASS  [ ] FAIL
1. In the `three_cubes_separate_uv.obj` project, open the Layer 1 Objects popup, untick `All`, tick `B`.
2. Paint a stroke across all three cubes.
   - [ ] Only cube B shows the stroke
Notes: ______

### 1.12 Ensure Per Object export writes only the covered object
[ ] PASS  [ ] FAIL
1. Save the project as `three.arm`. Add a second layer restricted to `{B}` and keep layer 1 unrestricted.
2. File > Export > Textures..., **Layers** `Per Object`, click Export.
   - [ ] Files like `three_B_base.png` are written for `B` only
   - [ ] No file is written for `A` or `C`
   - [ ] No file carries a tile suffix
Notes: ______

## 2. Import: UDIM tiles Auto / On / Off

Setup: for every import in this section use File > Import > Mesh... (fresh import), **Split By** `Object` unless said otherwise, then check the Meshes tab (object list, row tooltip), the Tiles panel at the bottom of the Meshes tab, the Layers tab Edit menu and the Console tab.

### 2.1 Ensure udim_2x2_multi.obj imports with tiles when Auto
[ ] PASS  [ ] FAIL
1. File > Import > Mesh... > `udim_2x2_multi.obj`, **UDIM tiles** `Auto`, click Import.
   - [ ] Tiles are on without any prompt
   - [ ] The Meshes tab lists exactly `Body`, `Head`, `Arm`, `Leg`, `Prop` (5 objects, no `.001` copies)
   - [ ] The `Tiles` panel lists `1001`, `1002`, `1011`, `1012`
   - [ ] The Console shows no warning
Notes: ______

### 2.2 Ensure the resolution combo shows the per-tile line after a UDIM import
[ ] PASS  [ ] FAIL
1. Import `udim_2x2_multi.obj` (fresh, Auto). Open the Layers tab Edit menu.
   - [ ] The combo label reads `Resolution` and the value reads `2048`
   - [ ] The line under the combo reads `4 tiles, atlas 4096 px, about 192 MB`
Notes: ______

### 2.3 Ensure the 2D view shows the 2x2 tile grid after a UDIM import
[ ] PASS  [ ] FAIL
1. Import `udim_2x2_multi.obj` (fresh, Auto). Open the 2D view (layer type).
   - [ ] The 2D view shows a 2x2 grid
   - [ ] `1001` is top left, `1002` top right, `1011` bottom left, `1012` bottom right
Notes: ______

### 2.4 Ensure UDIM tiles Off imports without tiles
[ ] PASS  [ ] FAIL
1. Import `udim_2x2_multi.obj`, **UDIM tiles** `Off`, click Import.
   - [ ] There is no `Tiles` panel
   - [ ] The Edit menu reads `Resolution` and has no line under the combo
   - [ ] There is no Tiles button on the layer row
   - [ ] The Meshes tab lists exactly `Body`, `Head`, `Arm`, `Leg`, `Prop`
Notes: ______

### 2.5 Ensure UDIM tiles Off wraps as upstream
[ ] PASS  [ ] FAIL
1. Import `udim_2x2_multi.obj`, **UDIM tiles** `Off`, click Import.
2. Paint a stroke on `Body`.
   - [ ] The stroke also appears on `Head`, `Arm`, `Leg`, `Prop` at the same UV spots (everything overlaps in 0..1), as in upstream
Notes: ______

### 2.6 Ensure a tiling-UV mesh imports without tiles when Auto
[ ] PASS  [ ] FAIL
Note: the plane stands upright facing the default camera. Off wraps each UV corner on its own (see Known limits), so brush strokes on this mesh do not show a repeating pattern; that is not tested here.
1. File > Import > Mesh... > `udim_tiling_uv.obj`, **UDIM tiles** `Auto`, click Import. Do not move the camera.
   - [ ] `Floor` is visible in the viewport right after import
   - [ ] The Meshes tab lists only `Floor` (no `.001` copies)
   - [ ] There is no `Tiles` panel
   - [ ] There is no line under the Resolution combo in the Edit menu
   - [ ] There is no Tiles button on the layer row
   - [ ] The Console shows no message about tiles or clamped triangles
2. Click **New > Fill Layer**.
   - [ ] The fill colour covers the whole plane
Notes: ______

### 2.7 Ensure a straddling OBJ stays without tiles when Auto
[ ] PASS  [ ] FAIL
1. Import `udim_straddle.obj`, **UDIM tiles** `Auto`, click Import.
   - [ ] The Meshes tab lists exactly `Strip`
   - [ ] There is no `Tiles` panel (Auto decides Off because a triangle crosses the border)
   - [ ] The Console shows no warning
Notes: ______

### 2.8 Ensure a straddling OBJ gets tiles and a clamp message with On
[ ] PASS  [ ] FAIL
1. Import `udim_straddle.obj`, **UDIM tiles** `On`, click Import. Open the Console tab.
   - [ ] The message `1 triangles cross UDIM tile borders and were clamped` appears once
   - [ ] The Meshes tab lists exactly `Strip`
   - [ ] The `Tiles` panel lists `1001` and `1002`
2. Hover the row `Strip` in the Meshes tab.
   - [ ] The tooltip reads `Strip` then `Tiles: 1001-1002`
3. Open the 2D view.
   - [ ] It shows a 2x2 grid with `1001` and `1002` in the top row
Notes: ______

### 2.9 Ensure only the clamped triangle is distorted on a straddling mesh
[ ] PASS  [ ] FAIL
1. Import `udim_straddle.obj`, **UDIM tiles** `On`. Paint a stroke across the middle of `Strip`.
   - [ ] Only the one clamped triangle is distorted
   - [ ] The other quads look right
Notes: ______

### 2.10 Ensure a lone 1002 mesh imports with tiles when On
[ ] PASS  [ ] FAIL
1. Import `udim_single_1002.obj`, **UDIM tiles** `On`, click Import.
   - [ ] The Meshes tab lists exactly `Crate`
   - [ ] The Tiles panel lists `1002` only (the row reads `1002`, `1 objects`, `2048 x 2048`, no Remove button)
   - [ ] The Edit menu label reads `Resolution` and there is no `tiles, atlas` line (only one tile)
2. Paint a stroke on `Crate`.
   - [ ] The mesh appears painted on all faces as drawn
Notes: ______

### 2.11 Ensure a lone 1002 mesh exports Per Tile as 1002
[ ] PASS  [ ] FAIL
1. Import `udim_single_1002.obj`, **UDIM tiles** `On`. Paint a stroke on `Crate`.
2. File > Export > Textures... and open the **Layers** combo.
   - [ ] The combo lists 5 entries and starts on a tile mode
3. Choose `Per Tile` and click Export.
   - [ ] Files `<f>_base_1002.png` etc. are written
   - [ ] No `_1001` file is written
   - [ ] Each file is 2048 x 2048
Notes: ______

### 2.12 Ensure a mesh entirely in tile 1002 gets tiles when Auto
[ ] PASS  [ ] FAIL
1. Import `udim_single_1002.obj`, **UDIM tiles** `Auto`, click Import.
   - [ ] The Meshes tab lists exactly `Crate`
   - [ ] The Tiles panel lists only `1002`
   - [ ] The Meshes row tooltip of `Crate` reads `Tiles: 1002`
   - [ ] There is no Tiles button on the layer row (single tile)
   - [ ] There is no line under the Resolution combo
Notes: ______

### 2.13 Ensure Split By Object with tiles On gives the four tiles
[ ] PASS  [ ] FAIL
1. Import `udim_2x2_multi.obj`, **UDIM tiles** `On`, **Split By** `Object`, click Import.
   - [ ] The tile list is `1001, 1002, 1011, 1012`
   - [ ] The Meshes tab lists exactly 5 objects (`Body`, `Head`, `Arm`, `Leg`, `Prop`)
Notes: ______

### 2.14 Ensure Split By Material with tiles On gives the four tiles
[ ] PASS  [ ] FAIL
1. Import `udim_2x2_multi.obj`, **UDIM tiles** `On`, **Split By** `Material`, click Import. (The file has no `usemtl`, so Material gives the same objects as Object.)
   - [ ] The tile list is `1001, 1002, 1011, 1012`
   - [ ] The Meshes tab lists exactly 5 objects (every `o` object is imported in every mode); record the count: ______
Notes: ______

### 2.15 Ensure a seam between two tiles is not stretched
[ ] PASS  [ ] FAIL
1. Import `udim_seam_shared_vertex.obj`, **UDIM tiles** `On`.
   - [ ] The Meshes tab lists exactly `Seam`
   - [ ] The Console shows no clamp message (no straddling)
2. Paint a stroke across the shared edge of the two quads on both sides.
   - [ ] The two quads show the stroke continuously across the 3D seam
3. Open the 2D view and look at the slots `1001` / `1002`.
   - [ ] No triangle is stretched across the atlas from slot `1001` to slot `1002`
   - [ ] The stroke appears in both slots near their facing borders (left quad in `1001`, right in `1002`)
Notes: ______

### 2.16 Ensure Reimport with UDIM tiles Off leaves no tiles behind
[ ] PASS  [ ] FAIL
1. File > Import > Mesh... `udim_2x2_multi.obj`, **UDIM tiles** `Auto`, click Import.
2. Paint a stroke with the Brush on `Body`.
3. File > Reimport Mesh (the Import Mesh box opens). Set **UDIM tiles** `Off` and click Import.
   - [ ] The Console shows `Tile layout changed, undo history was cleared`
   - [ ] The Console shows `Tiles 1002, 1011, 1012 are not used by the imported meshes and were removed with their pixels`
   - [ ] There is no `Tiles` panel in the Meshes tab (no `(unused)` tile rows)
   - [ ] There is no line under the Resolution combo
   - [ ] There is no Tiles button on the layer rows
   - [ ] The layers are kept (Reimport keeps layers)
   - [ ] The Meshes tab lists `Body`, `Head`, `Arm`, `Leg`, `Prop`
Notes: ______

### 2.17 Ensure Reimport with Auto keeps the tile list and paint
[ ] PASS  [ ] FAIL
1. File > Import > Mesh... `udim_2x2_multi.obj` (fresh), **UDIM tiles** `Auto`, click Import.
2. Paint a stroke on `Body` (tile 1002 faces) and one on `Head`.
3. File > Reimport Mesh, **UDIM tiles** `Auto`, click Import.
   - [ ] The Tiles panel still lists `1001`, `1002`, `1011`, `1012`
   - [ ] No tile row reads `(unused)`
   - [ ] The strokes are still in place
   - [ ] The Console shows no `Tile layout changed` message
Notes: ______

### 2.18 Ensure Reimport with On after Off brings the tiles back
[ ] PASS  [ ] FAIL
1. Repeat steps 1 to 3 of 2.16 (Auto import, stroke on `Body`, Reimport with `Off`).
2. File > Reimport Mesh, **UDIM tiles** `On`, click Import.
   - [ ] The Tiles panel lists `1001`, `1002`, `1011`, `1012`
   - [ ] The Console shows `Tile layout changed, undo history was cleared`
   - [ ] The paint in tile 1001 is kept (the single-tile layer content stays in tile 1001, 2D view slot `1001`)
Notes: ______

### 2.19 Ensure a fresh import with Off after a UDIM import leaves no tiles
[ ] PASS  [ ] FAIL
1. File > Import > Mesh... `udim_2x2_multi.obj`, **UDIM tiles** `Auto`, click Import.
2. File > Import > Mesh... the same file, **UDIM tiles** `Off`, click Import.
   - [ ] There is no `Tiles` panel
   - [ ] There is no line under the Resolution combo
   - [ ] There is no Tiles button on the layer rows
Notes: ______

FBX note: the committed FBX files are valid (Blender 5.2 binary FBX 7400, one UV set, checked with the importer code outside the app). If an FBX test fails, record the file, the UDIM tiles and Split By settings, Import or Append, and exactly what looks wrong (size, shading, tiles, crash, console message).

### 2.20 Ensure FBX imports with UDIM tiles when Auto
[ ] PASS  [ ] FAIL
1. File > Import > Mesh... `udim_2x2_multi.fbx`, **UDIM tiles** `Auto`, click Import. Do not move the camera.
   - [ ] The Meshes tab lists `Body`, `Head`, `Arm`, `Leg`, `Prop` (names may carry Blender suffixes, record them: ______)
   - [ ] The objects are visible in the viewport at a sensible size right after import
   - [ ] The Tiles panel lists `1001`, `1002`, `1011`, `1012`
   - [ ] The Meshes row tooltips are the same as for the OBJ in 2.1 (see 3.1)
2. Paint a stroke on `Body`.
   - [ ] The stroke appears on `Body`
Notes: ______

### 2.21 Ensure a straddling FBX stays without tiles when Auto
[ ] PASS  [ ] FAIL
1. File > Import > Mesh... `udim_straddle.fbx`, **UDIM tiles** `Auto`, click Import.
   - [ ] The `Strip` object is visible in the viewport at a sensible size
   - [ ] There is no `Tiles` panel (tiles off)
   - [ ] The Console shows no message
Notes: ______

### 2.22 Ensure a straddling FBX clamps with tiles On
[ ] PASS  [ ] FAIL
1. File > Import > Mesh... `udim_straddle.fbx`, **UDIM tiles** `On`, click Import.
   - [ ] The Tiles panel lists `1001`, `1002`
   - [ ] The Console shows `1 triangles cross UDIM tile borders and were clamped`
   - [ ] The `Strip` object is visible in the viewport at a sensible size
Notes: ______

### 2.23 Ensure a fresh multi-tile import resets the resolution
[ ] PASS  [ ] FAIL
1. Start from a default project. Edit menu > Resolution `4096`.
2. Import `udim_2x2_multi.obj` (fresh, Auto). Open the Edit menu.
   - [ ] The combo reads `2048`
   - [ ] The line under it reads `4 tiles, atlas 4096 px, about 192 MB`
Notes: ______

### 2.24 Ensure a plain mesh import after that does not change the resolution again
[ ] PASS  [ ] FAIL
1. Continue from 2.23. Import a plain single-tile mesh (`three_cubes_separate_uv.obj`, fresh).
   - [ ] The resolution is not changed again by this import
Notes: ______

### 2.25 Ensure glTF and Blend imports stay Off
[ ] PASS  [ ] FAIL
Optional: import any `.glb` or `.blend` with UVs above 1, if available.
1. File > Import > Mesh... pick the `.glb` or `.blend`.
   - [ ] The Import Mesh box has no **UDIM tiles** radio
2. Click Import.
   - [ ] The mesh uses wrap behaviour
   - [ ] There is no `Tiles` panel
Notes: ______

### 2.26 Ensure Reimport keeps the paint when Auto moves a single tile from 1001 to 1002
[ ] PASS  [ ] FAIL
1. File > Import > Mesh... `udim_single_1002.obj`, **UDIM tiles** `Off`, click Import.
2. Paint a bold stroke on `Crate`. Take a viewport screenshot (fixed camera).
3. File > Reimport Mesh, **UDIM tiles** `Auto`, click Import.
   - [ ] The Tiles panel lists only `1002`
   - [ ] The stroke on `Crate` is unchanged (compare with the screenshot)
   - [ ] The Console shows no `Tile layout changed` and no `were removed with their pixels` message
4. File > Reimport Mesh, **UDIM tiles** `Off`, click Import.
   - [ ] There is no `Tiles` panel
   - [ ] The stroke on `Crate` is still unchanged
Notes: ______

## 3. Tile list, Meshes tab and Remove

Setup: import `udim_2x2_multi.obj` (fresh, Auto).

### 3.1 Ensure object row tooltips show their tiles
[ ] PASS  [ ] FAIL
1. Hover `Body` in the Meshes tab.
   - [ ] The tooltip second line reads `Tiles: 1001-1002`
2. Hover `Head`.
   - [ ] The second line reads `Tiles: 1011`
3. Hover `Arm`.
   - [ ] The second line reads `Tiles: 1012`
4. Hover `Leg`.
   - [ ] The second line reads `Tiles: 1012`
5. Hover `Prop`.
   - [ ] The second line reads `Tiles: 1001`
Notes: ______

### 3.2 Ensure the Tiles panel lists the four tiles
[ ] PASS  [ ] FAIL
1. Scroll to the `Tiles` panel at the bottom of the Meshes tab.
   - [ ] There are four rows, each with the id, `N objects` and `2048 x 2048`
   - [ ] `1001` reads `2 objects` (Body, Prop)
   - [ ] `1002` reads `1 objects`
   - [ ] `1011` reads `1 objects`
   - [ ] `1012` reads `2 objects` (Arm, Leg)
Notes: ______

### 3.3 Ensure used tiles have no Remove button
[ ] PASS  [ ] FAIL
1. Look at every row of the `Tiles` panel.
   - [ ] No row has a `Remove` button (all tiles are used)
Notes: ______

### 3.4 Ensure deleting an object marks its tile unused
[ ] PASS  [ ] FAIL
1. Paint a stroke on `Head` and one on `Arm`. Note the colours.
2. In the Meshes tab, right-click `Head` > Delete.
   - [ ] The Head disappears
   - [ ] In the `Tiles` panel the row `1011` reads `0 objects` and shows a `Remove` button
   - [ ] The other rows have no `Remove` button
3. Open a layer's Tiles popup.
   - [ ] It lists `1011 (unused)`
Notes: ______

### 3.5 Ensure Remove asks for confirmation and Cancel changes nothing
[ ] PASS  [ ] FAIL
1. Continue from 3.4. Click `Remove` on row `1011`.
   - [ ] A box titled `Remove tile` opens with `Remove tile 1011? Its pixels are deleted and undo history is cleared.`
   - [ ] The box has the buttons `Cancel` and `Remove`
2. Click `Cancel`.
   - [ ] The box closes and nothing changes
Notes: ______

### 3.6 Ensure Remove deletes the tile and relayouts the atlas
[ ] PASS  [ ] FAIL
1. Continue from 3.5. Click `Remove` on row `1011` again, then `Remove` in the box.
   - [ ] The Console shows `Tile layout changed, undo history was cleared`
   - [ ] The Tiles panel lists `1001`, `1002`, `1012`
   - [ ] The Edit menu line reads `3 tiles, atlas 4096 px, about 192 MB` (stride stays 2)
Notes: ______

### 3.7 Ensure Remove keeps the other tiles' paint and slots
[ ] PASS  [ ] FAIL
1. Continue from 3.6. Open the 2D view.
   - [ ] The 2D view shows `1001`, `1002`, `1012` (the 1012 slot moved to bottom left)
   - [ ] The stroke on `Arm` is still in place in the viewport
   - [ ] The stroke on `Arm` is still in place in the 2D view slot `1012`
Notes: ______

### 3.8 Ensure Remove clears the undo history and painting still works
[ ] PASS  [ ] FAIL
1. Continue from 3.6. Open the Edit menu.
   - [ ] Edit > Undo has no step (empty or greyed)
2. Paint a stroke.
   - [ ] Painting still works
Notes: ______

### 3.9 Ensure used tiles have no Remove button after a Remove
[ ] PASS  [ ] FAIL
1. Continue from 3.6. Look at the rows `1001`, `1002`, `1012`.
   - [ ] No row shows a `Remove` button
2. Look at a project with only one tile.
   - [ ] The Remove button never shows
Notes: ______

## 4. Grid growth, memory budget, clamp, append

### 4.1 Ensure 12 tiles show the combo and per-tile line
[ ] PASS  [ ] FAIL
1. Import `udim_12_tiles.obj` (fresh, Auto). Open the Layers tab Edit menu.
   - [ ] The combo `Resolution` reads `2048`
   - [ ] The line reads `12 tiles, atlas 8192 px, about 768 MB` (stride 4, one layer)
   - [ ] There is no clamp line
Notes: ______

### 4.2 Ensure 12 tiles show a 4x4 grid in the 2D view
[ ] PASS  [ ] FAIL
1. Import `udim_12_tiles.obj` (fresh, Auto). Open the 2D view.
   - [ ] The 2D view shows a 4x4 grid with tile ids in slot order
   - [ ] Row 1 reads 1001..1004
   - [ ] Row 2 reads 1005, 1006, 1011, 1012
   - [ ] Row 3 reads 1013..1016
Notes: ______

### 4.3 Ensure 12 tiles list 12 objects and 12 tile rows
[ ] PASS  [ ] FAIL
1. Import `udim_12_tiles.obj` (fresh, Auto). Look at the Meshes tab.
   - [ ] The Meshes tab lists exactly the 12 objects `T1001`..`T1006`, `T1011`..`T1016`
   - [ ] The Tiles panel lists all 12 tiles, each `1 objects`
Notes: ______

### 4.4 Ensure Resolution 4096 fits the atlas limit with 12 tiles
[ ] PASS  [ ] FAIL
1. Continue from 4.1. Edit menu > Resolution: choose `4096`.
   - [ ] The line reads `12 tiles, atlas 16384 px, about 3072 MB`
   - [ ] There is no clamp line (4 x 4096 = 16384)
Notes: ______

### 4.5 Ensure Resolution 8192 is clamped with 12 tiles
[ ] PASS  [ ] FAIL
Needs several GB of GPU memory; skip if the machine cannot take it and mark Notes.
1. Continue from 4.4. Choose `8192`.
   - [ ] The clamp line `Clamped to 4096 px per tile (atlas limit 16384)` appears
   - [ ] The atlas stays `16384 px`
   - [ ] The `Tiles` panel shows `4096 x 4096` per tile
   - [ ] The paint is not corrupted
Notes: ______

### 4.6 Ensure removing three tiles leaves nine
[ ] PASS  [ ] FAIL
1. Continue from 4.5. Set Resolution back to `2048`.
2. Delete `T1004`, `T1005`, `T1006` (Meshes tab, right-click > Delete).
3. `Remove` their tiles 1004 to 1006 in the Tiles panel (three confirmations).
   - [ ] The Tiles panel lists 9 tiles (stride 3)
Notes: ______

### 4.7 Ensure Resolution 8192 is clamped with 9 tiles
[ ] PASS  [ ] FAIL
1. Continue from 4.6. Set Resolution `8192`.
   - [ ] The line reads `9 tiles, atlas 16383 px, about 3072 MB`
   - [ ] The clamp line reads `Clamped to 5461 px per tile (atlas limit 16384)` (16384 / 3 = 5461)
   - [ ] The Tiles panel shows `5461 x 5461`
2. Set Resolution back to `2048`.
   - [ ] The value returns to `2048`
Notes: ______

### 4.8 Ensure the line under the combo appears only with more than one tile
[ ] PASS  [ ] FAIL
1. Open the Edit menu in a plain default cube project.
   - [ ] There is no line under the combo
2. Import `udim_single_1002.obj` (fresh, Auto). Open the Edit menu.
   - [ ] There is no line under the combo (one tile)
3. Import `udim_2x2_multi.obj` (fresh, Auto). Open the Edit menu.
   - [ ] The line `4 tiles, atlas 4096 px, about 192 MB` is shown
Notes: ______

### 4.9 Ensure the label reads Resolution in every project
[ ] PASS  [ ] FAIL
1. In a plain default cube project open the Edit menu and then the Export Textures box.
   - [ ] The label reads `Resolution` in both
2. In a `udim_single_1002.obj` project open the Edit menu and the Export Textures box.
   - [ ] The label reads `Resolution` in both
3. In a `udim_2x2_multi.obj` project open the Edit menu and the Export Textures box.
   - [ ] The label reads `Resolution` in both (never `Resolution (per tile)`)
Notes: ______

### 4.10 Ensure appending a new tile relayouts the atlas
[ ] PASS  [ ] FAIL
1. Import `udim_2x2_multi.obj` (fresh). Paint a distinct stroke on `Body`, `Head`, `Arm`, `Prop`.
2. Take a viewport screenshot (fixed camera).
3. File > Import > Mesh... `udim_12_tiles.obj`, click the chevron button next to `Import` and choose `Append`.
   - [ ] The Console shows `Tile layout changed, undo history was cleared`
   - [ ] The tile list grows to `1001`-`1006`, `1011`-`1016` (12 tiles) in the Tiles panel
   - [ ] The Edit line reads `12 tiles, atlas 8192 px, about 768 MB`
Notes: ______

### 4.11 Ensure appending a new tile keeps paint in place
[ ] PASS  [ ] FAIL
1. Continue from 4.10 and compare with the screenshot.
   - [ ] The five original objects look identical to the screenshot (strokes in the same places)
   - [ ] The new `T10xx` objects are empty (default colour)
Notes: ______

### 4.12 Ensure appending a new tile moves the old tiles to their new slots
[ ] PASS  [ ] FAIL
1. Continue from 4.10. Open the 2D view.
   - [ ] The old tiles show at their new slots with the strokes intact
   - [ ] `1001` and `1002` are still top left
   - [ ] `1011` moved to row 2, column 3
   - [ ] `1012` moved to row 2, column 4
2. Open the Edit menu.
   - [ ] Edit > Undo is empty
Notes: ______

### 4.13 Ensure appending a tile mesh to a plain project adds the tile
[ ] PASS  [ ] FAIL
1. In the default cube project paint a stroke on the cube.
2. Import `udim_single_1002.obj` with `Append`.
   - [ ] The tile list is `1001`, `1002`
   - [ ] The Edit menu label reads `Resolution` and the line under it shows the 2 tiles
Notes: ______

### 4.14 Ensure appending to a plain project keeps the cube paint
[ ] PASS  [ ] FAIL
1. Continue from 4.13.
   - [ ] The cube keeps its stroke (it is in tile 1001, slot 1)
   - [ ] `Crate` is empty and sits on `1002`
Notes: ______

### 4.15 Ensure appending a single-tile mesh to a tile project changes nothing
[ ] PASS  [ ] FAIL
1. In a project with tiles (`udim_2x2_multi.obj`), paint a stroke. Append `three_cubes_separate_uv.obj`.
   - [ ] The new objects go to tile 1001
   - [ ] The tile list is unchanged
   - [ ] There is no layout-changed message in the Console
   - [ ] The existing paint is unchanged
Notes: ______

### 4.16 Ensure an append that exceeds the atlas limit warns and clamps
[ ] PASS  [ ] FAIL
Optional (about 3 GB GPU memory).
1. Set `udim_2x2_multi.obj` (fresh) to Resolution `8192` (atlas 16384, no clamp).
2. `Append` `udim_12_tiles.obj`.
   - [ ] The Console shows `UDIM atlas too large: 12 tiles use 4096 px per tile (16384 px atlas)`
   - [ ] The Edit menu shows the clamp line `Clamped to 4096 px per tile (atlas limit 16384)`
Notes: ______

### 4.17 Ensure relayout keeps layer and mask pixels on the old tiles
[ ] PASS  [ ] FAIL
1. In a 4-tile project (`udim_2x2_multi.obj`) click **Add Mask** on a layer and paint black on the mask.
2. Add a second layer and paint on both layers. Take a screenshot.
3. `Append` `udim_12_tiles.obj`.
   - [ ] Both layers and the mask keep their pixels on the original tiles (compare screenshots)
Notes: ______

### 4.18 Ensure relayout fills new tiles with empty layers and white masks
[ ] PASS  [ ] FAIL
1. Continue from 4.17. Look at the new tiles in the 2D view.
   - [ ] The layer is empty on the new tiles
   - [ ] The mask is white (not black) on the new tiles
Notes: ______

### 4.19 Ensure relayout keeps the material channels
[ ] PASS  [ ] FAIL
1. Continue from 4.17. Look at roughness and metal of the old paint.
   - [ ] The material channels (roughness, metal) of the old paint are unchanged
Notes: ______

### 4.20 Ensure relayout keeps layer data in a 16bit project
[ ] PASS  [ ] FAIL
1. Open the Edit menu > `Bits` `16bit`. Repeat steps 1 to 3 of 4.17 and the checks of 4.18 and 4.19.
   - [ ] Both layers and the mask keep their pixels on the original tiles
   - [ ] New tiles: layer empty, mask white (not black)
   - [ ] Material channels (roughness, metal) of the old paint are unchanged
Notes: ______

## 5. Layer Tiles popup

Setup: `udim_2x2_multi.obj` fresh import.

### 5.1 Ensure a plain default project has no Tiles button
[ ] PASS  [ ] FAIL
1. Start a plain default cube project. Look at a layer row and the Layers tab `Filter` combo.
   - [ ] There is no Tiles button on the layer row (shown only with more than one tile)
   - [ ] There are no `Tile ...` filter entries
Notes: ______

### 5.2 Ensure a 1002-only project has no Tiles button
[ ] PASS  [ ] FAIL
1. Import `udim_single_1002.obj` (fresh, Auto). Look at a layer row and the Layers tab `Filter` combo.
   - [ ] There is no Tiles button on the layer row (a single tile 1002 also shows none)
   - [ ] There are no `Tile ...` filter entries
Notes: ______

### 5.3 Ensure a layer row shows both coverage buttons in a 4-tile project
[ ] PASS  [ ] FAIL
1. In the 4-tile project look at the layer row of `Layer 1`.
   - [ ] Two buttons sit side by side: `Objects: All` and `Tiles: All`
Notes: ______

### 5.4 Ensure the Tiles button has a tooltip
[ ] PASS  [ ] FAIL
1. Hover the Tiles button.
   - [ ] The tooltip reads `Limit this layer to the selected UDIM tiles.`
Notes: ______

### 5.5 Ensure the Tiles popup lists all tiles ticked and greyed with All
[ ] PASS  [ ] FAIL
1. Click `Tiles: All`.
   - [ ] The popup shows `All` ticked, a separator, then `1001`, `1002`, `1011`, `1012`
   - [ ] The tile rows are all ticked and greyed out
Notes: ______

### 5.6 Ensure clicking a greyed tile row does nothing
[ ] PASS  [ ] FAIL
1. Continue from 5.5. Click a greyed tile row.
   - [ ] Nothing changes
   - [ ] No undo step is added
Notes: ______

### 5.7 Ensure ticking tiles updates the button text
[ ] PASS  [ ] FAIL
1. Open the Tiles popup. Untick `All`, tick `1002` and `1012`.
   - [ ] The popup stays open
   - [ ] The button reads `1002, 1012`
Notes: ______

### 5.8 Ensure three ticked tiles read as a count
[ ] PASS  [ ] FAIL
1. Continue from 5.7. Tick `1011` also.
   - [ ] The button reads `3 tiles`
Notes: ______

### 5.9 Ensure unticking back to one tile shows its id
[ ] PASS  [ ] FAIL
1. Continue from 5.8. Untick `1011` and `1002`.
   - [ ] The button reads `1012`
Notes: ______

### 5.10 Ensure no tiles dims the layer name
[ ] PASS  [ ] FAIL
1. Continue from 5.9. Untick everything.
   - [ ] The button reads `No tiles`
   - [ ] The layer name is dimmed
   - [ ] Hovering shows `No tiles are selected for this layer or its group, so it covers nothing. Select tiles or choose All to use it.`
Notes: ______

### 5.11 Ensure All can be toggled without losing the stored list
[ ] PASS  [ ] FAIL
1. Tick `All` again, then untick it again.
   - [ ] The popup shows the stored list again (rows enabled, the earlier ticks kept)
Notes: ______

### 5.12 Ensure an unused tile is marked in the popup
[ ] PASS  [ ] FAIL
1. Delete `Head` so tile `1011` is unused (see 3.4). Open the Tiles popup.
   - [ ] The row reads `1011 (unused)`
   - [ ] The other rows show plain ids
Notes: ______

### 5.13 Ensure a group's tile set greys out the child's rows
[ ] PASS  [ ] FAIL
1. Click **New > Group**, drag a layer into it.
2. Open the group Tiles popup: untick `All`, tick `1012` only.
3. Open the child layer's Tiles popup.
   - [ ] The rows `1001`, `1002`, `1011` are greyed and unticked
   - [ ] Hovering one shows `Excluded by the group`
   - [ ] `1012` is enabled
Notes: ______

### 5.14 Ensure the child's own list survives the group going back to All
[ ] PASS  [ ] FAIL
1. Continue from 5.13. Set the group back to `All`.
   - [ ] Nothing is greyed in the child's popup
   - [ ] The child's own list is unchanged
Notes: ______

### 5.15 Ensure a child with nothing in common with its group is dimmed
[ ] PASS  [ ] FAIL
1. Set the group to `1001` and the child to `1012` only.
   - [ ] The child's name is dimmed (nothing in common)
   - [ ] Hovering shows the tiles tooltip from 5.10
Notes: ______

### 5.16 Ensure the Tiles popup never changes the Objects button
[ ] PASS  [ ] FAIL
1. Set a layer to `Objects: Body`.
2. Tick and untick tiles in its Tiles popup (end on `Tiles: 1002`).
   - [ ] The Objects button text stays `Objects: Body`
Notes: ______

### 5.17 Ensure the Objects popup never changes the Tiles button
[ ] PASS  [ ] FAIL
1. Continue from 5.16 (`Objects: Body`, `Tiles: 1002`). Tick and untick objects in the Objects popup (end on `Objects: Body`).
   - [ ] The Tiles button text stays `1002`
Notes: ______

### 5.18 Ensure the mini layers context menu has an Objects sub-menu
[ ] PASS  [ ] FAIL
1. Collapse the sidebar to mini mode if it is available. Right-click a layer.
   - [ ] The context menu has an `Objects` sub-menu with the same entries as the popup
Notes: ______

### 5.19 Ensure the mini layers context menu has a Tiles sub-menu with several tiles
[ ] PASS  [ ] FAIL
1. In the 4-tile project right-click a layer in the mini layers panel.
   - [ ] The context menu has a `Tiles` sub-menu with the same entries as the popup
Notes: ______

## 6. Painting, fills and sculpt restricted by tile / object / both

Setup: `udim_2x2_multi.obj` fresh import, front camera so `Body` (faces on 1001 and 1002) is clearly visible. `Body`'s 3 faces on 1001 and 3 faces on 1002 are on different cube faces: check with a preliminary stroke on a spare layer that you can tell them apart.

### 6.1 Ensure a tile-restricted layer paints only that tile's faces
[ ] PASS  [ ] FAIL
1. Set Layer 1 to `Tiles: 1002` only. Drag a Brush stroke across the faces of `Body`.
   - [ ] Only the faces of `Body` that sit on tile 1002 get paint
   - [ ] The faces on tile 1001 stay unpainted, also where the stroke crosses them
   - [ ] Other objects on 1001 (`Prop`) get nothing
   - [ ] Objects on 1011 / 1012 (`Head`, `Arm`, `Leg`) get nothing
Notes: ______

### 6.2 Ensure Tiles 1012 paints Arm and Leg but not Head
[ ] PASS  [ ] FAIL
1. Set a layer to `Tiles: 1012` only. Drag a Brush stroke across `Head`, `Arm` and `Leg`.
   - [ ] `Arm` and `Leg` are painted
   - [ ] `Head` is not painted
2. Start a stroke on `Head`.
   - [ ] Record whether it adds an undo step (tile restriction is a shader discard, no pick, so one may be added): ______
Notes: ______

### 6.3 Ensure an object-restricted layer paints only that object on both its tiles
[ ] PASS  [ ] FAIL
1. Set a layer to `Tiles: All`, `Objects: Body` only. Drag a Brush stroke across `Body`, `Head`, `Prop`.
   - [ ] Only `Body` is painted, on both its tiles
2. Start a stroke on `Head`.
   - [ ] It is skipped without an undo step (as in phase A)
Notes: ______

### 6.4 Ensure Objects and Tiles restrictions apply as an intersection
[ ] PASS  [ ] FAIL
1. Set a layer to `Objects: Body` AND `Tiles: 1002`. Drag a stroke across `Body`.
   - [ ] Only the 1002 faces of `Body` are painted
Notes: ______

### 6.5 Ensure switching the intersection to tile 1001 paints the other faces
[ ] PASS  [ ] FAIL
1. Continue from 6.4. Switch the layer to `Tiles: 1001`. Drag a new stroke across `Body` and `Prop`.
   - [ ] Only the 1001 faces of `Body` are painted
   - [ ] `Prop` is never painted (not covered by Objects)
Notes: ______

### 6.6 Ensure an empty intersection covers nothing
[ ] PASS  [ ] FAIL
1. Switch the layer to `Objects: Prop`, `Tiles: 1002`.
   - [ ] The layer covers nothing
   - [ ] The layer name is dimmed
Notes: ______

### 6.7 Ensure a layer with no tiles paints nothing
[ ] PASS  [ ] FAIL
1. Set a layer to `No tiles`. Drag a Brush stroke.
   - [ ] Nothing is painted
   - [ ] The Console tab title shows `Layer covers no tiles` for a few seconds (not `Layer covers no objects`)
Notes: ______

### 6.8 Ensure the Fill tool respects the tile restriction
[ ] PASS  [ ] FAIL
1. Pick the **Fill** tool with a fill mode (`Mesh` / `Object`). Set the layer to `Tiles: 1002`. Click on `Body` and on `Prop`.
   - [ ] Only the covered part of tile 1002 is filled
   - [ ] Texels on tiles 1001, 1011, 1012 stay untouched
Notes: ______

### 6.9 Ensure a Fill Layer follows its tile set
[ ] PASS  [ ] FAIL
1. Click **New > Fill Layer** with `Tiles: 1011` set.
   - [ ] Only `Head` shows the fill right away
2. Change the layer's Tiles set.
   - [ ] The layer refills when the set changes
Notes: ______

### 6.10 Ensure 2D view painting respects the tile restriction
[ ] PASS  [ ] FAIL
1. Set a layer to `Tiles: 1002`. Open the 2D view (layer type).
2. Paint a stroke on slot `1001` (top left).
   - [ ] The stroke on slot `1001` does nothing
3. Paint a stroke on slot `1002` (top right).
   - [ ] The stroke on slot `1002` paints
   - [ ] The viewport shows only the 1002 stroke on `Body`
Notes: ______

### 6.11 Ensure other tools respect the tile restriction
[ ] PASS  [ ] FAIL
1. Set a layer to `Tiles: 1002`. With the Eraser, click or stroke on tile 1001.
   - [ ] Nothing changes outside tile 1002
2. Repeat with Clone.
   - [ ] Nothing changes outside tile 1002
3. Repeat with Blur.
   - [ ] Nothing changes outside tile 1002
4. Repeat with Decal.
   - [ ] Nothing changes outside tile 1002
5. Repeat with Text.
   - [ ] Nothing changes outside tile 1002
Notes: ______

### 6.12 Ensure a restriction covering every tile behaves like All
[ ] PASS  [ ] FAIL
1. Set a layer to `Tiles: 1001, 1002, 1011, 1012` (all ticked by hand, `All` unticked). Paint across every object.
   - [ ] It behaves like `All` (paint reaches every object)
   - [ ] There is no visible difference in speed or look
Notes: ______

### 6.13 Ensure a tile-restricted sculpt layer deforms only covered objects
[ ] PASS  [ ] FAIL
1. Click **New > Sculpt Layer** (sculpt tools mode). Set Tiles `1012` only. Sculpt (Brush or Grab) across `Arm`, `Leg` and `Head`.
   - [ ] `Arm` and `Leg` deform
   - [ ] `Head` does not deform
Notes: ______

### 6.14 Ensure sculpting tile 1002 does not tear the border
[ ] PASS  [ ] FAIL
1. Set the sculpt layer's tiles to `1002`. Sculpt across `Body`.
   - [ ] Only the vertices on `Body`'s 1002 faces deform
   - [ ] The shared edge vertices at the 1001/1002 border deform too
   - [ ] The surface does not tear (no cracks along the border)
Notes: ______

### 6.15 Ensure seam vertices move together with Tiles 1002
[ ] PASS  [ ] FAIL
1. Import `udim_seam_shared_vertex.obj` (On). Click **New > Sculpt Layer**, set Tiles `1002` only. Grab or inflate on the shared edge.
   - [ ] The shared edge vertices move together with the 1002 quad
   - [ ] The surface stays closed (the left quad is not left behind with a gap)
Notes: ______

### 6.16 Ensure seam vertices move together with Tiles 1001
[ ] PASS  [ ] FAIL
1. Continue from 6.15. Set Tiles `1001` only. Grab or inflate on the shared edge.
   - [ ] The shared edge vertices move together with the 1001 quad
   - [ ] There is no tearing
Notes: ______

### 6.17 Ensure a sculpt layer with no tiles does nothing
[ ] PASS  [ ] FAIL
1. Set a sculpt layer to `No tiles`. Sculpt on the mesh.
   - [ ] Nothing deforms
   - [ ] The Console tab title shows `Layer covers no tiles`
   - [ ] No undo step is added
Notes: ______

### 6.18 Ensure material and brush previews ignore tile restrictions
[ ] PASS  [ ] FAIL
1. Open a material in the Materials tab and look at the preview.
   - [ ] Previews show the material normally (not cut by any tile restriction)
Notes: ______

### 6.19 Ensure a decal paints only on the covered tile
[ ] PASS  [ ] FAIL
1. Apply a decal on a tile-restricted layer (covered tile).
   - [ ] The decal paints only on the covered tile
Notes: ______

### 6.20 Ensure tile borders show no bleed
[ ] PASS  [ ] FAIL
1. Paint a bold stroke along the border between two faces on the same tile slot.
2. Look closely at the tile edge in the 2D view slot.
   - [ ] There is no bleed of one slot into the neighbouring slot at the seams of the mesh
   - [ ] There is no dark or lighter line along tile borders of an unrestricted layer
Notes: ______

## 7. Layer filter by tile

Setup: `udim_2x2_multi.obj`. Layers: `L_all` (Tiles All), `L1012` (Tiles `1012`), `L1001` (Tiles `1001`).

### 7.1 Ensure the Filter combo lists tile entries in a 4-tile project
[ ] PASS  [ ] FAIL
1. Open the Layers tab `Filter` combo.
   - [ ] The entries are `All`, the 5 objects, then `Tile 1001`, `Tile 1002`, `Tile 1011`, `Tile 1012`
Notes: ______

### 7.2 Ensure the Filter combo has no tile entries with only tile 1001
[ ] PASS  [ ] FAIL
1. In a project with only tile 1001 open the `Filter` combo.
   - [ ] There are no `Tile ...` entries
Notes: ______

### 7.3 Ensure the tile filter hides layers that do not cover the tile
[ ] PASS  [ ] FAIL
1. In the 4-tile project choose `Tile 1012`.
   - [ ] The layer list hides `L1001` (does not cover 1012)
   - [ ] The layer list shows `L_all` and `L1012`
Notes: ______

### 7.4 Ensure the tile filter keeps every object visible
[ ] PASS  [ ] FAIL
1. Continue from 7.3. Look at the viewport.
   - [ ] All objects stay visible (unlike the object filter, which hides other objects)
Notes: ______

### 7.5 Ensure a new layer under a tile filter gets that tile
[ ] PASS  [ ] FAIL
1. With the filter on `Tile 1012`, click **New > Paint Layer**.
   - [ ] Its Tiles button reads `1012`
   - [ ] Its Objects button reads `Objects: All`
Notes: ______

### 7.6 Ensure choosing an object clears the tile filter
[ ] PASS  [ ] FAIL
1. Choose `Tile 1011`, then choose an object (for example `Head`).
   - [ ] Only the object filter is active (the tile filter is cleared)
Notes: ______

### 7.7 Ensure choosing a tile clears the object filter
[ ] PASS  [ ] FAIL
1. Choose `Head`, then choose `Tile 1011`.
   - [ ] Only the tile filter is active (the object filter is cleared)
Notes: ______

### 7.8 Ensure All shows every layer
[ ] PASS  [ ] FAIL
1. Choose `All`.
   - [ ] Every layer is shown
Notes: ______

### 7.9 Ensure a stale tile filter falls back to All
[ ] PASS  [ ] FAIL
1. Set the filter to `Tile 1011`. Delete `Head` and remove tile 1011 (3.4 to 3.6).
   - [ ] The filter falls back to `All` the next time the combo draws (the stale tile filter resets)
   - [ ] There is no crash
Notes: ______

## 8. Viewport vs path trace vs export agreement

Setup: `udim_2x2_multi.obj` fresh, three layers: `Base` (Fill, grey, all), `T1002` (Fill red, Tiles `1002`), `ObjHead` (Fill blue, Objects `Head`), plus `Mix` (Fill yellow, Objects `Body`, Tiles `1001`). Save the project as `agree.arm`. Fix the camera so every object is visible and take a screenshot (S1).

### 8.1 Ensure the viewport shows the expected coverage
[ ] PASS  [ ] FAIL
1. Look at S1.
   - [ ] `Body` shows red on its 1002 faces and yellow on its 1001 faces
   - [ ] `Head` is blue
   - [ ] Other objects are grey (Base only)
   - [ ] `Mix` is not on `Prop` (even though `Prop` is on 1001)
Notes: ______

### 8.2 Ensure the path traced view agrees with the viewport
[ ] PASS  [ ] FAIL
1. Viewport Mode > `Path Traced`, same camera, wait for convergence, screenshot (S2).
   - [ ] S2 shows the same coverage as S1 (same objects red/yellow/blue/grey, lighting aside)
   - [ ] Where the footprint includes dilate padding, paint reaches a few pixels beyond a UV island, never visibly onto a different object
Notes: ______

### 8.3 Ensure Per Tile export agrees with the viewport
[ ] PASS  [ ] FAIL
1. File > Export > Textures..., **Layers** `Per Tile`, folder `tile_out`, click Export. Open `agree_base_1001.png`, `agree_base_1002.png`, `agree_base_1011.png`, `agree_base_1012.png`.
   - [ ] `_1002` contains red in the 1002 island area only
   - [ ] `_1001` has yellow in the `Body` faces on 1001 and none over the `Prop` island
   - [ ] `_1011` contains blue in the `Head` island
   - [ ] `_1012` has only grey in its islands
   - [ ] Each file is 2048 x 2048
Notes: ______

### 8.4 Ensure the Visible export matches the 2D view
[ ] PASS  [ ] FAIL
1. Export `Visible`. Import the result back as a texture on a fill layer (or open the image) and compare with the 2D view.
   - [ ] The exported atlas `agree_base.png` is 4096 x 4096
   - [ ] It matches the 2D view of the flattened layers (2x2 slots in the same positions)
Notes: ______

### 8.5 Ensure Per Object export names and sizes
[ ] PASS  [ ] FAIL
1. Set `ObjHead` to `Objects: Head` and `Mix` to `Objects: Body`. Export `Per Object`.
   - [ ] Files `agree_Head_*.png` and `agree_Body_*.png` are written (objects covered by an object-restricted layer)
   - [ ] Each is a full atlas (4096 x 4096)
Notes: ______

### 8.6 Ensure the Body Per Object file holds the right layers
[ ] PASS  [ ] FAIL
1. Open the `agree_Body_*.png` files.
   - [ ] They contain the yellow and red layers in the Body islands, plus `Base`
   - [ ] There is no blue
Notes: ______

### 8.7 Ensure the Head Per Object file holds the right layers
[ ] PASS  [ ] FAIL
1. Open the `agree_Head_*.png` files.
   - [ ] They contain `Base` and the blue layer in the Head island
Notes: ______

### 8.8 Ensure path tracing with object-restricted layers does not stall
[ ] PASS  [ ] FAIL
1. In path traced mode, add and toggle a layer set `Objects: Head` several times.
   - [ ] The view refreshes without a long stall or a crash (a footprint mask is built per object-restricted layer)
   - [ ] Rough stall times recorded: ______
Notes: ______

### 8.9 Ensure an object-restricted layer works in a non-UDIM project
[ ] PASS  [ ] FAIL
1. Import `three_cubes_separate_uv.obj`. Add layer `{B}` painted over everything. Export `Per Object`, then path trace.
   - [ ] B's file/texture holds the layer only inside B's UV quadrant (footprint plus padding)
   - [ ] The A and C quadrants hold only unrestricted layers
   - [ ] The path trace shows the layer on B only
Notes: ______

## 9. Overlap tooltip

### 9.1 Ensure the Objects tooltip names an overlapping object
[ ] PASS  [ ] FAIL
1. Import `three_cubes_overlapping_uv.obj` (fresh, Auto). In Layer 1 open the Objects popup, untick `All`, tick `A`. Hover the Objects button.
   - [ ] The first tooltip line reads `Limit this layer to the selected objects.`
   - [ ] A second line names B or C, for example `A shares UV space with B on tile 1001. In exports and path tracing this layer also applies to B.` (the named object is the first uncovered object that overlaps A on tile 1001)
Notes: ______

### 9.2 Ensure Objects All shows no overlap sentence
[ ] PASS  [ ] FAIL
1. In the same project set the layer to `Objects: All`. Hover the Objects button.
   - [ ] Only the first line of the tooltip shows
   - [ ] There is no overlap sentence
Notes: ______

### 9.3 Ensure disjoint UVs show no overlap sentence
[ ] PASS  [ ] FAIL
1. Import `three_cubes_separate_uv.obj`, set a layer to `{A}`. Hover the Objects button.
   - [ ] Only the first line shows (disjoint quadrants)
   - [ ] There is no overlap sentence
Notes: ______

### 9.4 Ensure Arm shows no overlap sentence
[ ] PASS  [ ] FAIL
1. Import `udim_2x2_multi.obj`, set a layer to `Objects: Arm`. Hover the Objects button.
   - [ ] No sentence shows (`Arm` and `Leg` are on different halves of tile 1012)
Notes: ______

### 9.5 Ensure Body shows no overlap sentence
[ ] PASS  [ ] FAIL
1. Set a layer to `Objects: Body`. Hover the Objects button.
   - [ ] No sentence shows (`Body` and `Prop` share tile 1001 but different halves, v bottom/top)
   - [ ] Edge contact does not count
Notes: ______

### 9.6 Check the documented overlap limit in exports
[ ] PASS  [ ] FAIL
Expected limit, not a failure.
1. With the overlapping cubes, restrict a layer to `A` and paint a stroke on A.
2. Export `Per Object`.
   - [ ] The exported A set holds the stroke
   - [ ] The same texels are used by B and C because they share UV space (paint also appears on B and C in exports and path tracing, but only on A in the raster viewport)
Notes: ______

## 10. Export modes

Setup: `udim_2x2_multi.obj` fresh import, project saved as `ex.arm`, preset `generic` (channels `base`, `nor`, `occ`, `rough`, `metal`). Layers: `L1` (fill green, All), `L2` (stroke, red, Tiles `1002`), `L3` (stroke, blue, Objects `Head`). `<f>` below is the project name `ex`.

### 10.1 Ensure the Layers combo lists all modes in a multi-tile project
[ ] PASS  [ ] FAIL
1. File > Export > Textures... and open the **Layers** combo.
   - [ ] It lists `Visible`, `Selected`, `Per Object`, `Per Tile`, `Per Object & Tile`
Notes: ______

### 10.2 Ensure the Layers combo starts on Per Tile
[ ] PASS  [ ] FAIL
1. File > Export > Textures... (more than one tile).
   - [ ] The **Layers** combo starts on `Per Tile`
Notes: ______

### 10.3 Ensure the resolution label reads Resolution in the Export box
[ ] PASS  [ ] FAIL
1. File > Export > Textures... in the 4-tile project.
   - [ ] The resolution combo reads `Resolution`
Notes: ______

### 10.4 Ensure Per Tile writes one file per tile and channel
[ ] PASS  [ ] FAIL
1. Set **Layers** `Per Tile`, folder `out_tile`, click Export.
   - [ ] 20 files are written (4 tiles x 5 channels): `ex_base_1001.png`, `ex_base_1002.png`, `ex_base_1011.png`, `ex_base_1012.png`, and the same for `nor`, `occ`, `rough`, `metal`
   - [ ] Every file is 2048 x 2048 (native per-tile size for a 2k project)
   - [ ] There is no `ex_base.png` atlas and no object names
   - [ ] Every used tile is written even if empty
Notes: ______

### 10.5 Ensure Per Tile keeps the red stroke in tile 1002 only
[ ] PASS  [ ] FAIL
1. Open the files of 10.4.
   - [ ] `ex_base_1002.png` contains the red stroke from `L2`
   - [ ] `ex_base_1001.png`, `_1011.png`, `_1012.png` contain no red
Notes: ______

### 10.6 Ensure Per Tile keeps the blue stroke in the Head island only
[ ] PASS  [ ] FAIL
1. Open the files of 10.4.
   - [ ] The blue `L3` stroke appears only in the `Head` island of `_1011`
Notes: ______

### 10.7 Ensure Per Object writes files for the object-restricted layer only
[ ] PASS  [ ] FAIL
1. Set **Layers** `Per Object`, folder `out_obj`, click Export.
   - [ ] Files are written for the objects covered by an object-restricted layer only: `ex_Head_base.png`, `ex_Head_nor.png`, ... (5 files)
   - [ ] Each is 4096 x 4096 (atlas)
   - [ ] No file has a tile suffix
Notes: ______

### 10.8 Ensure Per Object masks layers to the object footprint
[ ] PASS  [ ] FAIL
1. Open the files of 10.7.
   - [ ] `L1` appears inside `Head`'s footprint only
   - [ ] `L2` (tile-restricted, covers 1002) is absent from Head's set since Head is on 1011
Notes: ______

### 10.9 Ensure Per Object & Tile writes every object and tile pair
[ ] PASS  [ ] FAIL
1. Set **Layers** `Per Object & Tile`, folder `out_both`, click Export.
   - [ ] Names follow `ex_<object>_<ch>_<tile>.png`
   - [ ] With `L1` unrestricted all pairs apply: `ex_Body_base_1001.png`, `ex_Body_base_1002.png`, `ex_Head_base_1011.png`, `ex_Arm_base_1012.png`, `ex_Leg_base_1012.png`, `ex_Prop_base_1001.png` (+ the 4 other channels each = 30 files)
   - [ ] Every file is 2048 x 2048
Notes: ______

### 10.10 Ensure Per Object & Tile content is masked to the footprint
[ ] PASS  [ ] FAIL
1. Open the files of 10.9.
   - [ ] The red stroke is in `ex_Body_base_1002.png` only
   - [ ] Nothing of `Prop` appears in `ex_Body_base_1001.png` (footprint mask of Body)
Notes: ______

### 10.11 Ensure pairs without an applying visible layer are skipped
[ ] PASS  [ ] FAIL
1. Hide `L1` (eye off) and export `Per Object & Tile` again.
   - [ ] Only `Head` / `1011` (from `L3`) and `Body` / `1002` (from `L2`) are written
Notes: ______

### 10.12 Ensure Visible writes one atlas set
[ ] PASS  [ ] FAIL
1. Set **Layers** `Visible` and export.
   - [ ] One set is written: `ex_base.png` etc
   - [ ] Each file is 4096 x 4096, the whole atlas (2x2 slots)
   - [ ] There is no suffix
Notes: ______

### 10.13 Ensure Selected writes one atlas set without an object name
[ ] PASS  [ ] FAIL
1. Select one layer, set **Layers** `Selected` and export.
   - [ ] The files are the same as for `Visible` (one atlas set)
   - [ ] No object name is appended
Notes: ______

### 10.14 Ensure the tile crop has no off-by-one
[ ] PASS  [ ] FAIL
1. Open `ex_base_1002.png` and compare with the 2D view slot `1002`.
   - [ ] The image is exactly 2048 x 2048
   - [ ] The content is the same as the slot (no one-pixel shift)
   - [ ] There is no 1 px line of the neighbouring tile at the edges
Notes: ______

### 10.15 Ensure a single-1001 project lists only the plain modes
[ ] PASS  [ ] FAIL
1. In a single-1001 project (default cube or `three_cubes_separate_uv.obj`) open the Export Textures box.
   - [ ] The **Layers** combo lists `Visible`, `Selected`, `Per Object` only
Notes: ______

### 10.16 Ensure a tile mode falls back to Visible in a single-1001 project
[ ] PASS  [ ] FAIL
1. Select a tile mode in a multi-tile project, then open a single-1001 project and the Export Textures box.
   - [ ] The mode falls back to `Visible`
Notes: ______

### 10.17 Ensure a 1002-only project lists all five modes
[ ] PASS  [ ] FAIL
1. Import `udim_single_1002.obj` and open the Export Textures box.
   - [ ] The box lists all five modes
Notes: ______

### 10.18 Ensure a 1002-only project exports Per Tile as 1002 only
[ ] PASS  [ ] FAIL
1. Choose `Per Tile` and export.
   - [ ] Files `<f>_base_1002.png` etc. are written
   - [ ] No `_1001` files are written
Notes: ______

### 10.19 Ensure 16bit exr export carries the tile suffix
[ ] PASS  [ ] FAIL
1. Set Color `16bit` (exr) and export `Per Tile`.
   - [ ] The file names read `ex_base_1001.exr` (tile suffix before the extension)
   - [ ] The sizes are the same as for png
Notes: ______

### 10.20 Ensure jpg export carries the tile suffix
[ ] PASS  [ ] FAIL
1. Set format `jpg` with `8bit` and export `Per Tile`.
   - [ ] The file names read `ex_base_1001.jpg` (tile suffix before the extension)
   - [ ] The sizes are the same as for png
Notes: ______

### 10.21 Ensure the export shortcut starts in Per Tile with several tiles
[ ] PASS  [ ] FAIL
1. Use the export shortcut (File > Export > Textures As... if it exists, or ctrl+shift+e / the keymap's file_export_textures_as) in a project with several tiles.
   - [ ] The output names carry `_1001` etc.
Notes: ______

### 10.22 Ensure Pack into Project works Per Tile
[ ] PASS  [ ] FAIL
1. Set Destination `Pack into Project`, **Layers** `Per Tile`, export.
   - [ ] Textures with tile suffixes are added to the Textures tab (names such as `ex_base_1001`)
   - [ ] There is no crash
Notes: ______

## 11. Bakes (Bake Maps plugin)

Setup: `udim_2x2_multi.obj` fresh import, project saved as `bk.arm`, Resolution `2048` per tile (atlas 4096). Plugins tab, panel `Bake Maps`, button `Bake Mesh Maps...`. Box tabs: `Maps`, `Settings`, `About`. In `Maps` click `Select None`, then tick `Ambient Occlusion`. In `Settings` > `Common` set `Resolution` `2048` (per tile; the combo does not say so). In `Settings` > `Output` set `Name` `bkao` and click the `...` folder button and pick an empty folder `bake_out`. Click `Bake`. The tiles in the project are 1001, 1002, 1011, 1012 (not 1001..1012).

### 11.1 Ensure the AO bake reports success
[ ] PASS  [ ] FAIL
1. Bake the AO map (GPU default).
   - [ ] The Console shows `Baked 1 maps`
   - [ ] The Console then shows `Baked maps linked in material bkao Baked`
Notes: ______

### 11.2 Ensure the AO bake writes the atlas and tile crops
[ ] PASS  [ ] FAIL
1. Look in `bake_out`.
   - [ ] `bkao_ao.png` (the atlas) is 4096 x 4096
   - [ ] The tile crops `bkao_ao_1001.png`, `bkao_ao_1002.png`, `bkao_ao_1011.png`, `bkao_ao_1012.png` exist, each 2048 x 2048
Notes: ______

### 11.3 Ensure only the atlas is imported into the project
[ ] PASS  [ ] FAIL
1. Open the project Textures tab.
   - [ ] Only the atlas file is imported (`bkao_ao`)
   - [ ] The tile crops are not imported
Notes: ______

### 11.4 Ensure the tile crops match the atlas slots
[ ] PASS  [ ] FAIL
1. Open the 4 tile crops and the atlas.
   - [ ] Each crop equals the matching slot of the atlas (`1001` top left, `1002` top right, `1011` bottom left, `1012` bottom right)
   - [ ] AO shading shows on the islands of the objects of that tile
Notes: ______

### 11.5 Ensure the baked material uses the Atlas UV option
[ ] PASS  [ ] FAIL
1. Materials tab: select material `bkao Baked`. Open the node editor (the `Nodes` view) and select the image texture node.
   - [ ] An image node is linked to Occlusion
   - [ ] The button `UV` reads `Atlas` (the two options of the button read `Default` and `Atlas`)
Notes: ______

### 11.6 Ensure Add Baked Layer shows the bake correctly
[ ] PASS  [ ] FAIL
1. Click `Add Baked Layer` in the Plugins panel.
   - [ ] A fill layer with the `bkao Baked` material is added
   - [ ] The baked AO displays correctly on all objects of all tiles
   - [ ] There are no repeats and no tile shows the whole atlas squeezed in
Notes: ______

### 11.7 Ensure UV Default no longer fits the map
[ ] PASS  [ ] FAIL
1. In the image node switch `UV` from `Atlas` to `Default`.
   - [ ] The map no longer fits: the texture repeats per tile (each tile shows the whole atlas shrunk or a seam of 2x2 repeats)
   - [ ] Old canvases with only two buttons behave as Default
Notes: ______

### 11.8 Ensure UV Atlas brings the correct map back
[ ] PASS  [ ] FAIL
1. Switch `UV` back to `Atlas`.
   - [ ] The correct map returns
Notes: ______

### 11.9 Ensure the plugin View buttons show the bake on all tiles
[ ] PASS  [ ] FAIL
1. Click the `View` buttons `AO` / `Normal` / `Cavity` of the plugin panel.
   - [ ] The viewport shows the baked AO on all tiles correctly
   - [ ] There are no seams or empty tiles
Notes: ______

### 11.10 Ensure Inpaint reports the repair
[ ] PASS  [ ] FAIL
1. Note the modification times of `bkao_ao_1001.png` etc (`ls -l@ bake_out`).
2. Pick the plugin tool `Inpaint` (Tools list), mode Copy, paint a short stroke on the island of `Head` (tile 1011) and release the mouse.
   - [ ] The Console shows `Inpaint: 1 maps repaired`
Notes: ______

### 11.11 Ensure Inpaint rewrites the atlas and tile crops
[ ] PASS  [ ] FAIL
1. Continue from 11.10. Check `ls -l@ bake_out`.
   - [ ] `bkao_ao.png` is rewritten
   - [ ] The tile crops of the tiles that exist have newer timestamps, in particular `bkao_ao_1011.png`
Notes: ______

### 11.12 Ensure the repaired tile crop equals the new atlas slot
[ ] PASS  [ ] FAIL
1. Open `bkao_ao_1011.png` and the new atlas.
   - [ ] The content of `bkao_ao_1011.png` equals slot 1011 of the new atlas
   - [ ] The repaired area is visible
Notes: ______

### 11.13 Ensure Inpaint leaves untouched tiles alone
[ ] PASS  [ ] FAIL
1. Compare the crops of the other tiles with their earlier versions.
   - [ ] Tiles that were not touched keep the same pixels
Notes: ______

### 11.14 Ensure a 4096 per tile bake gives an 8192 atlas
[ ] PASS  [ ] FAIL
1. Bake at `Resolution` 4096 per tile on the 4 tiles.
   - [ ] The atlas is 8192 x 8192
   - [ ] The tile crops are 4096 x 4096
   - [ ] The names `bkao_ao_...` are unchanged
Notes: ______

### 11.15 Ensure a 1024 per tile bake gives a 2048 atlas
[ ] PASS  [ ] FAIL
1. Bake at `Resolution` 1024.
   - [ ] The atlas is 2048 x 2048
   - [ ] The tile crops are 1024 x 1024
   - [ ] The names `bkao_ao_...` are unchanged
Notes: ______

### 11.16 Ensure the bake size follows the script Resolution
[ ] PASS  [ ] FAIL
1. Set the project Edit menu resolution to `4096`, run the bake again at script Resolution `2048`.
   - [ ] The bake size follows the script Resolution (per tile 2048), not the project resolution
   - [ ] Sizes recorded: ______
Notes: ______

### 11.17 Ensure the CPU bake warns about a 16k atlas
[ ] PASS  [ ] FAIL
1. Import `udim_12_tiles.obj`, Edit menu Resolution `2048`.
2. In the plugin, Settings > `Raytracing` `Bake on` `CPU`, `Resolution` `4096` (12 tiles, atlas 4096 x 4 = 16384), click `Bake`. Press `Stop` after the message appears (the CPU bake needs about 2 GB).
   - [ ] The Console shows `CPU bake of a 16384 px atlas needs a lot of memory, consider the GPU bake or a lower resolution` before the bake starts
Notes: ______

### 11.18 Ensure the CPU bake gives no warning at an 8192 atlas
[ ] PASS  [ ] FAIL
1. In the same project set `Resolution` `2048` (12 tiles = 8192 atlas), `Bake on` `CPU`, click `Bake`, then `Stop`.
   - [ ] No memory warning is shown
Notes: ______

### 11.19 Ensure a 1002-only project bakes a single tile copy
[ ] PASS  [ ] FAIL
1. Import `udim_single_1002.obj` and bake AO.
   - [ ] `bkao_ao.png` (the atlas) is written
   - [ ] A `bkao_ao_1002.png` copy of the same size is written (single tile)
   - [ ] No `_1001` file is written
Notes: ______

### 11.20 Ensure a plain single-tile project bakes as upstream
[ ] PASS  [ ] FAIL
1. In the default cube project bake AO.
   - [ ] Only `bkao_ao.png` is written, no tile files
   - [ ] The Baked material's image node still works
Notes: ______

### 11.21 Ensure a bake after relayout uses the new layout
[ ] PASS  [ ] FAIL
1. In the 4-tile project append `udim_12_tiles.obj` while the bake window is closed, then bake again.
   - [ ] The bake uses the new layout (12 tiles, stride 4)
   - [ ] Per tile files are written for every used tile (`_1001`..`_1006`, `_1011`..`_1016`)
   - [ ] Tile files of removed or no longer used tiles are not deleted
Notes: ______

## 12. Save and reopen

### 12.1 Ensure the tile list and resolution survive save and reopen
[ ] PASS  [ ] FAIL
1. Import `udim_2x2_multi.obj` (fresh). Paint strokes on several tiles.
2. Set layers: `L2` Tiles `1002` + Objects `Body`, `L3` Tiles `1011, 1012`, a group with `Objects: Head`.
3. Set resolution `4096` per tile (atlas 8192). Note a screenshot and the 2D view.
4. File > Save As... `rt.arm`, close the app, reopen it, File > Open... `rt.arm`.
   - [ ] The `Tiles` panel lists `1001`, `1002`, `1011`, `1012`
   - [ ] The Edit menu shows `4096` with `4 tiles, atlas 8192 px, about ...`
Notes: ______

### 12.2 Ensure coverage buttons survive save and reopen
[ ] PASS  [ ] FAIL
1. Continue from 12.1. Look at every layer and the group.
   - [ ] The Tiles and Objects buttons are the same on every layer and the group
Notes: ______

### 12.3 Ensure the viewport and 2D view are unchanged after reopen
[ ] PASS  [ ] FAIL
1. Continue from 12.1. Compare the viewport and the 2D view with the earlier screenshots.
   - [ ] The viewport is pixel-identical to before saving
   - [ ] The 2D view is pixel-identical to before saving
Notes: ______

### 12.4 Ensure row tooltips survive save and reopen
[ ] PASS  [ ] FAIL
1. Continue from 12.1. Hover the Meshes rows.
   - [ ] The Meshes row tooltips still read the same `Tiles: ...`
Notes: ______

### 12.5 Ensure a second save and reopen works
[ ] PASS  [ ] FAIL
1. Continue from 12.1. Save again as `rt2.arm`, close and reopen it.
   - [ ] It loads without errors
   - [ ] Everything is still the same (save -> reopen -> save works repeatedly)
Notes: ______

### 12.6 Ensure a removed tile stays removed after save and reopen
[ ] PASS  [ ] FAIL
1. Remove a tile first (3.4 to 3.6), save, reopen.
   - [ ] The removed tile is gone
   - [ ] The other tiles' paint is intact
   - [ ] The 2D view slots match
Notes: ______

### 12.7 Ensure a linked duplicate of Arm survives save and reopen
[ ] PASS  [ ] FAIL
1. In the Meshes tab right-click `Arm` > Duplicate. Paint on the copy, save, reopen.
   - [ ] The duplicate's tooltip shows `Tiles: 1012`
   - [ ] It paints on the right tile
   - [ ] It has the same layout as before saving
Notes: ______

### 12.8 Ensure a duplicate of Body survives save and reopen
[ ] PASS  [ ] FAIL
1. Duplicate `Body` (two tiles), save, reopen.
   - [ ] The duplicate shows both tiles (`Tiles: 1001-1002`) and paints on the right tiles
Notes: ______

### 12.9 Ensure a mesh-only .arm import into a tile project goes to 1001
[ ] PASS  [ ] FAIL
1. File > Import > Mesh... on a `.arm` saved from `rt.arm` (mesh-only) into a project with tiles.
   - [ ] The imported meshes go to tile 1001 (no tile data)
   - [ ] There is no crash
   - [ ] Existing paint is untouched
Notes: ______

### 12.10 Ensure a mesh-only .arm import into a plain project works
[ ] PASS  [ ] FAIL
1. File > Import > Mesh... the same `.arm` into a plain project.
   - [ ] The imported meshes go to tile 1001
   - [ ] There is no crash
Notes: ______

### 12.11 Ensure a 16bit tile project survives save and reopen
[ ] PASS  [ ] FAIL
1. Save a project with tiles and 16bit colour, reopen.
   - [ ] Same result as 12.1 to 12.4
Notes: ______

### 12.12 Ensure a plain project has no tile UI after reopen
[ ] PASS  [ ] FAIL
1. Open `plain.arm` from section 1.
   - [ ] The plain project has no tile UI
Notes: ______

### 12.13 Ensure a 1002-only project keeps its tile after reopen
[ ] PASS  [ ] FAIL
1. Save a `udim_single_1002` project, reopen it.
   - [ ] The `Tiles` panel still shows `1002`
   - [ ] Export writes `_1002` files
Notes: ______

### 12.14 Ensure a phase A project opens
[ ] PASS  [ ] FAIL
1. Open a saved format "99" file from before (for example `p99.arm` from the phase A plan).
   - [ ] It opens without error
   - [ ] Coverage buttons are unchanged
   - [ ] The tile list is `{1001}` and there is no Tiles button
Notes: ______

### 12.15 Ensure sculpt bumps survive save and reopen in a multi-tile project (review fix)
[ ] PASS  [ ] FAIL
1. Import `udim_2x2_multi.obj` (fresh, 2048 per tile, atlas 4096). Layers tab: add a sculpt layer, sculpt a visible bump on `Body` in tile 1001 and on `Head`.
2. Save `sc.arm`, close, reopen.
   - [ ] Both bumps are back exactly as sculpted
   - [ ] There are no scrambled spikes or flattened areas
Notes: ______

### 12.16 Ensure sculpting after reopen works without jumps
[ ] PASS  [ ] FAIL
1. Continue from 12.15. Sculpt again.
   - [ ] Sculpting works without jumps
Notes: ______

### 12.17 Ensure painting right after a mesh-only .arm import works (review fix)
[ ] PASS  [ ] FAIL
1. With `udim_2x2_multi.obj` open, File > Import > Mesh... a mesh-only `.arm` (exported earlier with File > Export > Mesh, format `.arm`, of the default cube).
2. Without switching layers, paint a stroke on the imported cube and on `Head`.
   - [ ] The cube lands in tile 1001 (2D view slot 1001)
   - [ ] Strokes appear under the cursor on both objects, never in another slot
Notes: ______

### 12.18 Ensure the footprint of a vertex-colour mesh is right in the path tracer (review fix)
[ ] PASS  [ ] FAIL
1. Import an OBJ that has vertex colours (or a mesh with a second UV map) with `UDIM tiles` `Off`. Restrict a layer to one of its objects with the Objects button.
2. Set viewport mode `Path Traced`.
   - [ ] The layer shows only on that object in the path traced view
   - [ ] The footprint matches the raster viewport (no garbage shapes, no missing areas)
Notes: ______

### 12.19 Ensure the footprint of a vertex-colour mesh is right in the export (review fix)
[ ] PASS  [ ] FAIL
1. Continue from 12.18. Export `Visible`.
   - [ ] The layer shows only on that object in the exported files
   - [ ] The footprint matches the raster viewport (no garbage shapes, no missing areas)
Notes: ______

## 13. B7 migration of the old name-based UDIM project

Uses `old_udim.arm` from 0.4 (`Body.1001` / `Body.1002`, project resolution R, layer `Shared` crossing both tiles, layer `OnlyB2` restricted to `Body.1002`).

### 13.1 Ensure the old project opens and keeps object names
[ ] PASS  [ ] FAIL
1. File > Open... `old_udim.arm` in the new build.
   - [ ] It opens without a crash or errors
   - [ ] The Meshes tab objects keep their names `Body.1001` and `Body.1002`
Notes: ______

### 13.2 Ensure the migrated project has two tiles with half resolution
[ ] PASS  [ ] FAIL
1. Continue from 13.1. Look at the `Tiles` panel and the Edit menu.
   - [ ] The `Tiles` panel lists `1001` and `1002` (2 tiles)
   - [ ] The per-tile resolution reads `R/2 x R/2` (for R = 2048: `1024 x 1024`)
   - [ ] The Edit menu line reads `2 tiles, atlas <R> px, ...` (atlas = old resolution)
Notes: ______

### 13.3 Ensure the viewport is unchanged after migration
[ ] PASS  [ ] FAIL
1. Compare the viewport with the upstream screenshot (same camera).
   - [ ] The `Shared` stroke across both cubes is where it was
   - [ ] The `OnlyB2` stroke is only on the right cube (`Body.1002`)
Notes: ______

### 13.4 Ensure the Shared layer keeps its atlas positions
[ ] PASS  [ ] FAIL
1. Open the 2D view for `Shared`.
   - [ ] `1001` is in the first slot at the top left and `1002` to its right, same pixels as before
   - [ ] The grid labels read `1001`, `1002`
Notes: ______

### 13.5 Ensure the OnlyB2 layer sits in its slot
[ ] PASS  [ ] FAIL
1. Open the 2D view for `OnlyB2`.
   - [ ] Its stroke is in slot `1002`, the rest of the atlas is empty (its pixels are drawn into the slot, they were tile-local upstream)
Notes: ______

### 13.6 Ensure the migrated layers show the right coverage buttons
[ ] PASS  [ ] FAIL
1. Look at the Layers tab rows.
   - [ ] `OnlyB2`'s Objects button reads `Body.1002` and its Tiles button `Tiles: All`
   - [ ] `Shared` reads `Objects: All`, `Tiles: All`
Notes: ______

### 13.7 Ensure painting on OnlyB2 still paints only the right cube
[ ] PASS  [ ] FAIL
1. Paint a new stroke on `OnlyB2`.
   - [ ] It paints only the right cube
Notes: ______

### 13.8 Ensure the mask of OnlyB2 is migrated
[ ] PASS  [ ] FAIL
Optional: only if you added a mask to `OnlyB2` upstream.
1. Look at the mask in the 2D view.
   - [ ] The mask is drawn into its slot and white outside it
   - [ ] The layer shows as before
Notes: ______

### 13.9 Ensure Meshes row tooltips show the migrated tiles
[ ] PASS  [ ] FAIL
1. Hover the Meshes rows.
   - [ ] `Body.1001` reads `Tiles: 1001`
   - [ ] `Body.1002` reads `Tiles: 1002`
Notes: ______

### 13.10 Ensure painting on Shared after migration behaves normally
[ ] PASS  [ ] FAIL
1. Paint new strokes on `Shared` across both cubes.
   - [ ] Normal behaviour as in section 6
Notes: ______

### 13.11 Ensure a tile-restricted fill after migration appears on the right cube only
[ ] PASS  [ ] FAIL
1. Click **New > Fill Layer** with `Tiles: 1002`.
   - [ ] The fill appears only on the right cube
Notes: ______

### 13.12 Ensure the migrated project saves as a normal phase B project
[ ] PASS  [ ] FAIL
1. File > Save As... `migrated.arm`, reopen.
   - [ ] It loads as a normal phase B project
   - [ ] All values are the same as in 13.1 to 13.7
Notes: ______

### 13.13 Ensure there is no name-based handling after migration
[ ] PASS  [ ] FAIL
1. In `migrated.arm` rename `Body.1002` to `Foo`.
   - [ ] The paint and Tiles stay as they were
Notes: ______

### 13.14 Ensure Per Tile export of the migrated project works
[ ] PASS  [ ] FAIL
1. Export `Per Tile` of the migrated project.
   - [ ] Files `<f>_base_1001.png` and `<f>_base_1002.png` are written at `R/2` px (1024 x 1024 for R = 2048)
   - [ ] `OnlyB2`'s content is only in the `_1002` file
Notes: ______

### 13.15 Ensure an old plain v23 project converts
[ ] PASS  [ ] FAIL
Optional: only if you have an old plain v23 project (no UDIM).
1. Open it.
   - [ ] It is converted as in the phase A plan (section 13)
   - [ ] The tile list is `{1001}`
Notes: ______

## 14. Undo / redo across tile edits

Setup: `udim_2x2_multi.obj`.

### 14.1 Ensure undo reverts a tile coverage change
[ ] PASS  [ ] FAIL
1. Open the Layer 1 Tiles popup: untick `All`, tick `1002`. Press ctrl+z.
   - [ ] The Edit menu shows `Undo Layer Object` (the same history entry is used for tile and object coverage)
   - [ ] The button returns to `Tiles: All`
   - [ ] The paint reappears on every tile
Notes: ______

### 14.2 Ensure redo restores a tile coverage change
[ ] PASS  [ ] FAIL
1. Continue from 14.1. Press ctrl+shift+z.
   - [ ] The button is back to `1002`
Notes: ______

### 14.3 Ensure each tick in the layer Tiles popup is one undo step
[ ] PASS  [ ] FAIL
1. Tick and untick several tiles in a layer's Tiles popup, then undo step by step.
   - [ ] Each tick or untick is one undo step
   - [ ] The exact sets are restored
Notes: ______

### 14.4 Ensure each tick in a group's Tiles popup is one undo step
[ ] PASS  [ ] FAIL
1. Tick and untick several tiles in a group's Tiles popup, then undo step by step.
   - [ ] Each tick or untick is one undo step
   - [ ] The exact sets are restored
Notes: ______

### 14.5 Ensure strokes and coverage changes interleave in undo
[ ] PASS  [ ] FAIL
1. Paint strokes before and after a coverage change, then undo and redo through them.
   - [ ] Undo/redo of strokes and coverage interleave correctly
   - [ ] Pixels return exactly
Notes: ______

### 14.6 Ensure a layout change clears the undo history
[ ] PASS  [ ] FAIL
1. Paint several strokes (history has steps). `Remove` an unused tile or `Append` a mesh with a new tile.
   - [ ] The Console shows `Tile layout changed, undo history was cleared`
   - [ ] The Edit menu's Undo and Redo entries are empty
Notes: ______

### 14.7 Ensure new strokes undo normally after a relayout
[ ] PASS  [ ] FAIL
1. Continue from 14.6. Paint a stroke and press ctrl+z.
   - [ ] The new stroke undoes normally
Notes: ______

### 14.8 Ensure redo is not possible after a relayout
[ ] PASS  [ ] FAIL
1. Continue from 14.6. Open the Edit menu.
   - [ ] The redo list is empty
Notes: ______

### 14.9 Ensure changing the resolution keeps the paint
[ ] PASS  [ ] FAIL
1. Edit menu > Resolution `4096` then `2048` back.
   - [ ] There is no crash
   - [ ] The paint is kept (scaled uniformly, tiles keep their slots)
   - [ ] Undo history behaves as upstream does for resolution changes
Notes: ______

### 14.10 Ensure deleting an object leaves its layers covering nothing
[ ] PASS  [ ] FAIL
Undo of deleting an object is not available (deferred).
1. Delete an object that a layer covers.
   - [ ] There is no crash
   - [ ] Layers that covered it cover nothing (phase A behaviour)
Notes: ______

## 15. Mesh export

### 15.1 Ensure Export Mesh writes the OBJ file
[ ] PASS  [ ] FAIL
1. Import `udim_2x2_multi.obj` (fresh). File > Export > Mesh..., tab `Export Mesh`: Format `OBJ`, Meshes `All` (the first entry), Export to `mesh_out/mesh.obj`.
   - [ ] The file is written
Notes: ______

### 15.2 Ensure the exported vt lines hold UDIM UVs
[ ] PASS  [ ] FAIL
1. Open `mesh_out/mesh.obj` and look at the `vt` lines.
   - [ ] The `vt` lines hold UDIM UVs with u in 0..2 and v in 0..2 (for example `Head` vt with v between 1.02 and 1.98)
   - [ ] They are not atlas UVs squeezed into 0..1
Notes: ______

### 15.3 Ensure the exported file has the same tiles per object
[ ] PASS  [ ] FAIL
1. In a terminal run `python3 /Users/martin.crawford/Development/Workspaces/Game/armorpaint/docs/udim/testdata/check_tiles.py <path>/mesh.obj`.
   - [ ] Tiles are `[1001, 1002]` for `Body`, `[1011]` for `Head`, `[1012]` for `Arm` and `Leg`, `[1001]` for `Prop`
   - [ ] `straddling=0` for all
Notes: ______

### 15.4 Ensure the exported file reimports with the same tiles
[ ] PASS  [ ] FAIL
1. Import the exported file (fresh, **UDIM tiles** `Auto`).
   - [ ] The tile list is `1001`, `1002`, `1011`, `1012`
   - [ ] The Meshes tooltips are the same as in section 3 (3.1)
Notes: ______

### 15.5 Ensure a single object exports with UDIM UVs
[ ] PASS  [ ] FAIL
1. Export a single object (Meshes combo, pick `Head`).
   - [ ] Only `Head` is written
   - [ ] The `vt` v values are in 1..2
Notes: ______

### 15.6 Ensure GLB export writes UDIM UVs
[ ] PASS  [ ] FAIL
Only if the GLB format exists.
1. Export `Head` as GLB.
   - [ ] The UVs in the GLB also are UDIM UVs
Notes: ______

### 15.7 Ensure a plain single-tile mesh exports as upstream
[ ] PASS  [ ] FAIL
1. Export a plain single-tile mesh.
   - [ ] The output is unchanged from upstream (UVs 0..1)
   - [ ] Nothing extra is written
Notes: ______

## 16. Known limits (expected, not failures)

Record any difference you see as a note, these are decided or deferred.

- Overlapping UVs of different objects inside one tile: a texture cannot tell them apart, exports and path tracing can differ from the viewport. Only the Objects button tooltip warns (section 9). Needs per-object texture sets (`DEFERRED.md`).
- Translucency with the path tracer (D5): with translucency on and a bottom layer restricted by object or tile, texels outside its coverage flatten to alpha 0, so path tracing and the exported opacity map show them transparent while the raster viewport shows them opaque. Left as on main (`DEFERRED.md`).
- glTF and Blend import stay on the Off (wrap) behaviour, no **UDIM tiles** option for them.
- UDIM tiles Off wraps each UV corner on its own, as upstream: tiling UVs that cross whole numbers are distorted or collapse; a tiling texture does not repeat (for example `udim_tiling_uv.obj`, whose corners sit on 0, 2, 4, collapses and strokes show no repeating pattern).
- Mesh nodes (`mesh_nodes_objects`) are positional: deleting or reordering objects can re-point a canvas.
- Per Object semantics differ from the design table (DECISIONS B5): Per Object iterates the objects covered by an OBJECT-RESTRICTED layer, not every object covered by any layer, so a non-UDIM project with only unrestricted layers exports once without a suffix. Unrestricted layers in each Per Object file are masked to that object's footprint.
- Raytraced lightmap bake squeezes the whole atlas into the script's resolution.
- The Bake Texture node repeats its atlas target per tile. (Fixed on branch `feat/udim-bake-node-per-tile`, see section 17.)
- The Bake Maps script `Resolution` combo does not say `(per tile)` (the script API has no UDIM query), it is per tile.
- CPU bake memory: a 16k atlas needs about 2 GB; only a warning above an 8192 atlas, per-tile CPU bake is deferred.
- Tile files of removed tiles are not deleted from the bake folder.
- The atlas clamp is not remembered: a project saved while clamped (for example 9 tiles at 8192 -> 5461 per tile) reopens with 5461 as its configured per-tile resolution; removing tiles afterwards keeps 5461. A sculpt layer saved while the atlas was clamped may not load correctly.
- An image node with `UV` `Atlas` used in a fill layer follows that layer's UV scale and angle (scale 1 and angle 0 show the baked map correctly).
- Tools other than the UV editor that change UVs in place (if any) may leave stale tile-local UVs for save; report any UV change that reverts after save and reopen.
- Blender FBX files are in cm with a node scale of 100, so an FBX added with `Append` comes in 100 times larger than meshes already in the project (a fresh import rescales to fit). Same as upstream.
- Crop is blocked with more than one tile.
- Beyond about 64 tiles at 2k or 16 at 4k the single atlas stops being practical (tile streaming deferred); the atlas side is capped at 16384.
- Mesh delete has no undo; "Join UDIM tile objects" is not built (use Merge Geometry); timeline keyframes of migrated tile-local layers and group masks over them are not migrated.
- Material and brush previews and the material bake export paint every tile (no tile discard) and do not repeat materials per tile.
- Hidden objects still count for footprint masks (the texture does not depend on visibility).
- Object names in export file names are not sanitized (a `/` or other odd characters in an object name can break the name).
- The `Per Tile` export writes every used tile even if empty.

## Watch for (riskiest spots)

- Relayout (append, Remove tile, 4.10 to 4.12, 4.17 to 4.20, 3.6, 14.6, Reimport 2.16 to 2.18): undo textures of other layers, layer masks (must come back white in new slots), sculpt textures, layer previews, 16 and 32 bit projects, fill layers re-parsing, the merged mesh and UV caches (stale geometry after relayout), and the undo history being really empty.
- Atlas UV border texels: paint near the edge of a tile slot in the 2D view must not appear in the neighbouring slot, and a vertex on a tile border must not resolve to the wrong slot (6.20, 2.15).
- Footprint mask cost in path trace mode: an R8 scratch target (256 MB at a 16k atlas) is held while an object-restricted layer is flattened; check stalls and memory in 8.8.
- B7 tile-local layers and masks (section 13): single-tile layers must land in the correct slot, their masks white outside it, layers with more than one tile object must stay atlas-wide. Also files with more than 64 tile ids (console warning).
- Save round trip of mesh tiles (12.7 to 12.10): linked duplicates whose tile data differs from their source, duplicates with two tiles, mesh-only .arm imports, re-save after load (12.5).
- FBX import (2.20 to 2.22): object names with suffixes, objects visible at a sensible size, the whole-scene scan for Auto, straddle count, UVs above 1 now wrapping like OBJ.
- Export crop off-by-one (10.14): slot rectangle origin in each tile file, one pixel shifts, atlas sizes that are not a power of two (stride 3, 5461 px per tile).
- Bake tile crops with non-divisible sizes (11.14, 11.15, 4.6, 4.7 in a 9-tile project with the bake): atlas = per tile x stride, 16383 vs 16384, tile files must have the per-tile size and the atlas the image node uses must match.
- Sculpt seam coverage (6.13 to 6.16): any vertex on a covered tile counts, so seam vertices must move together without cracks.
- Appended projects' tiles (4.10 to 4.16, 12.9, 12.10): objects appended without tile data must go to 1001, the existing list must stay sorted and distinct, and layers made before an append must keep their `Tiles` sets.
- Import tile-list rule (2.16 to 2.19): an import that replaces the meshes sets the tile list to exactly the tiles the new meshes use; an append keeps existing tiles and adds new ones.
- Sculpt layers in multi-tile projects across save and reopen (12.15, 12.16), mesh-only .arm import into a UDIM project (12.17), footprints of meshes with vertex colours or a second UV map (12.18, 12.19): all three were fixed after the final code review and never run.
- Mixing coverage: a layer with both Objects and Tiles restrictions, groups with their own sets (D1 intersection) and the greying in both popups.

## 17. Bake Texture node per tile (branch `feat/udim-bake-node-per-tile`)

Added 2026-11-13 for the fix of the B6 open question "the Bake Texture node repeats its atlas target per tile" (DECISIONS.md, last section). Before the fix each tile showed a full shrunken copy of the whole bake; now each tile shows only the bake of its own geometry.

How to build the node graph (used by every step below):
1. Materials tab: click the material thumbnail of the default material (`Material`) to select it, then click `Nodes` (the node editor opens).
2. In the node editor menubar click `Texture`, pick `Bake Texture`. A node `Bake Texture` appears with a `Bake` combo, a `Bake` button (play icon) and a `Color` output.
3. Drag from the node's `Color` output to the `Base Color` input of the `Material Output` node (replace the existing link).
4. In the node's `Bake` combo pick the bake type named by the step, then click the `Bake` button. While it bakes the button is a percentage slider; wait until the button comes back (one frame for non-raytraced types).
5. Layers tab: `New` > `Fill Layer`. The fill layer uses the selected material.
To compare with the old behaviour, run the same steps in the build of `feat/udim-object-tile-selection` (tip `91cd5b54b`).

Setup: `udim_2x2_multi.obj` fresh import (**UDIM tiles** `Auto`), Layers tab Edit menu Resolution `2048` per tile (atlas 4096, tiles 1001, 1002, 1011, 1012).

### 17.1 Ensure a TexCoord bake draws each island once in its own slot
[ ] PASS  [ ] FAIL
1. Build the node graph, `Bake` = `TexCoord`, bake, add the fill layer.
2. Look at the 2D view (layer type) of the fill layer and at the node preview of the `Bake Texture` node (the node's thumbnail, it shows the raw atlas target).
   - [ ] The 2D view of the layer equals the node preview
   - [ ] The UV islands of all objects are drawn once each, in their own tile slot
   - [ ] Red and green change as ONE smooth gradient over the whole 2x2 grid (red along x, green along y), with no jump back to the same colours at each slot border
   - [ ] No slot contains a shrunken copy of the whole 2x2 grid (old build: every slot shows the full grid shrunk into it)
Notes: ______

### 17.2 Ensure objects show their own island colours in the viewport
[ ] PASS  [ ] FAIL
1. Continue from 17.1. Look at the viewport (Lit) front view.
   - [ ] Each object shows the colours of its own islands in 17.1
   - [ ] Objects on different tiles have clearly different colour ranges (`Head` on 1011, `Arm`/`Leg` on 1012 and `Prop` on 1001 do not look alike)
   - [ ] `Body`'s 1001 faces and 1002 faces have different colours
   - [ ] No object shows patches of other objects' islands
Notes: ______

### 17.3 Ensure a Position bake is per object and per tile
[ ] PASS  [ ] FAIL
1. Change `Bake` to `Position`, bake again (the fill layer updates by itself; if it does not, select another layer and back).
   - [ ] Each object is coloured by its own world position (objects further along x look different from each other, `Body` at x=-4.5 differs from `Leg` at x=4.5)
   - [ ] The 2D view slot of each tile holds only the islands of the objects on that tile
   - [ ] No tile repeats the whole atlas
Notes: ______

### 17.4 Ensure a Curvature bake follows the real edges
[ ] PASS  [ ] FAIL
1. Change `Bake` to `Curvature`, bake again.
   - [ ] Edge highlights follow the real edges of each cube on all four tiles
   - [ ] No edge lines cross faces (which is what a repeated atlas looks like)
Notes: ______

### 17.5 Ensure an Occlusion bake sits correctly on all tiles
[ ] PASS  [ ] FAIL
Only if the `Occlusion` entry exists in the `Bake` combo.
1. Set `Bake` = `Occlusion`, `Samples` `64`, bake, wait for 100%.
   - [ ] AO darkening sits on each object where expected on all four tiles
   - [ ] The 2D view slot of each tile holds only its own islands
   - [ ] There are no repeats
Notes: ______

### 17.6 Ensure a bake is not limited to the selected layer's tiles
[ ] PASS  [ ] FAIL
1. Set `Bake` back to `TexCoord`. Select the default paint layer `Layer 1`, set its Tiles button to `1001` only.
2. With `Layer 1` still selected, click `Bake` on the node. Then select the fill layer.
   - [ ] The fill layer (Tiles `All`) shows the bake on all four tiles, as in 17.1
Notes: ______

### 17.7 Ensure a bake does not paint into the selected layer
[ ] PASS  [ ] FAIL
1. Continue from 17.6. Select `Layer 1`.
   - [ ] `Layer 1` keeps its own content (the bake does not paint into it)
2. Paint on `Layer 1`.
   - [ ] Painting stays on 1001 only
Notes: ______

### 17.8 Ensure a tile-restricted fill layer shows its own parts of the bake
[ ] PASS  [ ] FAIL
1. Set the fill layer's Tiles button to `1002, 1012`.
   - [ ] Only `Body`'s 1002 faces and `Arm`/`Leg` show the bake, each with its own part (same colours as in 17.1 on those tiles)
   - [ ] 1001 and 1011 show the layers below
Notes: ______

### 17.9 Ensure the export matches the 2D view slots
[ ] PASS  [ ] FAIL
1. File > Export > Textures..., `Per Tile`, export.
   - [ ] Each `<name>_base_<tile>.png` (2048 x 2048) equals the matching slot of the 2D view of 17.1
   - [ ] Each file holds only that tile's islands (no shrunken full grid)
Notes: ______

### 17.10 Ensure a re-bake after reopen restores the bake
[ ] PASS  [ ] FAIL
1. Save as `bn.arm`, close, reopen.
2. Click `Bake` on the node again (the bake target is not saved) and look at the fill layer.
   - [ ] The result is as in 17.1 after the bake
Notes: ______

### 17.11 Ensure the node is black before re-baking after reopen
[ ] PASS  [ ] FAIL
1. Save as `bn.arm`, close, reopen. Do not click `Bake`. Look at the fill layer.
   - [ ] The node gives black (as before this fix)
Notes: ______

### 17.12 Ensure a 1002-only project bakes once
[ ] PASS  [ ] FAIL
1. Import `udim_single_1002.obj` (fresh). Build the graph with `TexCoord`, bake, add the fill layer.
   - [ ] The bake fills the whole layer once (single slot, atlas = tile)
   - [ ] `Crate` shows its UV gradient with no repeats (same as before the fix)
Notes: ______

### 17.13 Ensure the plain cube TexCoord bake is unchanged
[ ] PASS  [ ] FAIL
1. File > New (default cube). Build the graph with `TexCoord`, bake, add the fill layer.
   - [ ] The result is identical to the old build (the cube's UV gradient once over its layout)
Notes: ______

### 17.14 Ensure the plain cube Curvature bake is unchanged
[ ] PASS  [ ] FAIL
1. Continue from 17.13. Change `Bake` to `Curvature`, bake again.
   - [ ] The result is identical to the old build (curvature on the edges)
Notes: ______

### 17.15 Ensure UV Scale 2 repeats the bake twice on the plain cube
[ ] PASS  [ ] FAIL
1. Right-click the fill layer row and set the `UV Scale` slider to `2`.
   - [ ] The bake repeats twice, as before
Notes: ______

### 17.16 Ensure the material preview shows the wrapped atlas
[ ] PASS  [ ] FAIL
1. In the 4-tile setup with `TexCoord` baked, look at the material thumbnail in the Materials tab.
   - [ ] The thumbnail shows the atlas wrapped on the preview ball (unchanged, previews sample raw UVs)
Notes: ______

### 17.17 Ensure no shader error appears in a multi-tile bake session
[ ] PASS  [ ] FAIL
1. After all steps above in the multi-tile project, open the Console tab.
   - [ ] There is no shader error in the Console after any of the steps above
Notes: ______

### 17.18 Ensure the material bake export shows the atlas once
[ ] PASS  [ ] FAIL
1. In the 4-tile setup with `TexCoord` baked, Materials tab: right-click the material thumbnail, `Bake`, export to an empty folder.
   - [ ] The exported base color map shows the bake atlas once (same picture as the node preview of 17.1)
   - [ ] It does not show one shrunken copy per tile slot
   - [ ] File names and sizes recorded: ______
Notes: ______

Known limit: like `UV: Atlas` image nodes, the Bake Texture node in a fill layer follows that layer's `UV Scale` and angle; with a scale other than 1 or an angle the atlas is scaled/rotated and wraps across tiles (scale 1 and angle 0 are correct). The bake itself also uses the selected layer's (or the header's) UV scale and angle, so bake with scale 1 and angle 0.
