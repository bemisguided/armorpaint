# UDIM phase B: test worksheet (tiered)

Round 2. This worksheet replaces the long atomic list for hand testing: 34 consolidated tests in three tiers instead of about 240 tiny ones. Nothing was dropped: every test has a `Covers:` line with the ids of the full plan `TEST_PLAN_PHASE_B_FULL.md`, which stays the reference (look an id up there when an expectation is unclear, or to retest one item on its own).

- In round 1 the tester stopped at the old section 2. The three tests that failed in round 1 are all in **Tier 1** and are retested after the fixes: the tiling-UV plane under Auto (FULL 2.6, here 1.4), the lone 1002 mesh under Auto (FULL 2.12, here 1.5) and Reimport with UDIM tiles Off leaving tiles behind (FULL 2.16, here 1.7). FBX import was also reported broken (here 2.12).
- **Do Tier 1 first** (about 30 minutes) and report the result. Continue with Tier 2 and then Tier 3 only if Tier 1 is clean (every Tier 1 test PASS). A Tier 1 failure is fixed first and Tier 1 is rerun.
- Every test starts from the project it names in its first step (a fresh import, the default cube, or a named saved file). No test depends on the state left by another test, except where a saved file is named explicitly (for example `ex.arm` from 1.10).
- For each test tick PASS or FAIL, tick every assert and fill in `Notes:`. Then fill in the summary. All checkboxes are unchecked for this round.

Branch `feat/udim-integration`, phase B (B1 UDIM tile import, B2 tile list and atlas relayout, B3 tile coverage in viewport/paint/sculpt, B4 footprint masks, B5 export modes, B6 bakes, B7 migration of name-based UDIM projects), plus the round 1 fixes and the Bake Texture node per tile fix.

On macOS, ctrl in shortcuts means cmd. "Layer row" means a row in the **Layers** tab. Every layer and group row has an **Objects button** (reads `Objects: All`, an object name, `Name1, Name2`, `N objects` or `No objects`). With more than one tile the row also has a **Tiles button** to its right (reads `Tiles: All`, `1012`, `1001, 1002`, `N tiles` or `No tiles`). Clicking either opens a popup with an `All` check, a separator and one check per object or tile. The popup stays open while you toggle.
Console messages show in the **Console** tab (and briefly in the status bar).
"Tile" means a UDIM tile id: `1001 + floor(u) + 10 * floor(v)`. The project atlas has `stride x stride` slots, `stride` = smallest s with s*s >= tile count, slots are filled in sorted tile order row by row, so in the 2D view tile 1001 is at the top left, the next tile to its right.
"Fresh import" means File > Import > Mesh... (replaces the meshes, clears the layers), **Split By** `Object` unless said otherwise.

## Summary

| Tier | Tests | Time | PASS | FAIL |
|------|-------|------|------|------|
| 1 Smoke | 11 (1.1 to 1.11) | about 30 min | ___ | ___ |
| 2 Core | 12 (2.1 to 2.12) | about 1.5 h | ___ | ___ |
| 3 Edge | 11 (3.1 to 3.11) | about 1.5 h | ___ | ___ |
| Total | 34 | | ___ | ___ |

Phase B passes when Tiers 1 and 2 pass and Tier 3 has no crash, data loss or export mismatch. Any crash, lost paint after relayout/save/reload, paint on an uncovered tile or object, or an export tile that does not match the viewport is a FAIL. Items listed under Known limits are not failures.

## 0. Prerequisites

0.1 App. The app built for this round is at
`/private/tmp/claude-501/-Users-martin-crawford-Development-Workspaces-Game-armorpaint/b8bfd9f3-2c7e-4d01-9c91-d7daa0a9e8e7/scratchpad/dd_integration/Build/Products/Release/ArmorPaint.app`
(it lives in a temp folder and can vanish). To rebuild anywhere with another `<dd>`:
```
cd paint && ../base/make
cd build && xcodebuild -project ArmorPaint.xcodeproj -scheme ArmorPaint -configuration Release -derivedDataPath <dd> build
```

0.2 Test data, all in `/Users/martin.crawford/Development/Workspaces/Game/armorpaint/docs/udim/testdata/`. Each OBJ object must import as exactly ONE object; if you see `.001` copies (`Floor.001`, `Body.001` ...), regenerate: `cd docs/udim/testdata && python3 gen_phase_b.py . && python3 gen_obj.py .` (FBX copies: `blender -b --factory-startup --python obj_to_fbx.py -- in.obj out.fbx`). Check tiles per object with `python3 check_tiles.py <obj>`.
- `udim_2x2_multi.obj` and `.fbx`: 5 objects, tiles 1001, 1002, 1011, 1012. `Body` (x=-4.5) spans 1001 (3 faces) and 1002 (3 faces). `Head` (x=-1.5) on 1011. `Arm` (x=1.5) on 1012 left half. `Leg` (x=4.5) on 1012 right half. `Prop` (x=7.5) on 1001 top half. Tile count 4, stride 2.
- `udim_straddle.obj` and `.fbx`: object `Strip`, tiles 1001 and 1002, exactly 1 triangle crosses the u=1 border.
- `udim_tiling_uv.obj`: object `Floor`, an upright 2x2 plane facing the default camera, UVs 0..4 (tiling), 8 straddling triangles. Auto must decide Off.
- `udim_single_1002.obj`: object `Crate`, entirely in tile 1002.
- `udim_12_tiles.obj`: 12 cubes `T1001`..`T1006`, `T1011`..`T1016`, each on its own tile. Stride 4.
- `udim_seam_shared_vertex.obj`: object `Seam`, two quads sharing an edge in 3D, left quad in 1001, right quad in 1002.
- `three_cubes_separate_uv.obj`: cubes `A`, `B`, `C` in disjoint quadrants of 1001. `three_cubes_overlapping_uv.obj`: the same cubes, all using the whole 0-1 tile.
- `udim_body_two_tiles.obj`: object `Body`, left cube in 1001, right cube in 1002 (for the migration project 0.4).

0.3 Dialogs:
- **Import Mesh**: File > Import > Mesh.... Box with **Split By** (`Object` / `Material`), **UDIM tiles** (`Auto` / `On` / `Off`, OBJ and FBX only), `Cancel`, `Import`, and a chevron next to `Import` whose menu has `Append` (keeps meshes and layers). File > Reimport Mesh opens the same box for the current mesh file and keeps the layers.
- **Export Textures**: File > Export > Textures... (Resolution, Color bits, **Layers** combo, Preset, Destination, Padding, Export). Preset `generic` writes `base`, `nor`, `occ`, `rough`, `metal`.
- **Export Mesh**: File > Export > Mesh... (tab `Export Mesh`: Format, Meshes, Export).
- **Layers tab Edit menu**: the `Edit` button in the Layers tab header; first line is the resolution combo (label `Resolution`; with more than one tile a line under it reads like `4 tiles, atlas 4096 px, about 192 MB`).
- **Bake Maps**: sidebar tab **Plugins**, panel `Bake Maps`, button `Bake Mesh Maps...` (enable `bake_maps` in Plugins > `Preferences` if missing).
- **2D view**: shift+tab, or the `2D View` button at the top of the Layers tab. The slot grid with tile ids shows with more than one tile.
- **Path tracing**: the viewport's `Viewport Mode` combo, entry `Path Traced`.
- Fill layers use the selected material. For tests that need coloured fills (grey, red, blue, yellow, green), make one material per colour in the Materials tab and select it before **New > Fill Layer**, or use any clearly different colours and note which is which.

0.4 Old v23 project `old_udim.arm` (only for Tier 2 test 2.10). Make it with an upstream build:
- `git worktree add /tmp/ap-upstream 22097dfdd`, build it as in 0.1 with `-derivedDataPath /tmp/ap-upstream-dd`, run `/tmp/ap-upstream-dd/Build/Products/Release/ArmorPaint.app`.
- File > Import > Mesh... `udim_body_two_tiles.obj`, **Split By** `UDIM Tile` (upstream only), Import. The Meshes tab shows `Body.1001` and `Body.1002`.
- Note the project resolution R (Layers tab Edit menu, default `2048`): ______.
- Rename `Layer 1` to `Shared` (object combo stays `Shared`), paint a bold red stroke across both cubes and one stroke near the top of each cube.
- **New > Paint Layer** `OnlyB2`, object combo `Body.1002`, paint a bold blue stroke on the right cube. Optional (for the mask check in 2.10): add a mask to `OnlyB2` and paint on it.
- Screenshot the viewport (front, fixed camera) and the 2D view for `Shared` and `OnlyB2`.
- File > Save As... `old_udim.arm`. Quit. `git worktree remove --force /tmp/ap-upstream`, delete `/tmp/ap-upstream-dd`.

0.5 Bake Texture node graph (used by 2.9 and 3.11):
1. Materials tab: select the default material `Material`, click `Nodes`.
2. Node editor menubar `Texture` > `Bake Texture` (node with a `Bake` combo, a `Bake` button and a `Color` output).
3. Link its `Color` output to `Base Color` of `Material Output` (replace the existing link).
4. Pick the bake type in the `Bake` combo, click `Bake`, wait until the button stops showing a percentage.
5. Layers tab: **New > Fill Layer** (uses the selected material).
To compare with the old behaviour use the build of `feat/udim-object-tile-selection` (tip `91cd5b54b`).

## Tier 1: Smoke (about 30 minutes, do first)

### 1.1 Plain cube: no tile UI, brush, fill, mask and hide work as upstream
Covers: FULL plan ids 1.1, 1.2, 1.3, 1.4, 1.5, 4.8 (step 1), 4.9 (step 1, Edit menu), 5.1, 7.2
[ ] PASS  [ ] FAIL
1. Start the app (default cube). Open the Layers tab Edit menu, look at the layer row, the Meshes tab and the Layers tab `Filter` combo.
   - [ ] The combo label reads `Resolution` and there is no `tiles, atlas` line under it
   - [ ] There is no Tiles button on the layer row
   - [ ] There is no `Tiles` panel in the Meshes tab
   - [ ] There are no `Tile ...` entries in the `Filter` combo
2. Paint a Brush stroke across the cube.
   - [ ] The stroke appears on the cube
3. Click **New > Fill Layer**.
   - [ ] The fill covers the cube
4. Add a mask to a layer and paint black on it.
   - [ ] The mask hides and shows the layer as on upstream
5. **New > Paint Layer**, paint on it, then hide it with its eye.
   - [ ] The second layer's paint disappears from the cube
   - [ ] The Console shows no errors after any step
Notes: ______

### 1.2 Plain cube: export writes the upstream files and the project reopens unchanged
Covers: FULL plan ids 1.6, 1.7, 1.8, 4.9 (step 1, Export box), 10.15, 12.12
[ ] PASS  [ ] FAIL
1. Start the app (default cube). Paint a Brush stroke, add **New > Paint Layer** with a second stroke. File > Save As... `plain.arm` into a scratch folder.
2. File > Export > Textures... and open the **Layers** combo.
   - [ ] The combo lists `Visible`, `Selected`, `Per Object` only (no tile modes)
   - [ ] The resolution label reads `Resolution`
3. Set **Layers** `Visible`, preset `generic`, click Export.
   - [ ] Files written: `plain_base.png`, `plain_nor.png`, `plain_occ.png`, `plain_rough.png`, `plain_metal.png`
   - [ ] Each file is 2048 x 2048
   - [ ] No file has a `_1001` suffix or an object name
4. File > New, then File > Open... `plain.arm`.
   - [ ] The same layers and paint are there
   - [ ] There is no tile UI (no Tiles button, no `Tiles` panel, no line under the Resolution combo)
   - [ ] The Console shows no errors
Notes: ______

### 1.3 udim_2x2_multi imports with tiles under Auto
Covers: FULL plan ids 2.1, 2.2, 2.3, 3.1, 3.2, 3.3, 4.8 (step 3), 4.9 (step 3), 5.3, 5.4
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj`, **UDIM tiles** `Auto`, click Import.
   - [ ] Tiles are on without any prompt and the Console shows no warning
   - [ ] The Meshes tab lists exactly `Body`, `Head`, `Arm`, `Leg`, `Prop` (no `.001` copies)
   - [ ] The `Tiles` panel (bottom of the Meshes tab) has four rows `1001`, `1002`, `1011`, `1012`, each with `N objects` and `2048 x 2048`
   - [ ] `1001` reads `2 objects`, `1002` `1 objects`, `1011` `1 objects`, `1012` `2 objects`
   - [ ] No row has a `Remove` button
2. Hover each row in the Meshes tab.
   - [ ] Second tooltip lines: `Body` `Tiles: 1001-1002`, `Head` `Tiles: 1011`, `Arm` `Tiles: 1012`, `Leg` `Tiles: 1012`, `Prop` `Tiles: 1001`
3. Open the Layers tab Edit menu, then the Export Textures box (Cancel it).
   - [ ] The Edit menu combo reads `Resolution` `2048`, the line under it reads `4 tiles, atlas 4096 px, about 192 MB`
   - [ ] The Export Textures resolution label reads `Resolution` (never `Resolution (per tile)`)
4. Look at the `Layer 1` row and hover its Tiles button.
   - [ ] Two buttons sit side by side: `Objects: All` and `Tiles: All`
   - [ ] The Tiles tooltip reads `Limit this layer to the selected UDIM tiles.`
5. Open the 2D view (layer type).
   - [ ] It shows a 2x2 grid: `1001` top left, `1002` top right, `1011` bottom left, `1012` bottom right
Notes: ______

### 1.4 Tiling-UV plane and straddling mesh: Auto decides Off, On forces tiles and clamps (round 1 failure)
Covers: FULL plan ids 2.6, 2.7, 2.8, 2.9
[ ] PASS  [ ] FAIL
Note: Off wraps each UV corner on its own (see Known limits), so strokes on the tiling plane show no repeating pattern; that is not tested.
1. Start the app. Fresh import `udim_tiling_uv.obj`, **UDIM tiles** `Auto`, click Import. Do not move the camera.
   - [ ] `Floor` is visible in the viewport right after import
   - [ ] The Meshes tab lists only `Floor`
   - [ ] There is no `Tiles` panel, no line under the Resolution combo, no Tiles button on the layer row
   - [ ] The Console shows no message about tiles or clamped triangles
2. Click **New > Fill Layer**.
   - [ ] The fill colour covers the whole plane
3. Fresh import `udim_straddle.obj`, **UDIM tiles** `Auto`.
   - [ ] The Meshes tab lists exactly `Strip`
   - [ ] There is no `Tiles` panel (a triangle crosses the border, so Auto decides Off) and no Console warning
4. Fresh import `udim_straddle.obj`, **UDIM tiles** `On`. Open the Console tab.
   - [ ] `1 triangles cross UDIM tile borders and were clamped` appears once
   - [ ] The Meshes tab lists exactly `Strip`, the `Tiles` panel lists `1001` and `1002`
   - [ ] The `Strip` row tooltip reads `Strip` then `Tiles: 1001-1002`
   - [ ] The 2D view shows a 2x2 grid with `1001` and `1002` in the top row
5. Paint a stroke across the middle of `Strip`.
   - [ ] Only the one clamped triangle is distorted, the other quads look right
Notes: ______

### 1.5 Lone 1002 mesh: Auto and On give tile 1002, Per Tile exports 1002 (round 1 failure)
Covers: FULL plan ids 2.10, 2.11, 2.12, 4.8 (step 2), 4.9 (step 2), 5.2, 10.17, 10.18
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_single_1002.obj`, **UDIM tiles** `Auto`.
   - [ ] The Meshes tab lists exactly `Crate`, its row tooltip reads `Tiles: 1002`
   - [ ] The `Tiles` panel lists only `1002` (row reads `1002`, `1 objects`, `2048 x 2048`, no Remove button)
   - [ ] The Edit menu label reads `Resolution` and there is no line under the combo
   - [ ] There is no Tiles button on the layer row and no `Tile ...` entry in the `Filter` combo
2. Fresh import `udim_single_1002.obj` again, now with **UDIM tiles** `On`.
   - [ ] The Meshes tab lists exactly `Crate`; the `Tiles` panel lists `1002` only (row reads `1002`, `1 objects`, `2048 x 2048`, no Remove button)
   - [ ] The Edit menu label reads `Resolution` and there is no `tiles, atlas` line
3. Paint a stroke on `Crate`.
   - [ ] The mesh appears painted on all faces as drawn
4. File > Save As... `crate.arm`. File > Export > Textures... and open the **Layers** combo.
   - [ ] The resolution label reads `Resolution`
   - [ ] The combo lists all 5 modes (`Visible`, `Selected`, `Per Object`, `Per Tile`, `Per Object & Tile`) and starts on a tile mode
5. Choose `Per Tile` and click Export.
   - [ ] Files `crate_base_1002.png` etc. are written, each 2048 x 2048
   - [ ] No `_1001` file is written
Notes: ______

### 1.6 Fresh import with UDIM tiles Off after a UDIM import leaves no tiles
Covers: FULL plan ids 2.4, 2.5, 2.19
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj`, **UDIM tiles** `Auto` (the Tiles panel shows four tiles).
2. Fresh import the same file, **UDIM tiles** `Off`.
   - [ ] There is no `Tiles` panel
   - [ ] The Edit menu reads `Resolution` with no line under the combo
   - [ ] There is no Tiles button on the layer rows
   - [ ] The Meshes tab lists exactly `Body`, `Head`, `Arm`, `Leg`, `Prop`
3. Paint a stroke on `Body`.
   - [ ] The stroke also appears on `Head`, `Arm`, `Leg`, `Prop` at the same UV spots (everything overlaps in 0..1), as in upstream
Notes: ______

### 1.7 Reimport with Auto, then Off, then On (round 1 failure)
Covers: FULL plan ids 2.16, 2.17, 2.18
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj`, **UDIM tiles** `Auto`. Paint a stroke on `Body`'s 1002 faces, one on `Body`'s 1001 faces and one on `Head`.
2. File > Reimport Mesh, **UDIM tiles** `Auto`, Import.
   - [ ] The Tiles panel still lists `1001`, `1002`, `1011`, `1012`, no row reads `(unused)`
   - [ ] The strokes are still in place
   - [ ] The Console shows no `Tile layout changed` message
3. File > Reimport Mesh, **UDIM tiles** `Off`, Import.
   - [ ] The Console shows `Tile layout changed, undo history was cleared`
   - [ ] The Console shows `Tiles 1002, 1011, 1012 are not used by the imported meshes and were removed with their pixels`
   - [ ] There is no `Tiles` panel (no `(unused)` tile rows), no line under the Resolution combo, no Tiles button on the layer rows
   - [ ] The layers are kept and the Meshes tab lists `Body`, `Head`, `Arm`, `Leg`, `Prop`
4. File > Reimport Mesh, **UDIM tiles** `On`, Import.
   - [ ] The Tiles panel lists `1001`, `1002`, `1011`, `1012`
   - [ ] The Console shows `Tile layout changed, undo history was cleared`
   - [ ] The paint of tile 1001 is kept (the `Body` 1001 stroke shows in 2D view slot `1001`)
Notes: ______

### 1.8 Tiles popup and tile-restricted brush painting
Covers: FULL plan ids 5.5, 5.6, 5.7, 5.8, 5.9, 5.10, 6.1, 6.2, 6.7, 6.10
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto). Front camera so `Body` is clearly visible (its 1001 and 1002 faces are different cube faces). Click `Tiles: All` on `Layer 1`.
   - [ ] The popup shows `All` ticked, a separator, then `1001`, `1002`, `1011`, `1012`, all ticked and greyed out
2. Click a greyed tile row.
   - [ ] Nothing changes and no undo step is added
3. Untick `All`, tick `1002`. Close the popup. Drag a Brush stroke across all faces of `Body` and across every object.
   - [ ] The popup stayed open while toggling and the button reads `1002`
   - [ ] Only the faces of `Body` on tile 1002 get paint; its 1001 faces stay unpainted where the stroke crosses them
   - [ ] `Prop` (1001), `Head`, `Arm`, `Leg` (1011 / 1012) get nothing
4. Open the 2D view. Paint a stroke on slot `1001` (top left), then one on slot `1002` (top right). Go back to the 3D view.
   - [ ] The stroke on slot `1001` does nothing, the stroke on slot `1002` paints
   - [ ] The viewport shows the new stroke on `Body`'s 1002 faces only
5. **New > Paint Layer**. In its Tiles popup untick `All`, tick `1002` and `1012`; then tick `1011`; then untick `1011` and `1002`.
   - [ ] The button reads `1002, 1012`, then `3 tiles`, then `1012`
6. Drag a Brush stroke across `Head`, `Arm` and `Leg`. Then start a stroke on `Head` only.
   - [ ] `Arm` and `Leg` are painted, `Head` is not
   - [ ] Record whether the stroke on `Head` adds an undo step (tile restriction is a shader discard, one may be added): ______
7. Untick `1012` (nothing ticked). Hover the layer name, then drag a stroke.
   - [ ] The button reads `No tiles` and the layer name is dimmed
   - [ ] Hovering shows `No tiles are selected for this layer or its group, so it covers nothing. Select tiles or choose All to use it.`
   - [ ] Nothing is painted and the Console tab title shows `Layer covers no tiles` for a few seconds (not `Layer covers no objects`)
Notes: ______

### 1.9 Fill tool and Fill Layer respect the tile set
Covers: FULL plan ids 6.8, 6.9, 6.12
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto). Set `Layer 1` to `Tiles: 1002`. Pick the **Fill** tool with fill mode `Mesh` or `Object`, click on `Body` and on `Prop`.
   - [ ] Only the covered part of tile 1002 (`Body`'s 1002 faces) is filled
   - [ ] Texels on tiles 1001, 1011, 1012 stay untouched (`Prop` stays empty)
2. **New > Fill Layer**, then set its Tiles to `1011` only.
   - [ ] Only `Head` shows the fill
3. Change the fill layer's Tiles to `1011, 1012`.
   - [ ] The layer refills: `Head`, `Arm` and `Leg` show the fill
4. **New > Paint Layer**, untick `All` and tick all four tiles by hand. Paint across every object.
   - [ ] It behaves like `All` (paint reaches every object), with no visible difference in speed or look
Notes: ______

### 1.10 Viewport matches the Per Tile and Visible exports
Covers: FULL plan ids 8.1, 8.3, 8.4, 10.1, 10.2, 10.3, 10.4, 10.5, 10.6, 10.12, 10.13, 10.14
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto). Add four fill layers (see 0.3 for colours), bottom to top: `Base` (grey, all), `T1002` (red, Tiles `1002`), `ObjHead` (blue, Objects `Head`), `Mix` (yellow, Objects `Body`, Tiles `1001`). File > Save As... `ex.arm` in a scratch folder (keep it, 3.3 uses it). Fix the camera so every object is visible and take a screenshot S1.
   - [ ] `Body` shows red on its 1002 faces and yellow on its 1001 faces
   - [ ] `Head` is blue, the other objects are grey
   - [ ] `Mix` (yellow) is not on `Prop`, even though `Prop` is on 1001
2. File > Export > Textures... and open the **Layers** combo.
   - [ ] It lists `Visible`, `Selected`, `Per Object`, `Per Tile`, `Per Object & Tile` and starts on `Per Tile`
   - [ ] The resolution combo reads `Resolution`
3. **Layers** `Per Tile`, preset `generic`, folder `out_tile`, Export.
   - [ ] 20 files are written (4 tiles x 5 channels): `ex_base_1001.png`, `ex_base_1002.png`, `ex_base_1011.png`, `ex_base_1012.png` and the same for `nor`, `occ`, `rough`, `metal`; every used tile is written even if empty
   - [ ] Every file is 2048 x 2048; there is no `ex_base.png` atlas and no object name
   - [ ] `ex_base_1002.png` has red in the `Body` 1002 island only; `_1001`, `_1011`, `_1012` contain no red
   - [ ] `ex_base_1001.png` has yellow in `Body`'s 1001 faces and none over the `Prop` island
   - [ ] `ex_base_1011.png` has blue in the `Head` island only (blue appears in no other file)
   - [ ] `ex_base_1012.png` has only grey in its islands
4. Compare `ex_base_1002.png` with 2D view slot `1002`.
   - [ ] The content is the same as the slot, with no one-pixel shift and no 1 px line of the neighbouring tile at the edges
5. **Layers** `Visible`, folder `out_vis`, Export. Compare with the 2D view of the flattened layers.
   - [ ] One set is written: `ex_base.png` etc., no suffix, each 4096 x 4096 (the whole atlas)
   - [ ] It matches the 2D view (2x2 slots in the same positions)
6. Select one layer, **Layers** `Selected`, folder `out_sel`, Export.
   - [ ] One atlas set is written as for `Visible`, no object name appended
Notes: ______

### 1.11 Save and reopen keeps tiles, coverage and pixels
Covers: FULL plan ids 12.1, 12.2, 12.3, 12.4, 12.5
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto). Paint strokes on `Body`, `Head`, `Arm` and `Prop` on `Layer 1`.
2. **New > Paint Layer** `L2`: Tiles `1002`, Objects `Body`, paint a stroke on `Body`. **New > Paint Layer** `L3`: Tiles `1011, 1012`, paint across `Head` and `Arm`. **New > Group** with Objects `Head`, drag a new paint layer into it and paint on `Head`.
3. Edit menu Resolution `4096`. Take a viewport screenshot (fixed camera) and a 2D view screenshot. Note the button texts of every layer and the group.
4. File > Save As... `rt.arm`, quit the app, start it again, File > Open... `rt.arm`.
   - [ ] The `Tiles` panel lists `1001`, `1002`, `1011`, `1012`
   - [ ] The Edit menu shows `4096` with `4 tiles, atlas 8192 px, about ...`
   - [ ] The Tiles and Objects buttons are the same on every layer and the group
   - [ ] The viewport and the 2D view are pixel-identical to the screenshots
   - [ ] The Meshes row tooltips read the same `Tiles: ...` as before
5. File > Save As... `rt2.arm`, quit, start again, open `rt2.arm`.
   - [ ] It loads without errors and everything is still the same
Notes: ______

**Stop here and report Tier 1. Continue only if every Tier 1 test passed.**

## Tier 2: Core

### 2.1 Delete an object, remove its unused tile, relayout and save
Covers: FULL plan ids 3.4, 3.5, 3.6, 3.7, 3.8, 3.9, 5.12, 7.9, 12.6
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto). Paint a stroke on `Head` and one on `Arm`. Set the Layers tab `Filter` combo to `Tile 1011`.
2. Meshes tab: right-click `Head` > Delete. Open `Layer 1`'s Tiles popup.
   - [ ] `Head` disappears; in the `Tiles` panel row `1011` reads `0 objects` and shows a `Remove` button, the other rows have none
   - [ ] The popup lists `1011 (unused)`, the other rows show plain ids
3. Click `Remove` on row `1011`, then `Cancel`.
   - [ ] A box `Remove tile` opens with `Remove tile 1011? Its pixels are deleted and undo history is cleared.` and the buttons `Cancel` and `Remove`
   - [ ] `Cancel` closes it and nothing changes
4. Click `Remove` on row `1011` again, then `Remove` in the box.
   - [ ] The Console shows `Tile layout changed, undo history was cleared`
   - [ ] The Tiles panel lists `1001`, `1002`, `1012`, none with a `Remove` button
   - [ ] The Edit menu line reads `3 tiles, atlas 4096 px, about 192 MB` (stride stays 2)
   - [ ] The `Filter` combo falls back to `All` the next time it draws, no crash
5. Open the 2D view.
   - [ ] It shows `1001`, `1002`, `1012` (the 1012 slot moved to bottom left)
   - [ ] The `Arm` stroke is in place in the viewport and in slot `1012`
6. Look at Edit > Undo, then paint a stroke.
   - [ ] Undo has no step; painting still works
7. File > Save As... `rm.arm`, File > New, File > Open... `rm.arm`.
   - [ ] The removed tile is gone, the other tiles' paint is intact, the 2D view slots match
Notes: ______

### 2.2 12-tile grid, atlas limit and clamp
Covers: FULL plan ids 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_12_tiles.obj` (Auto). Paint a stroke on a few cubes. Open the Edit menu, the Meshes tab and the 2D view.
   - [ ] `Resolution` reads `2048`, the line reads `12 tiles, atlas 8192 px, about 768 MB`, no clamp line
   - [ ] The Meshes tab lists exactly `T1001`..`T1006`, `T1011`..`T1016`; the Tiles panel lists all 12 tiles, each `1 objects`
   - [ ] The 2D view is a 4x4 grid: row 1 `1001`..`1004`, row 2 `1005`, `1006`, `1011`, `1012`, row 3 `1013`..`1016`
2. Edit menu > Resolution `4096`.
   - [ ] The line reads `12 tiles, atlas 16384 px, about 3072 MB`, no clamp line
3. Resolution `8192` (needs several GB of GPU memory; skip and note if the machine cannot take it).
   - [ ] The clamp line `Clamped to 4096 px per tile (atlas limit 16384)` appears and the atlas stays `16384 px`
   - [ ] The `Tiles` panel shows `4096 x 4096` per tile and the paint is not corrupted
4. Resolution back to `2048`. Delete `T1004`, `T1005`, `T1006` (Meshes tab, right-click > Delete) and `Remove` tiles 1004 to 1006 (three confirmations).
   - [ ] The Tiles panel lists 9 tiles (stride 3)
5. Resolution `8192`, then back to `2048`.
   - [ ] At 8192 the line reads `9 tiles, atlas 16383 px, about 3072 MB` and the clamp line `Clamped to 5461 px per tile (atlas limit 16384)`; the Tiles panel shows `5461 x 5461`
   - [ ] The value returns to `2048`
Notes: ______

### 2.3 Append a mesh with new tiles: relayout keeps paint and masks, clears undo
Covers: FULL plan ids 4.10, 4.11, 4.12, 4.17, 4.18, 4.19, 14.6, 14.7, 14.8
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto). Select a material with clear roughness and metal values. On `Layer 1` paint distinct strokes on `Body`, `Head`, `Arm`, `Prop`. Click **Add Mask** on `Layer 1` and paint black on the mask over part of `Head`. **New > Paint Layer**, paint strokes on `Body` and `Arm`. Take viewport and 2D view screenshots (fixed camera).
2. File > Import > Mesh... `udim_12_tiles.obj`, chevron next to `Import` > `Append`.
   - [ ] The Console shows `Tile layout changed, undo history was cleared`
   - [ ] The Tiles panel lists 12 tiles `1001`-`1006`, `1011`-`1016`; the Edit line reads `12 tiles, atlas 8192 px, about 768 MB`
   - [ ] Edit > Undo and Redo are empty
3. Compare the viewport with the screenshot.
   - [ ] The five original objects look identical (strokes and the mask cut in the same places, both layers)
   - [ ] The new `T10xx` objects are empty (default colour)
   - [ ] The roughness and metal of the old paint are unchanged
4. Open the 2D view, for the layer and for the mask.
   - [ ] `1001` and `1002` are still top left, `1011` moved to row 2 column 3, `1012` to row 2 column 4, with strokes intact
   - [ ] The layer is empty on the new tiles; the mask is white (not black) on the new tiles
5. Paint a stroke and press ctrl+z.
   - [ ] The new stroke undoes normally
Notes: ______

### 2.4 Append into a plain project and a single-tile append into a tile project
Covers: FULL plan ids 4.13, 4.14, 4.15
[ ] PASS  [ ] FAIL
1. Start the app (default cube). Paint a stroke on the cube. Import `udim_single_1002.obj` with `Append`.
   - [ ] The tile list is `1001`, `1002`; the Edit menu label reads `Resolution` and the line under it shows 2 tiles
   - [ ] The cube keeps its stroke (tile 1001, slot 1); `Crate` is empty and sits on `1002`
2. Fresh import `udim_2x2_multi.obj` (Auto), paint a stroke. Import `three_cubes_separate_uv.obj` with `Append`.
   - [ ] The new objects go to tile 1001 (tooltip `Tiles: 1001`)
   - [ ] The tile list is unchanged and there is no layout-changed message
   - [ ] The existing paint is unchanged
Notes: ______

### 2.5 Objects x Tiles intersection and group tile sets
Covers: FULL plan ids 5.11, 5.13, 5.14, 5.15, 5.16, 5.17, 6.3, 6.4, 6.5, 6.6
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto), front camera. On `Layer 1` (Tiles All) set Objects to `Body` only. Drag a stroke across `Body`, `Head`, `Prop`, then start a stroke on `Head` only.
   - [ ] Only `Body` is painted, on both its tiles
   - [ ] The stroke on `Head` is skipped without an undo step
2. **New > Paint Layer**, Objects `Body`. Tick and untick tiles in its Tiles popup, ending on `Tiles: 1002`. Drag a stroke across `Body`.
   - [ ] The Objects button stays `Objects: Body`
   - [ ] Only the 1002 faces of `Body` are painted
3. Tick and untick objects in the same layer's Objects popup, ending on `Objects: Body`.
   - [ ] The Tiles button stays `1002`
4. **New > Paint Layer**, Objects `Body`, Tiles `1001`. Drag a stroke across `Body` and `Prop`.
   - [ ] Only the 1001 faces of `Body` are painted; `Prop` is never painted
5. Switch this layer to Objects `Prop`, Tiles `1002`.
   - [ ] The layer covers nothing and its name is dimmed
6. In its Tiles popup tick `All`, then untick it again.
   - [ ] The stored list comes back (rows enabled, earlier ticks kept)
7. **New > Group**, drag a new paint layer `Child` (Tiles All) into it. Group Tiles: untick `All`, tick `1012` only. Open `Child`'s Tiles popup.
   - [ ] `1001`, `1002`, `1011` are greyed and unticked, hovering one shows `Excluded by the group`; `1012` is enabled
8. Set the group back to `All`.
   - [ ] Nothing is greyed in `Child`'s popup and its own list is unchanged
9. Set the group to `1001` and `Child` to `1012` only.
   - [ ] `Child`'s name is dimmed and hovering shows the `No tiles are selected ...` tooltip
Notes: ______

### 2.6 Per Object and Per Object & Tile exports
Covers: FULL plan ids 8.5, 8.6, 8.7, 10.7, 10.8, 10.9, 10.10, 10.11
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto). Layers: `L1` fill green (All), `L2` paint layer Tiles `1002` with a red stroke across `Body`'s 1002 faces, `L3` paint layer Objects `Head` with a blue stroke on `Head`. File > Save As... `po.arm`.
2. Export Textures, **Layers** `Per Object`, folder `out_obj`.
   - [ ] Only `po_Head_base.png`, `po_Head_nor.png`, ... (5 files) are written, each 4096 x 4096, no tile suffix
   - [ ] `L1` appears inside `Head`'s footprint only; `L2` (covers 1002) is absent
3. **Layers** `Per Object & Tile`, folder `out_both`.
   - [ ] 30 files named `po_<object>_<ch>_<tile>.png`: `po_Body_base_1001`, `po_Body_base_1002`, `po_Head_base_1011`, `po_Arm_base_1012`, `po_Leg_base_1012`, `po_Prop_base_1001` and the other 4 channels of each, every file 2048 x 2048
   - [ ] The red stroke is in `po_Body_base_1002.png` only; nothing of `Prop` appears in `po_Body_base_1001.png`
4. Hide `L1` (eye off), export `Per Object & Tile` again to an empty folder `out_both2`. Show `L1` again.
   - [ ] Only `Head` / `1011` (from `L3`) and `Body` / `1002` (from `L2`) are written
5. Add fill layer `L4` yellow, Objects `Body`, Tiles `1001`. Export `Per Object` to an empty folder `out_obj2`.
   - [ ] `po_Head_*.png` and `po_Body_*.png` are written, each a full atlas (4096 x 4096)
   - [ ] The Body files hold yellow and red in the Body islands plus `L1`, and no blue
   - [ ] The Head files hold `L1` and the blue stroke in the Head island
Notes: ______

### 2.7 Bake Maps AO bake: atlas, tile crops and UV Atlas
Covers: FULL plan ids 11.1, 11.2, 11.3, 11.4, 11.5, 11.6, 11.7, 11.8, 11.9
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto), Resolution `2048` (atlas 4096). File > Save As... `bk.arm`. Open `Bake Mesh Maps...`: `Maps` > `Select None`, tick `Ambient Occlusion`; `Settings` > `Common` `Resolution` `2048`; `Settings` > `Output` `Name` `bkao`, `...` folder an empty `bake_out`. Click `Bake` (GPU default).
   - [ ] The Console shows `Baked 1 maps`, then `Baked maps linked in material bkao Baked`
2. Look in `bake_out` and the project Textures tab.
   - [ ] `bkao_ao.png` is 4096 x 4096; `bkao_ao_1001.png`, `_1002`, `_1011`, `_1012` exist, each 2048 x 2048
   - [ ] Only the atlas (`bkao_ao`) is imported into the project, not the crops
   - [ ] Each crop equals its atlas slot (`1001` top left, `1002` top right, `1011` bottom left, `1012` bottom right) with AO on that tile's islands
3. Materials tab: select `bkao Baked`, open `Nodes`, select the image texture node.
   - [ ] An image node is linked to Occlusion and its `UV` button reads `Atlas` (options `Default` and `Atlas`)
4. Click `Add Baked Layer` in the Plugins panel.
   - [ ] A fill layer with `bkao Baked` is added and the AO shows correctly on all objects of all tiles, no repeats, no tile showing the whole atlas squeezed in
5. In the image node switch `UV` to `Default`, then back to `Atlas`.
   - [ ] With `Default` the map no longer fits (repeats per tile); old canvases with only two buttons behave as `Default`
   - [ ] With `Atlas` the correct map returns
6. Click the plugin `View` buttons `AO` / `Normal` / `Cavity`.
   - [ ] The viewport shows the baked AO on all tiles, no seams, no empty tiles
Notes: ______

### 2.8 Bake Maps Inpaint and bake resolutions
Covers: FULL plan ids 11.10, 11.11, 11.12, 11.13, 11.14, 11.15, 11.16
[ ] PASS  [ ] FAIL
1. Starting project: `bk.arm` with the AO bake of 2.7 in `bake_out` (if missing, do step 1 of 2.7). Run `ls -l@ bake_out` and note the times.
2. Pick the plugin tool `Inpaint` (Tools list), mode Copy, paint a short stroke on the `Head` island (tile 1011), release.
   - [ ] The Console shows `Inpaint: 1 maps repaired`
   - [ ] `bkao_ao.png` and `bkao_ao_1011.png` have newer timestamps
   - [ ] `bkao_ao_1011.png` equals slot 1011 of the new atlas and shows the repaired area
   - [ ] The crops of untouched tiles keep the same pixels
3. Bake again at `Resolution` `4096`, then at `1024`.
   - [ ] 4096: atlas 8192 x 8192, crops 4096 x 4096; 1024: atlas 2048 x 2048, crops 1024 x 1024; names `bkao_ao_...` unchanged
4. Set the project Edit menu resolution `4096`, bake at script `Resolution` `2048`.
   - [ ] The bake follows the script Resolution (2048 per tile), not the project resolution; sizes: ______
Notes: ______

### 2.9 Bake Texture node bakes per tile
Covers: FULL plan ids 17.1, 17.2, 17.3, 17.4, 17.6, 17.7, 17.8, 17.9
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto), Resolution `2048`. Build the node graph of 0.5 with `Bake` = `TexCoord`, bake, add the fill layer. Look at the fill layer's 2D view and the node preview.
   - [ ] The 2D view equals the node preview; every UV island is drawn once, in its own tile slot
   - [ ] Red and green form ONE smooth gradient over the whole 2x2 grid, no jump back at slot borders, no slot holds a shrunken copy of the whole grid
2. Look at the viewport (Lit, front view).
   - [ ] Each object shows its own island colours; objects on different tiles have clearly different colour ranges; `Body`'s 1001 and 1002 faces differ; no object shows patches of other islands
3. `Bake` = `Position`, bake (reselect the fill layer if it does not update).
   - [ ] Each object is coloured by its own world position (`Body` at x=-4.5 differs from `Leg` at x=4.5); each slot holds only its tile's islands, no repeated atlas
4. `Bake` = `Curvature`, bake.
   - [ ] Edge highlights follow the real cube edges on all four tiles, no edge lines across faces
5. `Bake` = `TexCoord`. Select `Layer 1`, set its Tiles to `1001` only, click `Bake` with `Layer 1` selected, then select the fill layer.
   - [ ] The fill layer (Tiles All) shows the bake on all four tiles as in step 1
6. Select `Layer 1` and paint on it.
   - [ ] `Layer 1` keeps its own content (the bake does not paint into it) and painting stays on 1001
7. Set the fill layer's Tiles to `1002, 1012`.
   - [ ] Only `Body`'s 1002 faces and `Arm`/`Leg` show the bake, each with its own part; 1001 and 1011 show the layers below
8. Set the fill layer back to Tiles All. Export Textures `Per Tile`.
   - [ ] Each `<name>_base_<tile>.png` (2048 x 2048) equals its 2D view slot of step 1 and holds only that tile's islands
Notes: ______

### 2.10 B7 migration of the old name-based UDIM project
Covers: FULL plan ids 13.1, 13.2, 13.3, 13.4, 13.5, 13.6, 13.7, 13.8, 13.9, 13.10, 13.11, 13.12, 13.13, 13.14
[ ] PASS  [ ] FAIL
1. Starting project: `old_udim.arm` from 0.4. Start the app, File > Open... `old_udim.arm`.
   - [ ] It opens without a crash or errors; the Meshes tab keeps `Body.1001` and `Body.1002`
   - [ ] The `Tiles` panel lists `1001` and `1002` at `R/2 x R/2` per tile (for R = 2048: `1024 x 1024`); the Edit line reads `2 tiles, atlas <R> px, ...`
   - [ ] Meshes tooltips: `Body.1001` `Tiles: 1001`, `Body.1002` `Tiles: 1002`
2. Compare the viewport and the 2D views with the upstream screenshots.
   - [ ] The `Shared` stroke crosses both cubes where it was; `OnlyB2` is only on the right cube
   - [ ] `Shared` 2D view: `1001` top left, `1002` to its right, same pixels, labels `1001`, `1002`
   - [ ] `OnlyB2` 2D view: stroke in slot `1002`, the rest empty
   - [ ] Optional (mask made in 0.4): `OnlyB2`'s mask is in its slot and white outside it, the layer shows as before
3. Look at the layer rows.
   - [ ] `OnlyB2` reads Objects `Body.1002`, `Tiles: All`; `Shared` reads `Objects: All`, `Tiles: All`
4. Paint a stroke on `OnlyB2`, strokes on `Shared` across both cubes, and add **New > Fill Layer** with `Tiles: 1002`.
   - [ ] `OnlyB2` paints only the right cube; `Shared` paints normally on both
   - [ ] The fill appears only on the right cube
5. File > Save As... `migrated.arm`, File > New, open `migrated.arm`.
   - [ ] It loads as a normal phase B project; tiles, resolution and buttons are as in steps 1 to 3
6. Rename `Body.1002` to `Foo`.
   - [ ] The paint and Tiles stay as they were (no name-based handling)
7. Export Textures `Per Tile`.
   - [ ] `<f>_base_1001.png` and `<f>_base_1002.png` are written at `R/2` px; `OnlyB2`'s content is only in the `_1002` file
Notes: ______

### 2.11 Undo and redo of tile coverage edits
Covers: FULL plan ids 14.1, 14.2, 14.3, 14.4, 14.5, 14.9, 14.10
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto). Paint `Layer 1` across every object. In its Tiles popup untick `All`, tick `1002`. Press ctrl+z.
   - [ ] The Edit menu shows `Undo Layer Object` (the same entry is used for tile and object coverage)
   - [ ] The button returns to `Tiles: All` and the paint reappears on every tile
2. Press ctrl+shift+z.
   - [ ] The button is back to `1002`
3. Tick and untick several tiles in the layer's Tiles popup, undo step by step. Repeat with a group's (**New > Group**) Tiles popup.
   - [ ] Each tick or untick is one undo step and the exact sets are restored (layer and group)
4. Paint strokes before and after a coverage change, then undo and redo through them.
   - [ ] Strokes and coverage interleave correctly and the pixels return exactly
5. Edit menu Resolution `4096`, then back to `2048`.
   - [ ] No crash; the paint is kept (scaled uniformly, tiles keep their slots); undo behaves as upstream for resolution changes
6. **New > Paint Layer** with Objects `Arm`, paint on `Arm`, then delete `Arm` in the Meshes tab (object delete has no undo).
   - [ ] No crash; the layer covers nothing (dimmed, phase A behaviour)
Notes: ______

### 2.12 FBX import with Auto and On
Covers: FULL plan ids 2.20, 2.21, 2.22
[ ] PASS  [ ] FAIL
If an FBX step fails, record the file, the UDIM tiles and Split By settings, and exactly what looks wrong (size, shading, tiles, crash, console message).
1. Start the app. Fresh import `udim_2x2_multi.fbx`, **UDIM tiles** `Auto`. Do not move the camera.
   - [ ] The Meshes tab lists `Body`, `Head`, `Arm`, `Leg`, `Prop` (record any Blender suffixes: ______)
   - [ ] The objects are visible at a sensible size right after import
   - [ ] The Tiles panel lists `1001`, `1002`, `1011`, `1012`; tooltips `Body` `Tiles: 1001-1002`, `Head` `Tiles: 1011`, `Arm` and `Leg` `Tiles: 1012`, `Prop` `Tiles: 1001`
2. Paint a stroke on `Body`.
   - [ ] The stroke appears on `Body`
3. Fresh import `udim_straddle.fbx`, **UDIM tiles** `Auto`.
   - [ ] `Strip` is visible at a sensible size; there is no `Tiles` panel and no Console message
4. Fresh import `udim_straddle.fbx`, **UDIM tiles** `On`.
   - [ ] The Tiles panel lists `1001`, `1002`; the Console shows `1 triangles cross UDIM tile borders and were clamped`; `Strip` is visible at a sensible size
Notes: ______

## Tier 3: Edge

### 3.1 Sculpt layers restricted by tile, and sculpt across save and reopen
Covers: FULL plan ids 6.13, 6.14, 6.17, 12.15, 12.16
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto). **New > Sculpt Layer**, Tiles `1012` only. Sculpt (Brush or Grab) across `Arm`, `Leg`, `Head`.
   - [ ] `Arm` and `Leg` deform, `Head` does not
2. Set the sculpt layer's Tiles to `1002`, sculpt across `Body`.
   - [ ] Only `Body`'s 1002 faces deform, plus the shared edge vertices at the 1001/1002 border; no cracks along the border
3. Set the sculpt layer to `No tiles`, sculpt.
   - [ ] Nothing deforms, the Console tab title shows `Layer covers no tiles`, no undo step is added
4. **New > Sculpt Layer** (Tiles All), sculpt a visible bump on `Body` in tile 1001 and on `Head`. File > Save As... `sc.arm`, quit, start again, open `sc.arm`.
   - [ ] Both bumps are back exactly as sculpted, no scrambled spikes or flattened areas
5. Sculpt again.
   - [ ] Sculpting works without jumps
Notes: ______

### 3.2 Seam between two tiles: painting, border bleed and sculpt
Covers: FULL plan ids 2.15, 6.15, 6.16, 6.20
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_seam_shared_vertex.obj`, **UDIM tiles** `On`.
   - [ ] The Meshes tab lists exactly `Seam`; no clamp message
2. Paint a stroke across the shared edge, on both sides. Open the 2D view.
   - [ ] The stroke is continuous across the 3D seam
   - [ ] No triangle is stretched across the atlas from slot `1001` to `1002`; the stroke shows in both slots near their facing borders
3. Paint a bold stroke along the diagonal between the two triangles of one quad. Look closely at the slot edges in the 2D view.
   - [ ] No bleed of one slot into the neighbouring slot; no dark or light line along the tile borders
4. **New > Sculpt Layer**, Tiles `1002` only, grab or inflate on the shared edge. Then set Tiles `1001` only and do the same.
   - [ ] With 1002 the shared edge vertices move with the 1002 quad, the surface stays closed
   - [ ] With 1001 the shared edge vertices move with the 1001 quad, no tearing
Notes: ______

### 3.3 Path tracing agrees with the raster view
Covers: FULL plan ids 1.9, 8.2, 8.8, 12.18, 12.19
[ ] PASS  [ ] FAIL
1. Start the app (default cube). Paint a stroke. Viewport Mode `Path Traced`, wait for a few samples, then switch back.
   - [ ] The paint shows in the path traced view as in the raster view, no Console errors
   - [ ] The raster view returns
2. Starting project: `ex.arm` from 1.10 (rebuild it with step 1 of 1.10 if missing). Open it, set the camera as for S1, `Path Traced`, wait for convergence, screenshot S2.
   - [ ] S2 shows the same coverage as S1 (red/yellow on `Body`, blue `Head`, grey elsewhere, lighting aside)
   - [ ] Dilate padding reaches at most a few pixels beyond a UV island, never visibly onto another object
3. Still path traced, add a layer with Objects `Head` and toggle its visibility several times.
   - [ ] The view refreshes without a long stall or a crash; stall times: ______
4. Optional, if you have an OBJ with vertex colours or a second UV map: fresh import it with **UDIM tiles** `Off`, restrict a layer to one object, `Path Traced`, then export `Visible`.
   - [ ] The layer shows only on that object in the path traced view and in the exported files, matching the raster footprint (no garbage shapes, no missing areas)
Notes: ______

### 3.4 Single-tile objects, Per Object export and the overlap tooltip
Covers: FULL plan ids 1.10, 1.11, 1.12, 8.9, 9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 10.15
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `three_cubes_separate_uv.obj`, **UDIM tiles** `Auto`.
   - [ ] The Meshes tab lists exactly A, B, C; the Edit menu reads `Resolution`; no Tiles button, no `Tiles` panel; no Console message about tiles
   - [ ] The Export Textures **Layers** combo lists `Visible`, `Selected`, `Per Object` only
2. On `Layer 1` set Objects to `B` only, paint across all three cubes. Then set `Layer 1` back to `Objects: All`.
   - [ ] Only cube B got the stroke
3. **New > Paint Layer** `OnlyB`, Objects `B`, paint heavily over all three cubes. Hover its Objects button. File > Save As... `three.arm`, export `Per Object`.
   - [ ] The tooltip shows only its first line (disjoint quadrants)
   - [ ] Files like `three_B_base.png` are written for `B` only, none for `A` or `C`, no tile suffix
   - [ ] B's file holds `OnlyB` only inside B's UV quadrant (plus padding); the A and C quadrants hold only unrestricted layers
4. Viewport Mode `Path Traced`.
   - [ ] `OnlyB` shows on B only
5. Fresh import `three_cubes_overlapping_uv.obj` (Auto). On `Layer 1` set Objects to `A` only, hover the Objects button. Set it to `Objects: All`, hover again. Set it back to `A`.
   - [ ] With `A`: first line `Limit this layer to the selected objects.`, a second line like `A shares UV space with B on tile 1001. In exports and path tracing this layer also applies to B.`
   - [ ] With `All`: only the first line
6. Paint a stroke on A, File > Save As... `ovl.arm`, export `Per Object` (documented limit, not a failure).
   - [ ] The A set holds the stroke; the same texels are used by B and C (paint also on B and C in exports and path tracing, only on A in the raster viewport)
7. Fresh import `udim_2x2_multi.obj` (Auto). Set a layer to Objects `Arm`, hover; then Objects `Body`, hover.
   - [ ] No overlap sentence for `Arm` (different halves of 1012) nor for `Body` (`Prop` is on the other half of 1001; edge contact does not count)
Notes: ______

### 3.5 Mesh export writes UDIM UVs
Covers: FULL plan ids 15.1, 15.2, 15.3, 15.4, 15.5, 15.6, 15.7
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto). File > Export > Mesh..., Format `OBJ`, Meshes `All`, export to `mesh_out/mesh.obj`.
   - [ ] The file is written; its `vt` lines hold UDIM UVs with u and v in 0..2 (for example `Head` v between 1.02 and 1.98), not atlas UVs in 0..1
2. Run `python3 /Users/martin.crawford/Development/Workspaces/Game/armorpaint/docs/udim/testdata/check_tiles.py <path>/mesh_out/mesh.obj`.
   - [ ] `Body` `[1001, 1002]`, `Head` `[1011]`, `Arm` and `Leg` `[1012]`, `Prop` `[1001]`, `straddling=0` for all
3. Fresh import `mesh_out/mesh.obj` (Auto).
   - [ ] The tile list is `1001`, `1002`, `1011`, `1012` and the Meshes tooltips are as in 1.3 step 2
4. Export only `Head` (Meshes combo) as OBJ; if the GLB format exists, export `Head` as GLB too.
   - [ ] Only `Head` is written, its `vt` v values are in 1..2; the GLB UVs are UDIM UVs too
5. File > New (default cube), export the mesh as OBJ.
   - [ ] UVs are 0..1 as upstream and nothing extra is written
Notes: ______

### 3.6 Import odd cases: Split By, resolution reset, other formats, Reimport moving 1001 to 1002
Covers: FULL plan ids 2.13, 2.14, 2.23, 2.24, 2.25, 2.26
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj`, **UDIM tiles** `On`, **Split By** `Object`. Then again with **Split By** `Material` (the file has no `usemtl`).
   - [ ] Both give the tile list `1001, 1002, 1011, 1012` and exactly 5 objects; Material count: ______
2. File > New (default cube), Edit menu Resolution `4096`. Fresh import `udim_2x2_multi.obj` (Auto). Then fresh import `three_cubes_separate_uv.obj`.
   - [ ] After the UDIM import the combo reads `2048` and the line `4 tiles, atlas 4096 px, about 192 MB`
   - [ ] The plain import does not change the resolution again (stays `2048`)
3. Optional: File > Import > Mesh... a `.glb` or `.blend` with UVs above 1.
   - [ ] The box has no **UDIM tiles** radio; the mesh wraps and there is no `Tiles` panel
4. Fresh import `udim_single_1002.obj`, **UDIM tiles** `Off`. Paint a bold stroke on `Crate`, take a screenshot (fixed camera). File > Reimport Mesh, `Auto`.
   - [ ] The Tiles panel lists only `1002` and the stroke is unchanged
   - [ ] No `Tile layout changed` and no `were removed with their pixels` message
5. File > Reimport Mesh, `Off`.
   - [ ] No `Tiles` panel; the stroke is still unchanged
Notes: ______

### 3.7 Layer filter by tile, mini menus, other tools and previews
Covers: FULL plan ids 5.18, 5.19, 6.11, 6.18, 6.19, 7.1, 7.3, 7.4, 7.5, 7.6, 7.7, 7.8
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto). Layers: `L_all` (Tiles All), `L1012` (Tiles `1012`), `L1001` (Tiles `1001`). Open the `Filter` combo.
   - [ ] Entries: `All`, the 5 objects, then `Tile 1001`, `Tile 1002`, `Tile 1011`, `Tile 1012`
2. Choose `Tile 1012`, then **New > Paint Layer**.
   - [ ] `L1001` is hidden, `L_all` and `L1012` shown; all objects stay visible in the viewport
   - [ ] The new layer reads Tiles `1012`, `Objects: All`
3. Choose `Tile 1011`, then `Head`; then `Head`, then `Tile 1011`; then `All`.
   - [ ] Choosing an object clears the tile filter, choosing a tile clears the object filter; `All` shows every layer
4. Collapse the sidebar to mini mode (if available), right-click a layer.
   - [ ] The context menu has an `Objects` and a `Tiles` sub-menu with the same entries as the popups
5. Paint `L_all` across every object, then set it to Tiles `1002`. On tile 1001 use Eraser, Clone, Blur, Decal and Text in turn.
   - [ ] Nothing changes outside tile 1002 with any of the five tools
6. Apply a decal on `Body`'s 1002 faces.
   - [ ] The decal paints only on the covered tile
7. Open a material in the Materials tab.
   - [ ] The preview shows the material normally (not cut by any tile restriction)
Notes: ______

### 3.8 Export odd cases: exr, jpg, shortcut, pack, mode fallback, 1002-only reopen
Covers: FULL plan ids 10.16, 10.19, 10.20, 10.21, 10.22, 12.13
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto), add a fill layer, File > Save As... `exo.arm`.
2. Export Textures `Per Tile` with Color `16bit` (exr), then with format `jpg` `8bit`.
   - [ ] Names read `exo_base_1001.exr` and `exo_base_1001.jpg` (tile suffix before the extension), sizes as for png
3. Use the export shortcut (File > Export > Textures As... if it exists, or the keymap's file_export_textures_as).
   - [ ] The output names carry `_1001` etc.
4. Destination `Pack into Project`, `Per Tile`, export.
   - [ ] Textures with tile suffixes (`exo_base_1001` ...) are added to the Textures tab, no crash
5. With `Per Tile` selected, File > New (default cube), open the Export Textures box.
   - [ ] The mode falls back to `Visible`
6. Fresh import `udim_single_1002.obj` (Auto), File > Save As... `c1002.arm`, File > New, open `c1002.arm`, export `Per Tile`.
   - [ ] The `Tiles` panel still shows `1002` and the export writes `_1002` files
Notes: ______

### 3.9 Save and append odd cases: duplicates, mesh-only .arm, 16bit, old formats
Covers: FULL plan ids 4.16, 4.20, 12.7, 12.8, 12.9, 12.10, 12.11, 12.14, 12.17, 13.15
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto). Meshes tab: right-click `Arm` > Duplicate, paint on the copy; duplicate `Body`, paint on the copy. File > Save As... `dup.arm`, File > New, open `dup.arm`.
   - [ ] The `Arm` copy reads `Tiles: 1012`, the `Body` copy `Tiles: 1001-1002`; both paint on the right tiles with the same layout as before saving
2. File > Export > Mesh..., format `.arm`, to `udim_mesh.arm`. File > New (default cube), export its mesh as `.arm` to `cube_mesh.arm`.
3. Open `dup.arm`, File > Import > Mesh... `udim_mesh.arm`. Then File > New, import `udim_mesh.arm` again.
   - [ ] In both projects the imported meshes go to tile 1001, no crash; in `dup.arm` the existing paint is untouched
4. Fresh import `udim_2x2_multi.obj` (Auto), File > Import > Mesh... `cube_mesh.arm`. Without switching layers paint on the imported cube and on `Head`.
   - [ ] The cube lands in tile 1001 (2D view slot `1001`); strokes appear under the cursor on both objects, never in another slot
5. Fresh import `udim_2x2_multi.obj` (Auto), Edit menu `Bits` `16bit`. Paint strokes, Save As `b16.arm`, File > New, reopen.
   - [ ] Tiles, resolution, buttons, viewport and 2D view are as before saving
6. In `b16.arm` add a mask (paint black) and a second painted layer, screenshot, then `Append` `udim_12_tiles.obj`.
   - [ ] Both layers and the mask keep their pixels on the original tiles; new tiles: layer empty, mask white; roughness/metal unchanged
7. Optional (about 3 GB GPU memory): fresh import `udim_2x2_multi.obj`, Resolution `8192`, `Append` `udim_12_tiles.obj`.
   - [ ] The Console shows `UDIM atlas too large: 12 tiles use 4096 px per tile (16384 px atlas)` and the Edit menu `Clamped to 4096 px per tile (atlas limit 16384)`
8. Optional: open a phase A format "99" file (`p99.arm` from the phase A plan) and an old plain v23 project.
   - [ ] `p99.arm` opens without error, coverage buttons unchanged, tile list `{1001}`, no Tiles button
   - [ ] The v23 project converts as in the phase A plan (section 13), tile list `{1001}`
Notes: ______

### 3.10 Bake Maps odd cases: 1002-only, plain cube, after relayout, CPU memory warning
Covers: FULL plan ids 11.17, 11.18, 11.19, 11.20, 11.21
[ ] PASS  [ ] FAIL
Bake settings as in 2.7 step 1 (AO only, `Name` `bkao`, an empty output folder per step).
1. Start the app. Fresh import `udim_single_1002.obj` (Auto), bake AO.
   - [ ] `bkao_ao.png` and a same-size `bkao_ao_1002.png` are written, no `_1001`
2. File > New (default cube), bake AO.
   - [ ] Only `bkao_ao.png` is written, no tile files; the Baked material's image node works
3. Fresh import `udim_2x2_multi.obj` (Auto), bake AO, close the bake window, `Append` `udim_12_tiles.obj`, bake again into the same folder.
   - [ ] The bake uses the new layout (12 tiles, stride 4) and writes `_1001`..`_1006`, `_1011`..`_1016`
   - [ ] Older tile files in the folder are not deleted
4. In the same project, Edit menu Resolution `2048`. Settings > `Raytracing` `Bake on` `CPU`, `Resolution` `4096` (atlas 16384), `Bake`, press `Stop` after the message.
   - [ ] The Console shows `CPU bake of a 16384 px atlas needs a lot of memory, consider the GPU bake or a lower resolution` before the bake starts
5. `Resolution` `2048` (atlas 8192), `CPU`, `Bake`, then `Stop`.
   - [ ] No memory warning is shown
Notes: ______

### 3.11 Bake Texture node odd cases: occlusion, reopen, 1002-only, plain cube, previews
Covers: FULL plan ids 17.5, 17.10, 17.11, 17.12, 17.13, 17.14, 17.15, 17.16, 17.17, 17.18
[ ] PASS  [ ] FAIL
1. Start the app. Fresh import `udim_2x2_multi.obj` (Auto), Resolution `2048`. Build the node graph of 0.5 with `TexCoord`, bake, add the fill layer.
   - [ ] The material thumbnail shows the atlas wrapped on the preview ball (previews sample raw UVs)
2. Materials tab: right-click the material thumbnail, `Bake`, export to an empty folder.
   - [ ] The base color map shows the bake atlas once (as the node preview), not one shrunken copy per slot; names and sizes: ______
3. Optional (if `Occlusion` is in the `Bake` combo): `Occlusion`, `Samples` `64`, bake to 100%.
   - [ ] AO sits on each object as expected on all four tiles, each slot holds only its own islands, no repeats
4. Open the Console tab.
   - [ ] No shader error after any step so far
5. Set `TexCoord` again and bake. File > Save As... `bn.arm`, quit, start again, open `bn.arm`. Look at the fill layer, then click `Bake` on the node.
   - [ ] Before baking the node gives black (the bake target is not saved)
   - [ ] After baking each island is drawn once in its own slot with one smooth gradient over the grid, as in 2.9 step 1
6. Fresh import `udim_single_1002.obj`. Graph with `TexCoord`, bake, fill layer.
   - [ ] The bake fills the whole layer once; `Crate` shows its UV gradient with no repeats
7. File > New (default cube). Graph with `TexCoord`, bake, fill layer; then `Curvature`, bake.
   - [ ] TexCoord: identical to the old build (the cube's gradient once over its layout); Curvature: identical to the old build
8. Right-click the fill layer row, `UV Scale` `2`.
   - [ ] The bake repeats twice, as before
Notes: ______

## Known limits (expected, not failures)

Record any difference you see as a note; these are decided or deferred. Full list in `TEST_PLAN_PHASE_B_FULL.md` section 16.
- Overlapping UVs of different objects inside one tile cannot be told apart by a texture: exports and path tracing can differ from the viewport; only the Objects tooltip warns (3.4).
- Translucency with the path tracer: texels outside a restricted bottom layer's coverage are transparent in path tracing and the opacity map, opaque in the raster view.
- glTF and Blend import have no **UDIM tiles** option and stay on wrap.
- UDIM tiles Off wraps each UV corner on its own, as upstream: tiling UVs crossing whole numbers distort or collapse (`udim_tiling_uv.obj` shows no repeating pattern).
- Per Object iterates only objects covered by an OBJECT-RESTRICTED layer; unrestricted layers in each Per Object file are masked to that object's footprint. `Per Tile` writes every used tile even if empty.
- The Bake Maps `Resolution` combo is per tile but does not say so; tile files of removed tiles are not deleted from the bake folder; CPU bake of a 16k atlas needs about 2 GB (warning only); the raytraced lightmap bake squeezes the whole atlas into the script resolution.
- `UV: Atlas` image nodes and the Bake Texture node in a fill layer follow that layer's UV scale and angle (scale 1 and angle 0 are correct); bake with scale 1 and angle 0.
- The atlas clamp is not remembered across save (a clamped project reopens with the clamped per-tile size).
- Blender FBX files appended with `Append` come in 100 times larger (cm, node scale 100); a fresh import rescales to fit. Same as upstream.
- Crop is blocked with more than one tile; the atlas side is capped at 16384 (about 64 tiles at 2k, 16 at 4k).
- Mesh delete has no undo; mesh nodes are positional; material/brush previews and material bake export ignore tiles; hidden objects still count for footprints; object names in export file names are not sanitized.

## Watch for (riskiest spots)

- Relayout (2.1, 2.3, 1.7, 3.9 step 6): other layers' pixels, masks white in new slots, sculpt textures, 16bit, stale merged mesh or UV caches, undo history really empty.
- Atlas slot borders (1.10 step 4, 3.2): no one-pixel shift in tile crops, no bleed into the neighbouring slot, border vertices resolving to the right slot.
- Import tile-list rule (1.6, 1.7, 3.6): a replacing import sets the tile list to exactly the new meshes' tiles; an append only adds.
- FBX (2.12): names with suffixes, sensible size, straddle count.
- Non-power-of-two atlases (2.2 step 5, 2.8): 16383 vs 16384, 5461 px per tile, crops at the per-tile size.
- Never run before the final review fixes: sculpt across save and reopen (3.1), painting right after a mesh-only .arm import (3.9 step 4), vertex-colour footprints (3.3 step 4).
