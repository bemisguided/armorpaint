import sys, math
# Phase B UDIM test meshes. Usage: python3 gen_phase_b.py <outdir>
# UDIM tile id = 1001 + floor(u) + 10*floor(v)
FACES = [(0,1,2,3),(5,4,7,6),(4,0,3,7),(1,5,6,2),(3,2,6,7),(4,5,1,0)]
CORNERS = [(-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1),(-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1)]
M = 0.02

def grid(region, cols, rows, m=M):
    """Cell rects (u0,v0,u1,v1) of a cols x rows grid inside region, row-major from bottom-left."""
    ru0, rv0, ru1, rv1 = region
    w, h = (ru1-ru0)/cols, (rv1-rv0)/rows
    return [(ru0+c*w+m, rv0+r*h+m, ru0+(c+1)*w-m, rv0+(r+1)*h-m)
            for r in range(rows) for c in range(cols)]

class Obj:
    def __init__(self, header):
        self.lines = list(header); self.nv = 0; self.nt = 0
    def add(self, name, pos, faces):
        """faces: list of lists of (pos_index, (u,v)); each corner gets its own vt unless a vt index is given as 3rd item."""
        self.lines.append(f"o {name}")
        for p in pos: self.lines.append("v %.4f %.4f %.4f" % p)
        vb = self.nv
        for f in faces:
            refs = []
            for pi, uv in f:
                self.nt += 1
                self.lines.append("vt %.4f %.4f" % uv)
                refs.append(f"{vb+pi+1}/{self.nt}")
            self.lines.append("f " + " ".join(refs))
        self.nv += len(pos)
    def cube(self, name, c, size, rects):
        pos = [(c[0]+x*size, c[1]+y*size, c[2]+z*size) for x,y,z in CORNERS]
        faces = []
        for fi, q in enumerate(FACES):
            u0,v0,u1,v1 = rects[fi]
            faces.append(list(zip(q, [(u0,v0),(u1,v0),(u1,v1),(u0,v1)])))
        self.add(name, pos, faces)
    def plane(self, name, c, size, rect):
        u0,v0,u1,v1 = rect
        pos = [(c[0]-size,c[1],c[2]-size),(c[0]+size,c[1],c[2]-size),(c[0]+size,c[1],c[2]+size),(c[0]-size,c[1],c[2]+size)]
        self.add(name, pos, [list(zip(range(4), [(u0,v0),(u1,v0),(u1,v1),(u0,v1)]))])
    def save(self, path):
        open(path, "w").write("\n".join(self.lines) + "\n")

d = sys.argv[1]

# 1. 2x2 multi
o = Obj(["# udim_2x2_multi: tiles 1001,1002,1011,1012. Body (cube x=-4.5): 3 faces in 1001 bottom half (v 0.02..0.48, 3 cells in a row), 3 faces in 1002 (3x1 cells over v 0.02..0.98), spans 1001+1002. "
         "Head (x=-1.5): 3x2 grid in tile 1011. Arm (x=1.5): 2x3 grid in tile 1012 left half (u 1.02..1.48). Leg (x=4.5): 1x6 column stack in tile 1012 right half (u 1.52..1.98). "
         "Prop (x=7.5): 3x2 grid in tile 1001 top half (v 0.52..0.98). Objects -> tiles: Body 1001+1002, Head 1011, Arm 1012, Leg 1012, Prop 1001"])
body = grid((0,0,1,0.5),3,1) + grid((1,0,2,1),3,1)
o.cube("Body",(-4.5,0,0),0.8,body)
o.cube("Head",(-1.5,0,0),0.8,grid((0,1,1,2),3,2))
o.cube("Arm",(1.5,0,0),0.8,grid((1,1,1.5,2),2,3))
o.cube("Leg",(4.5,0,0),0.8,grid((1.5,1,2,2),1,6))
o.cube("Prop",(7.5,0,0),0.8,grid((0,0.5,1,1),3,2))
o.save(f"{d}/udim_2x2_multi.obj")

# 2. straddle
o = Obj(["# udim_straddle: object Strip, flat strip along x. 4 quads clean (2 in tile 1001 at u 0.02..0.85, 2 in tile 1002 at u 1.15..1.98) plus exactly 1 triangle face (the straddler) "
         "with UVs (0.9,0.1),(1.1,0.1),(0.95,0.9) crossing the u=1 border (centroid in 1001). Straddling triangles after quad split: 1"])
S = []
def sq(x0,x1,u0,u1): return [(0,(u0,.1)),(1,(u1,.1)),(2,(u1,.9)),(3,(u0,.9))], (x0,x1)
pos = [(x,y,0) for x in range(6) for y in (0,1)]  # column x: idx 2x (y0), 2x+1 (y1)
def quad(x,uv0,uv1): return [(2*x,(uv0,.1)),(2*(x+1),(uv1,.1)),(2*(x+1)+1,(uv1,.9)),(2*x+1,(uv0,.9))]
faces = [quad(0,0.02,0.5), quad(1,0.5,0.85),
         [(4,(0.9,.1)),(6,(1.1,.1)),(7,(0.95,.9))],
         quad(3,1.15,1.5), quad(4,1.5,1.98)]
# column 2->3 is the triangle; quad(3) starts at column 3 so a gap-free strip: triangle uses columns 2,3 only
o.add("Strip", [(x*1.0,y*1.0,0.0) for x,y,_ in pos], faces)
o.save(f"{d}/udim_straddle.obj")

# 3. tiling
o = Obj(["# udim_tiling_uv: object Floor, 2x2 quad grid plane (4 quads), UVs 0..4 across (each quad spans 2x2 UV units, crossing integer borders) meant to REPEAT a tiling texture; "
         "every quad straddles tile borders (8 straddling triangles), so Auto UDIM detection must decide Off"])
pos = [(x*2.0-2,0,z*2.0-2) for z in range(3) for x in range(3)]
faces = []
for r in range(2):
    for c in range(2):
        i0 = r*3+c
        faces.append([(i0,(c*2,r*2)),(i0+1,((c+1)*2,r*2)),(i0+4,((c+1)*2,(r+1)*2)),(i0+3,(c*2,(r+1)*2))])
o.add("Floor", pos, faces)
o.save(f"{d}/udim_tiling_uv.obj")

# 4. single 1002
o = Obj(["# udim_single_1002: object Crate (cube), 3x2 grid of faces entirely in UDIM tile 1002 (u 1.02..1.98, v 0.02..0.98). No straddling"])
o.cube("Crate",(0,0,0),0.8,grid((1,0,2,1),3,2))
o.save(f"{d}/udim_single_1002.obj")

# 5. 12 tiles
o = Obj(["# udim_12_tiles: 12 cube objects T1001..T1006 and T1011..T1016, each entirely in its own tile (3x2 face grid, 0.02 margins), u 0..6, v 0..2. "
         "In 3D laid out as a 6x2 grid: x = 2.5*(column), y = 2.5*(row); row 0 = tiles 1001-1006, row 1 = tiles 1011-1016"])
for j in range(2):
    for i in range(6):
        o.cube(f"T{1001+i+10*j}",(i*2.5,j*2.5,0),0.8,grid((i,j,i+1,j+1),3,2))
o.save(f"{d}/udim_12_tiles.obj")

# 6. seam shared vertex
o = Obj(["# udim_seam_shared_vertex: object Seam, two quads sharing an edge in 3D (shared v indices 2,3). Left quad UVs in tile 1001 (u 0.02..0.98), right quad in tile 1002 (u 1.02..1.98); "
         "the shared edge uses SEPARATE vt indices per side (u=0.98 left, u=1.02 right). A variant sharing both v and vt indices across the tile border is impossible without straddling, so it is not included"])
pos = [(0,0,0),(0,1,0),(1,0,0),(1,1,0)]
pos = [(-2,0,0),(0,0,0),(0,1,0),(-2,1,0),(2,0,0),(2,1,0)]
faces = [[(0,(0.02,.1)),(1,(0.98,.1)),(2,(0.98,.9)),(3,(0.02,.9))],
         [(1,(1.02,.1)),(4,(1.98,.1)),(5,(1.98,.9)),(2,(1.02,.9))]]
o.add("Seam", pos, faces)
o.save(f"{d}/udim_seam_shared_vertex.obj")
