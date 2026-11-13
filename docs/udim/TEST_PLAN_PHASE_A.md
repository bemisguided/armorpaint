# UDIM phase A: manual test plan

Branch `feat/udim-object-tile-selection`, phase A (steps A1 persistent object ids, A2 layer coverage sets + save format "99", A3 Export > Atlases removed).
Nothing in phase A has been run in the GUI yet. Every step below is still to be checked by hand.

On macOS, ctrl in shortcuts means cmd. "Layer row" means a row in the **Layers** tab. Every layer and group row has a button to the right of its name, the **Objects button**. It reads `Objects: All`, an object name, `Name1, Name2`, `N objects` or `No objects`. Clicking it opens a popup with an `All` check, a separator, and one check per object. The popup stays open while you toggle.

## 0. Build and test data

0.1 Build: `cd paint && ../base/make`, then `cd build && xcodebuild -project ArmorPaint.xcodeproj -scheme ArmorPaint -configuration Release build`. Run `ArmorPaint.app` from the build products folder.

0.2 Test files in `docs/udim/testdata/`:
- `three_cubes.obj`: three cube objects `A`, `B`, `C` side by side, each with its own 0-1 UVs.
- `udim_body_two_tiles.obj`: one object `Body`. The left cube's UVs are in tile 1001 (u 0..1) and the right cube's in tile 1002 (u 1..2).
- Regenerate them with `python3 docs/udim/testdata/gen_obj.py docs/udim/testdata`. Any OBJ with several `o` objects also works for A, B and C.

0.3 Old project (format "23"). Make one with an upstream build:
- `git worktree add ../ap-main 22097dfdd`.
- Build it the same way.
- File > Import > Mesh... > `three_cubes.obj`, set **Split By** = `Object`, click **Import**.
- Add 2 paint layers and paint a stroke on each.
- In the layer row combo, set the second layer to object `B`.
- Optional: open File > Export > Textures... > tab `Atlases` and put B in "Atlas 2".
- File > Save As... `v23.arm`.
- Remove the worktree afterwards.

## 1. Non-UDIM regression (upstream behaviour unchanged)

1.1 Start the app (default cube). Paint a stroke with the Brush tool, add a **New > Fill Layer**, add a mask, then File > Export > Textures... (Layers `Visible`), File > Save As..., File > Open... the same file.
- Expected: everything works as on upstream main. The export writes the usual files. Reload shows the same layers and paint. No console errors.

1.2 Switch the viewport to path tracing and back.
- Expected: same look as on upstream.

PASS if 1.1 and 1.2 behave like upstream main.

## 2. Persistent object ids (A1)

2.1 File > Import > Mesh... > `three_cubes.obj`, **Split By** `Object`, **Import**.
- Expected: the Meshes tab shows A, B, C.

2.2 Layers tab: the default layer is `Layer 1`. Click its Objects button, untick `All`, tick `B`, then close the popup.
- Expected: the button reads `B`. Only cube B shows the layer's paint/fill in the viewport (paint a stroke across all three cubes to check).

2.3 Meshes tab: drag C above A (reorder), and use the Meshes tab **Edit** > **Sort**.
- Expected: the layer still applies only to B (the button reads `B`).

2.4 Right-click A > **Duplicate**.
- Expected: a copy appears. The layer button still reads `B`, and the copy gets no paint from the layer.

2.5 Right-click the copy > **Delete**.
- Expected: still `B`, and B keeps its paint.

PASS if coverage follows B through reorder, sort, duplicate and delete.

## 3. Objects multi-select and All

3.1 With `three_cubes.obj` loaded, add **New > Paint Layer** (`Layer 2`). Objects popup: untick `All`, tick `A` and `C`.
- Expected: the button reads `A, C`. A brush stroke dragged across all three cubes paints only A and C.

3.2 Tick `All` again.
- Expected: the button reads `Objects: All`, and new strokes paint all three cubes.

3.3 Untick `All` again.
- Expected: the button reads `A, C` again (the list was kept).

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

PASS if paint and fill never reach uncovered objects.

## 5. Group layers (decision D1)

5.1 **New > Group**. Drag `Layer 2` (set to `{A, C}`) into the group.
- Expected: the group row has its own Objects button reading `Objects: All`.

5.2 Group Objects popup: untick `All`, tick `C` only.
- Expected: Layer 2 now shows only on C (intersection of {A, C} and {C}).

5.3 Set the group to `A, B`.
- Expected: Layer 2 shows only on A.

5.4 Set the group to `B` only.
- Expected: Layer 2 covers nothing. Its name is dimmed with the tooltip from step 6. A stroke paints nothing.

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

PASS if 6.1-6.3 match.

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
- Expected: it fails to load cleanly (unknown version), with no crash.

PASS if 12.1-12.3 hold (12.4 optional).

## 13. Old v23 project (converter)

13.1 File > Open... `v23.arm` from 0.3.
- Expected: it opens without a crash, and every layer reads `Objects: All` (object masks and atlas groups are dropped).
- Paint and layers otherwise look the same, except that the layer that was limited to B now shows on all objects.

13.2 Save it as `v23_resaved.arm` and reopen.
- Expected: it loads as format "99".

PASS if it opens, every layer is All, and the resave round-trips.

## 14. Name-based UDIM (kept working as far as practical)

14.1 File > Import > Mesh... > `udim_body_two_tiles.obj`, **Split By** `UDIM Tile`, **Import**.
- Expected: objects `Body.1001` and `Body.1002`. A shared layer paints across both. Export **Layers** `Per Udim Tile` writes `.1001` / `.1002` files as on upstream.

14.2 Layer limited to `Body.1002` only: paint on it.
- Expected: it behaves as upstream's single-tile-object layer.

PASS if behaviour matches upstream main for these steps.

## 15. Sculpt coverage

15.1 Three cubes, **New > Sculpt Layer** (sculpt tools mode). Set its objects to `{A, C}` and sculpt across all three.
- Expected: B does not deform.

15.2 Change the set to `{B}`.
- Expected: only B deforms.

15.3 Set it to `All`.
- Expected: everything deforms. Also try a grab or inflate brush.

PASS if deformation follows the set.

## 16. Expected limitations in phase A (not failures)

- Tile selection is inert. The project always has the single tile 1001, the Tiles button is hidden, and the filter has no tile entries.
- No per-tile resolution, no UDIM tile extraction at import (only the old `Split By: UDIM Tile` name-based objects), no atlas relayout.
- Exports and path tracing don't mask object-restricted layers to their UV footprint (phase B4). The viewport is exact per object.
- The overlap tooltip sentence ("X shares UV space with Y...") is not implemented.
- Mesh delete has no undo (deferred). Deleted objects leave layers covering nothing.
- A sculpt stroke on a layer that covers nothing pushes an empty undo step.

## Result

Record PASS or FAIL per section 1-15. Phase A passes when every section passes. Any crash, lost paint after save/reload, or coverage reaching an uncovered object is a FAIL.
