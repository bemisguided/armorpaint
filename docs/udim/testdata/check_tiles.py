import sys, math
# Usage: python3 check_tiles.py file.obj [...]
# Per object: tile ids of triangles (by UV centroid) and count of triangles with a corner outside the centroid tile by > 1e-4.
EPS = 1e-4
for path in sys.argv[1:]:
    vts = []; objs = {}; cur = None
    for ln in open(path):
        p = ln.split()
        if not p or p[0].startswith("#"): continue
        if p[0] == "o": cur = objs.setdefault(p[1], [])
        elif p[0] == "vt": vts.append((float(p[1]), float(p[2])))
        elif p[0] == "f":
            idx = [vts[int(t.split("/")[1])-1] for t in p[1:]]
            for k in range(1, len(idx)-1): cur.append((idx[0], idx[k], idx[k+1]))
    print(path)
    for name, tris in objs.items():
        tiles = set(); st = 0
        for t in tris:
            cu = sum(c[0] for c in t)/3; cv = sum(c[1] for c in t)/3
            tu, tv = math.floor(cu), math.floor(cv)
            tiles.add(1001+tu+10*tv)
            if any(c[0] < tu-EPS or c[0] > tu+1+EPS or c[1] < tv-EPS or c[1] > tv+1+EPS for c in t): st += 1
        print(f"  {name}: tiles={sorted(tiles)} tris={len(tris)} straddling={st}")
