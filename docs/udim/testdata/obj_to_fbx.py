# Blender script: blender -b --factory-startup --python obj_to_fbx.py -- in.obj out.fbx
import sys, bpy
argv = sys.argv[sys.argv.index("--")+1:]
src, dst = argv[0], argv[1]
bpy.ops.wm.read_factory_settings(use_empty=True)
for o in list(bpy.data.objects): bpy.data.objects.remove(o, do_unlink=True)
if hasattr(bpy.ops.wm, "obj_import"): bpy.ops.wm.obj_import(filepath=src)
else: bpy.ops.import_scene.obj(filepath=src)
bpy.ops.export_scene.fbx(filepath=dst, use_selection=False)
print("exported", dst, [o.name for o in bpy.data.objects])
