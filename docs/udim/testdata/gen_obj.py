import sys
# Cube: 6 faces, each face a cell of a 3x2 grid in its tile, with a small margin
faces = [  # vertex index quads (ccw from outside)
    (0,1,2,3), (5,4,7,6), (4,0,3,7), (1,5,6,2), (3,2,6,7), (4,5,1,0)]
corners = [(-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1),(-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1)]
def cube(out, name, off, tile_u, state, group=True):
    vbase, tbase = state
    if group: out.append(f"o {name}")
    for (x,y,z) in corners:
        out.append(f"v {x*0.8+off[0]:.4f} {y*0.8+off[1]:.4f} {z*0.8+off[2]:.4f}")
    for fi in range(6):
        cu, cv = fi % 3, fi // 3
        m = 0.02
        u0, v0 = tile_u + cu/3 + m, cv/2 + m
        u1, v1 = tile_u + (cu+1)/3 - m, (cv+1)/2 - m
        for (u,v) in [(u0,v0),(u1,v0),(u1,v1),(u0,v1)]:
            out.append(f"vt {u:.4f} {v:.4f}")
    for fi,(a,b,c,d) in enumerate(faces):
        t = tbase + fi*4
        out.append("f " + " ".join(f"{vbase+i+1}/{t+k+1}" for k,i in enumerate((a,b,c,d))))
    state[0] += 8; state[1] += 24

def write(path, header, parts):
    out = [header]; st = [0,0]
    for p in parts: cube(out, *p, st)
    open(path,"w").write("\n".join(out)+"\n")

d = sys.argv[1]
write(f"{d}/three_cubes.obj", "# Three cube objects A, B, C, each with its own 0-1 UVs (UDIM tile 1001)",
      [("A",(-2.5,0,0),0), ("B",(0,0,0),0), ("C",(2.5,0,0),0)])
# One object whose first cube is in tile 1001 and second cube in tile 1002
out = ["# One object Body: left cube UVs in tile 1001 (u 0..1), right cube UVs in tile 1002 (u 1..2)", "o Body"]; st=[0,0]
cube(out, "Body", (-1.25,0,0), 0, st, group=False); cube(out, "Body", (1.25,0,0), 1, st, group=False)
open(f"{d}/udim_body_two_tiles.obj","w").write("\n".join(out)+"\n")
