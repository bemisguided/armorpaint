import sys
# Cube: 6 faces, each face a cell of a 3x2 grid in its UV region, with a small margin
faces = [  # vertex index quads (ccw from outside)
    (0,1,2,3), (5,4,7,6), (4,0,3,7), (1,5,6,2), (3,2,6,7), (4,5,1,0)]
corners = [(-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1),(-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1)]
# region: (u offset, v offset, size), the cube's faces fill that square of UV space
# Lines of one cube as (v, vt, f) lists. An object writes all its v, then all vt, then all f (as Blender does):
# the OBJ importer starts a new object at a "v"/"vt" line that follows faces.
def cube_lines(off, region, state):
    vbase, tbase = state
    ru, rv, rs = region
    vs, vts, fs = [], [], []
    for (x,y,z) in corners:
        vs.append(f"v {x*0.8+off[0]:.4f} {y*0.8+off[1]:.4f} {z*0.8+off[2]:.4f}")
    for fi in range(6):
        cu, cv = fi % 3, fi // 3
        m = 0.02
        u0, v0 = ru + (cu/3 + m)*rs, rv + (cv/2 + m)*rs
        u1, v1 = ru + ((cu+1)/3 - m)*rs, rv + ((cv+1)/2 - m)*rs
        for (u,v) in [(u0,v0),(u1,v0),(u1,v1),(u0,v1)]:
            vts.append(f"vt {u:.4f} {v:.4f}")
    for fi,(a,b,c,d) in enumerate(faces):
        t = tbase + fi*4
        fs.append("f " + " ".join(f"{vbase+i+1}/{t+k+1}" for k,i in enumerate((a,b,c,d))))
    state[0] += 8; state[1] += 24
    return vs, vts, fs

def cube(out, name, off, region, state):
    vs, vts, fs = cube_lines(off, region, state)
    out.append(f"o {name}"); out.extend(vs); out.extend(vts); out.extend(fs)

def write(path, header, parts):
    out = [header]; st = [0,0]
    for p in parts: cube(out, *p, st)
    open(path,"w").write("\n".join(out)+"\n")

d = sys.argv[1]
# Overlapping UVs: all three cubes use the whole tile 1001, so a texture can not tell them apart (documented limit)
write(f"{d}/three_cubes_overlapping_uv.obj", "# Three cube objects A, B, C, each with its own 0-1 UVs (UDIM tile 1001), all three overlap in UV space",
      [("A",(-2.5,0,0),(0,0,1)), ("B",(0,0,0),(0,0,1)), ("C",(2.5,0,0),(0,0,1))])
# Separate UVs: A, B and C use disjoint quadrants of tile 1001 (A top left, B top right, C bottom left)
write(f"{d}/three_cubes_separate_uv.obj", "# Three cube objects A, B, C in disjoint quadrants of UDIM tile 1001: A u 0..0.5 v 0.5..1, B u 0.5..1 v 0.5..1, C u 0..0.5 v 0..0.5",
      [("A",(-2.5,0,0),(0,0.5,0.5)), ("B",(0,0,0),(0.5,0.5,0.5)), ("C",(2.5,0,0),(0,0,0.5))])
# One object whose first cube is in tile 1001 and second cube in tile 1002
out = ["# One object Body: left cube UVs in tile 1001 (u 0..1), right cube UVs in tile 1002 (u 1..2)", "o Body"]; st=[0,0]
l = cube_lines((-1.25,0,0), (0,0,1), st); r = cube_lines((1.25,0,0), (1,0,1), st)
for k in range(3): out.extend(l[k] + r[k])
open(f"{d}/udim_body_two_tiles.obj","w").write("\n".join(out)+"\n")
