# UDIM phase A: manual test plan

Branch `feat/udim-object-tile-selection`, phase A (steps A1 persistent object ids, A2 layer coverage sets + save format "99", A3 Export > Atlases removed).
The first GUI test round fed back into the fixes of 2026-11-13 (converter, popup greying, skipped strokes, separate-UV mesh). Every step below is still to be checked by hand on the current tip.

On macOS, ctrl in shortcuts means cmd. "Layer row" means a row in the **Layers** tab. Every layer and group row has a button to the right of its name, the **Objects button**. It reads `Objects: All`, an object name, `Name1, Name2`, `N objects` or `No objects`. Clicking it opens a popup with an `All` check, a separator, and one check per object. The popup stays open while you toggle.

## 0. Build and test data

0.1 Build: `cd paint && ../base/make`, then `cd build && xcodebuild -project ArmorPaint.xcodeproj -scheme ArmorPaint -configuration Release build`. Run `ArmorPaint.app` from the build products folder.

0.2 Test files in `docs/udim/testdata/`:
- `three_cubes_separate_uv.obj`: three cube objects `A`, `B`, `C` side by side, in disjoint quadrants of tile 1001 (A top left u 0..0.5 v 0.5..1, B top right, C bottom left). Use this mesh for every restriction, paint and export test.
- `three_cubes_overlapping_uv.obj` (was `three_cubes.obj`): the same cubes, but each uses the whole 0-1 tile, so all three overlap in UV space. Only used to show the documented overlap limit (section 17).
- `udim_body_two_tiles.obj`: one object `Body`. The left cube's UVs are in tile 1001 (u 0..1) and the right cube's in tile 1002 (u 1..2).
- Regenerate them with `python3 docs/udim/testdata/gen_obj.py docs/udim/testdata`. Any OBJ with several `o` objects also works for A, B and C.

0.3 Old project (format "23"). Make one with an upstream build:
- `git worktree add ../ap-main 22097dfdd`.
- Build it the same way.
- File > Import > Mesh... > `three_cubes_separate_uv.obj`, set **Split By** = `Object`, click **Import**.
- Add 2 more paint layers (3 in total) and paint a stroke across all three cubes on each.
- In the layer row combo, set `Layer 2` to object `B`.
- File > Export > Textures... > tab `Atlases`: set C to `Atlas 2` (A and B stay in `Atlas 1`), close the box.
- In the layer row combo, set `Layer 3` to `Atlas 1` (listed after the objects).
- Add a **New > Group** and drag `Layer 1` into it. Add a mask to `Layer 2`.
- File > Save As... `v23.arm`.
- Optional, for 13.3: a second project with `udim_body_two_tiles.obj` imported with **Split By** `UDIM Tile`, a layer set to `Body.1002` and painted, saved as `v23_udim.arm`.
- Remove the worktree afterwards.

## 1. Non-UDIM regression (upstream behaviour unchanged)

1.1 Start the app (default cube). Paint a stroke with the Brush tool, add a **New > Fill Layer**, add a mask, then File > Export > Textures... (Layers `Visible`), File > Save As..., File > Open... the same file.
- Expected: everything works as on upstream main. The export writes the usual files. Reload shows the same layers and paint. No console errors.

1.2 Switch the viewport to path tracing and back.
- Expected: same look as on upstream.

PASS if 1.1 and 1.2 behave like upstream main.

## 2. Persistent object ids (A1)

2.1 File > Import > Mesh... > `three_cubes_separate_uv.obj`, **Split By** `Object`, **Import**.
- Expected: the Meshes tab shows A, B, C.

2.2 Layers tab: the default layer is `Layer 1`. Click its Objects button, untick `All`, tick `B`, then close the popup.
- Expected: the button reads `B`. Only cube B shows the layer's paint/fill in the viewport (paint a stroke across all three cubes to check).

2.3 Meshes tab: drag C above A (reorder), and use the Meshes tab **Edit** > **Sort**.
- Expected: the layer still applies only to B (the button reads `B`).

2.4 Right-click A > **Duplicate**.
- Expected: a copy appears. The layer button still reads `B`, and the copy gets no paint from the layer.

2.5 Right-click the copy > **Delete**.
- Expected: still `B`, and B keeps its paint.

2.6 Layer still set to `{B}`. File > **Reimport Mesh** (or File > Import > Mesh... with Replace Existing and layers kept) of the same `three_cubes_separate_uv.obj`.
- Expected: the button still reads `B` and the layer still applies to cube B only. A new mesh takes over the id of the replaced object with the same name (or, when a single object is replaced, the first new mesh takes over its id).

PASS if coverage follows B through reorder, sort, duplicate, delete and reimport.

## 3. Objects multi-select and All

3.1 With `three_cubes_separate_uv.obj` loaded, add **New > Paint Layer** (`Layer 2`). Objects popup: untick `All`, tick `A` and `C`.
- Expected: the button reads `A, C`. A brush stroke dragged across all three cubes paints only A and C.

3.2 Tick `All` again.
- Expected: the button reads `Objects: All`, and new strokes paint all three cubes.
- In the popup every object row (A, B, C) shows ticked and greyed out. Clicking a greyed row does nothing and adds no undo step.

3.3 Untick `All` again.
- Expected: the button reads `A, C` again (the list was kept). The rows are enabled again and show A and C ticked, B unticked.

3.4 Tick `B` as well.
- Expected: `3 objects`.

3.5 Hover the Objects button.
- Expected: tooltip `Limit this layer to the selected objects.`

PASS if all five match.

## 4. Painting and fill restricted to the set

4.1 Layer `{A, C}` active, **Fill** tool, **Fill Mode** `Object`, click on B.
- Expected: A and C fill, B stays unchanged.

4.2 **Fill Mode** `Mesh`, click on B.
- Expected: nothing is filled. Click A: only A fills.

4.3 New **Fill Layer**, set its objects to `{B}`.
- Expected: the fill shows only on B right away (the layer refills).

4.4 No undo step for strokes that start on an uncovered object. Layer `{A, C}` active, **Brush** tool. Note the Edit menu's Undo entry. Start a stroke on B and drag it onto A and C.
- Expected: nothing is painted at all (the whole stroke is skipped, also where it crosses A and C), and the Undo entry is unchanged.

4.5 Start a stroke on A and drag it across B onto C.
- Expected: A and C are painted, B is not, and exactly one undo step is added.

4.6 Start a stroke in the empty background and drag it onto A.
- Expected: A is painted and one undo step is added (strokes that start off the mesh behave as before).

4.7 The same check with **Eraser**, **Clone**, **Blur**, **Decal** and **Text** (start on B: nothing happens, no undo step). **Fill** is not affected: 4.1 still fills A and C when clicking B.

4.8 Layer `Objects: All`: start a stroke on B.
- Expected: it paints and adds an undo step as before.

4.9 Layer `{A, C}`: open the 2D view (shift+tab, or the `2D View` button at the top of the Layers tab) and paint a stroke in it.
- Expected: painting works as before, no stroke is skipped.

PASS if paint and fill never reach uncovered objects and 4.4-4.9 match.

## 5. Group layers (decision D1)

5.1 **New > Group**. Drag `Layer 2` (set to `{A, C}`) into the group.
- Expected: the group row has its own Objects button reading `Objects: All`.

5.2 Group Objects popup: untick `All`, tick `C` only.
- Expected: Layer 2 now shows only on C (intersection of {A, C} and {C}).
- Open Layer 2's Objects popup: A and B are greyed out and unticked, hovering them shows `Excluded by the group`. C is enabled and ticked.
- A brush stroke on Layer 2 that starts on A is skipped with no undo step (4.4 with the group intersection).

5.2b Set the group back to `All`, then open Layer 2's popup.
- Expected: nothing is greyed, and A and C are ticked again (Layer 2's own list was not changed by the greying). A group with `All` excludes nothing.

5.3 Set the group to `A, B`.
- Expected: Layer 2 shows only on A.

5.4 Set the group to `B` only.
- Expected: Layer 2 covers nothing. Its name is dimmed with the `No objects are selected...` tooltip from 6.4. A stroke paints nothing.

5.5 Masks inside Layer 2 have no Objects button (they inherit from Layer 2).

5.6 Tiles button: with a non-UDIM project there is only tile 1001, so no Tiles button is shown on layers or groups. This is expected in phase A.

PASS if 5.1-5.6 match.

## 6. Layer whose objects are all deleted

6.1 Layer set to `{B}` only. Meshes tab: right-click B > **Delete**.
- Expected: the button reads `No objects` and the layer name is dimmed.
- Hovering the name shows `This layer's objects were deleted, so it covers nothing. Select objects or choose All to use it again.`
- The layer's paint is not drawn on any object.

6.2 Try a brush stroke with that layer active.
- Expected: nothing is painted. The Console tab title shows `Layer covers no objects` for a few seconds.

6.3 Tick `All` in its Objects popup.
- Expected: the layer applies to every object again, and the name is no longer dimmed.

6.4 New paint layer. Objects popup: untick `All` while nothing is ticked in the list.
- Expected: the button reads `No objects`, the name is dimmed, and hovering it shows `No objects are selected for this layer or its group, so it covers nothing. Select objects or choose All to use it.` (not the "were deleted" text). The same text shows for a layer whose set and its group's set have nothing in common (5.4).

6.5 Layer set to `{A}`. Hide A with its eye in the Meshes tab, then try a brush stroke.
- Expected: nothing is painted, the name is not dimmed, and the Console tab title shows `Layer's objects are hidden or use another material`. The same message shows when A has a material set in the Meshes tab instead of being hidden, and no other object is painted.

PASS if 6.1-6.5 match.

## 7. Layer filter

7.1 Layers tab **Filter** combo (`All` by default): choose `A`.
- Expected: only A stays visible in the viewport. Layers that don't cover A are hidden in the list.

7.2 With the filter on A, **New > Paint Layer**.
- Expected: its Objects button reads `A`.

7.3 Filter back to `All`.
- Expected: all objects are visible again.

PASS if all match.

## 8. Join and split keep coverage

8.1 Layer `{A}`. Meshes tab: select A and B (shift-click), right-click > **Merge Geometry**.
- Expected: the joined mesh is covered by the layer (union rule).

8.2 Right-click the joined mesh > **Split**.
- Expected: both parts stay covered by layers that covered the joined mesh.

PASS if coverage survives join and split.

## 9. Undo and redo of coverage changes

9.1 Change a layer from `Objects: All` to `B`, then press ctrl+z.
- Expected: the menu shows `Undo Layer Object`, and the layer returns to `Objects: All`.

9.2 ctrl+shift+z (Redo).
- Expected: back to `B`.

9.3 Each tick or untick in the popup is one undo step. The same holds for a group's Objects popup.

PASS if undo and redo restore the exact sets.

## 10. Per Object export

10.1 Three cubes, Layer 1 `Objects: All`, Layer 2 `{A}`, Layer 3 `{B, C}`, each painted a different colour. File > Export > Textures..., **Layers** `Per Object`, export.
- Expected: one texture set per object that some restricted layer covers (A, B, C), with `_A_`, `_B_`, `_C_` in the file names.
- A's files contain Layer 1 + Layer 2, B's contain Layer 1 + Layer 3, and C's contain Layer 1 + Layer 3.

10.2 **Layers** `Visible`.
- Expected: one set with no object suffix.

Known limitation: object-restricted layers are not masked to the object's UV footprint in exports yet (phase B4). Where objects overlap in UV space, the output contains every included layer.

PASS if the file sets and the layers per set match.

## 11. Export > Atlases removed

11.1 File > Export > Textures...
- Expected: the tabs are `Export Textures` and `Presets` only (plus `Export Mesh` on Android/iOS). There is no `Atlases` tab.

11.2 Layers tab **Edit** menu.
- Expected: no `Atlases` entry.

PASS if no Atlases UI remains.

## 12. Save format "99" round trip

12.1 Set up three cubes, Layer 1 `All`, Layer 2 `{A, C}`, a group `{C}` containing Layer 3 `{A, C}`, and one layer `{B}` with B then deleted (`No objects`). File > Save As... `p99.arm`, close, File > Open... `p99.arm`.
- Expected: every Objects button shows the same value as before saving, Layer 3 still shows only on C, and the `No objects` layer is still dimmed.

12.2 After reopening, File > Import > Mesh... with **Append** (from the Import Mesh box menu) to add an object, then Duplicate one.
- Expected: new objects get fresh ids. No existing layer starts covering them.

12.3 Optional check of the format version: run `head -c 32 p99.arm | xxd`. Bytes 22-23 read `99`.

12.4 Upstream main opening `p99.arm`.
- Expected: upstream main as of 22097dfdd crashes on an unknown version (pre-existing upstream bug). With branch `fix/import-arm-unknown-version` it refuses the file with a console error and no crash. The fork has the same fix, so a future upstream format opened in the fork fails the same way.

PASS if 12.1-12.3 hold (12.4 optional).

## 13. Old v23 project (converter)

13.1 File > Open... `v23.arm` from 0.3.
- Expected: it opens without a crash. `Layer 1` (in the group) reads `Objects: All`, `Layer 2` reads `B`, `Layer 3` reads `A, B` (the objects of `Atlas 1`). The group reads `Objects: All`, and Layer 2's mask has no Objects button (it follows Layer 2).
- Paint looks the same as in the upstream build: Layer 2 only on B, Layer 3 only on A and B.

13.2 Save it as `v23_resaved.arm` and reopen.
- Expected: it loads as format "99" with the same Objects buttons.

13.3 Optional: File > Open... `v23_udim.arm` from 0.3.
- Expected: no crash. Objects `Body.1001` and `Body.1002`, the layer reads `Body.1002` and its paint stays on that tile as on upstream. Tiles are not migrated (the project still has only tile 1001 and no Tiles button); name-based UDIM to tiles migration is phase B (B7).

13.4 Rules (for reference): object mask 0 converts to All; a mask naming one object converts to that object; a mask naming an atlas group converts to the objects of that group; a group that can not be resolved converts to All. Masks, filters and groups convert to All, because upstream only applied a paint layer's own mask.

PASS if 13.1-13.2 match (13.3 optional).

## 14. Name-based UDIM (kept working as far as practical)

14.1 File > Import > Mesh... > `udim_body_two_tiles.obj`, **Split By** `UDIM Tile`, **Import**.
- Expected: objects `Body.1001` and `Body.1002`. A shared layer paints across both. Export **Layers** `Per Udim Tile` writes `.1001` / `.1002` files as on upstream.

14.2 Layer limited to `Body.1002` only: paint on it.
- Expected: it behaves as upstream's single-tile-object layer.

14.3 A layer is read tile-local only when its stored object list names exactly one tile object. Layer `L2` limited to `{Body.1001, Body.1002}`, paint a stroke across both. Delete `Body.1002`.
- Expected: L2's paint on Body.1001 stays where it was (atlas reading kept, not squeezed into 0-1). Before the fix this flipped to tile-local.

14.4 Layer limited to `Body.1001` only, painted. Import (Append) another mesh, and Duplicate an unrelated object.
- Expected: the layer's paint on Body.1001 does not move. Merge Geometry or Split of Body.1001 leaves the layer listing only Body.1001 (it is not extended to the joined or split objects), so the paint never changes reading; split parts are not covered by it, and a merge that removes Body.1001 leaves the layer covering nothing.

PASS if behaviour matches upstream main for 14.1-14.2 and the paint never changes reading in 14.3-14.4.

## 15. Sculpt coverage

15.1 Three cubes, **New > Sculpt Layer** (sculpt tools mode). Set its objects to `{A, C}` and sculpt across all three.
- Expected: B does not deform.

15.2 Change the set to `{B}`.
- Expected: only B deforms.

15.3 Set it to `All`.
- Expected: everything deforms. Also try a grab or inflate brush.

15.4 Set it to `{B}` and delete B (or untick All with nothing ticked), then sculpt.
- Expected: nothing deforms, the Console tab title shows `Layer covers no objects`, and no undo step is added (Edit menu's Undo entry is unchanged).

PASS if deformation follows the set.

## 16. Expected limitations in phase A (not failures)

- Tile selection is inert. The project always has the single tile 1001, the Tiles button is hidden, and the filter has no tile entries.
- No per-tile resolution, no UDIM tile extraction at import (only the old `Split By: UDIM Tile` name-based objects), no atlas relayout.
- Exports and path tracing don't mask object-restricted layers to their UV footprint (phase B4). The viewport is exact per object.
- The overlap tooltip sentence ("X shares UV space with Y...") is not implemented.
- Mesh delete has no undo (deferred). Deleted objects leave layers covering nothing.
- Selecting a layer whose painted set is unchanged reuses the merged mesh instead of rebuilding it (the merged mesh is rebuilt after any mesh, name or material change). Watch for stale geometry when painting right after a mesh edit and a layer switch.

## 17. Overlapping UVs (documented limit)

17.1 File > Import > Mesh... > `three_cubes_overlapping_uv.obj`, **Split By** `Object`. Layer 1 `Objects: All`, paint a stroke on A only.
- Expected: the same paint also shows on B and C, at the same place of each cube, because all three share the same UV space. This is the limit, not a bug.

17.2 New paint layer limited to `{A}`, paint a stroke on A.
- Expected: in the viewport only A shows the new stroke. In exports and path tracing it also shows on B and C (they share the texels). Use `three_cubes_separate_uv.obj` for every other test.

## Watch for (review findings not fixed in phase A)

- m7: with the Objects popup open, a shortcut (for example ctrl+z undoing "new layer" while the mouse is over the header) can leave the popup pointing at a layer that no longer exists. The next toggle would act on it. Report any crash or odd undo after using shortcuts with the popup open.
- m8: the sculpt shader always binds the coverage texture, even in single-object projects. Brush or material graphs close to the texture slot limit could fail to compile. Report any sculpt shader compile error.
- m9: Reimport Mesh keeps an object's id only when a new mesh has the same name (or a single object is replaced). A restricted layer whose objects all got new names covers nothing afterwards, with no warning beyond the dimmed name.

## Result

Record PASS or FAIL per section 1-15 and 17. Phase A passes when every section passes. Any crash, lost paint after save/reload, or coverage reaching an uncovered object is a FAIL.
