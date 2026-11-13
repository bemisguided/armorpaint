# UDIM work: deferred items

Things deliberately left out of the `feat/udim-object-tile-selection` branch. Come back to these once the core lands.

## Deferred by decision
- **Mesh-delete undo.** Main has none. With persistent object ids, restoring a deleted object with its old id would bring layer coverage back automatically. Until then, deleting an object leaves layers that covered it covering nothing. Needs a snapshot of the object, its mesh data, material override and hierarchy position in the history system.
- **Export groups.** Export > Atlases is removed with no replacement. A later id-based "Export groups" list (name to object ids) could bring back "several objects in one named export set" for plain 0-1 meshes, and fix the old tab's positional, unrenamable groups.
- **Import UDIM texture sets** (`foo_1001.png` ...) as one atlas asset for image nodes. Builds on the "UV: Atlas" image node option. Detect the sibling files, stitch them using the project tile layout, and show the result in the Textures list. Allows using sets painted in other apps.

## Deferred by scope
- **glTF and Blender import** of UDIM tiles. First pass keeps them on the wrap (Off) behaviour. Later they use the shared tile helper, with vertex splitting for indexed meshes.
- **CPU bake memory.** A 16k atlas needs about 2 GB in the Bake Maps CPU path. Warn above 8k now; later bake per tile (rasterize one tile's triangles with local UV, keep the BVH global).
- **Overlapping UVs between objects in one tile.** A texture can't tell them apart, so exports and path tracing can differ from the viewport. Only the layer tooltip warns. Fixing it needs per-object texture sets.
- **More than the single-atlas budget.** Beyond about 64 tiles at 2k, or 16 at 4k, the single atlas stops being practical. Tile streaming or sparse tiles would replace the storage layer only; the layer data model stays.
- **Migration of old projects.** Version "99" files only; v23 files load through a minimal converter that makes every layer Shared. Name-based UDIM projects and atlas groups are not converted, and the "Join UDIM tile objects" action is not built.
- **Mesh-nodes object index** is positional (`mesh_nodes_objects`). See the open decisions in DECISIONS.md.

## Undecided (see DECISIONS.md)
- D4 import of texture sets is listed above as deferred
- D5 translucency with the path tracer
- D6 convert the mesh-nodes object index to ids
- D7 default per-tile resolution (2k suggested)

## Reusable from `archive/custom-name-based-udim`
- Resolution helpers and call-site swaps, and the native-size export crop (`8a18795bc`). Not its layer rescale.
- Bake plugin tile crop and import helper (`abde6fde8`).
- `_1001` naming (`3cae084b8`).
- The tile-group and splitting commits are not reusable as code, only as a checklist of touch points.
