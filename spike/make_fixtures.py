"""Generate spike fixture STLs (ASCII): flat cube + T-shape overhang model."""
import os

def box_tris(x0, y0, z0, x1, y1, z1):
    v = [(x0,y0,z0),(x1,y0,z0),(x1,y1,z0),(x0,y1,z0),
         (x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1)]
    quads = [(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)]
    tris = []
    for a,b,c,d in quads:
        tris.append((v[a],v[b],v[c]))
        tris.append((v[a],v[c],v[d]))
    return tris

def write_stl(path, tris):
    with open(path, "w") as f:
        f.write("solid fixture\n")
        for t in tris:
            f.write(" facet normal 0 0 0\n  outer loop\n")
            for p in t:
                f.write("   vertex %.3f %.3f %.3f\n" % p)
            f.write("  endloop\n endfacet\n")
        f.write("endsolid fixture\n")

here = os.path.dirname(os.path.abspath(__file__))
# 30x30x3 flat cube (brim fixture; used twice via CLI)
write_stl(os.path.join(here, "cube30.stl"), box_tris(0, 0, 0, 30, 30, 3))
# T-shape: pillar 10x30x8 + roof slab 30x30x2 on top -> 10mm overhangs both sides
tris = box_tris(10, 0, 0, 20, 30, 8) + box_tris(0, 0, 8, 30, 30, 10)
write_stl(os.path.join(here, "tshape.stl"), tris)
print("wrote cube30.stl, tshape.stl")
