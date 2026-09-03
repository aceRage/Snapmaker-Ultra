#!/usr/bin/env python3
"""Generate the self-authored glTF/GLB fixtures for tests/libslic3r/test_gltf.cpp.

Run from anywhere:  python tests/data/test_gltf/make_fixtures.py
Every file it writes lands next to this script. The vendored Khronos assets (see SOURCES.md)
are NOT produced here - they are downloaded once and committed.

The numbers in these files are the test's expectations, so keep the two in step. glTF is
right-handed +Y up; the slicer is Z-up, so the reader maps (x, y, z) -> (x, -z, y).
"""

import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# The fixture names include non-ASCII characters on purpose; a cp1252 console must not stop us.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

FLOAT = 5126
UNSIGNED_SHORT = 5123
UNSIGNED_INT = 5125

MODE_POINTS = 0
MODE_TRIANGLES = 4
MODE_TRIANGLE_STRIP = 5
MODE_TRIANGLE_FAN = 6


# --------------------------------------------------------------------------------------- io

def pad4(blob, filler=b"\x00"):
    return blob + filler * ((4 - len(blob) % 4) % 4)


def write_glb(name, gltf, blob):
    gltf = dict(gltf)
    gltf["buffers"] = [{"byteLength": len(blob)}]
    js = pad4(json.dumps(gltf, separators=(",", ":")).encode("utf-8"), b" ")
    bn = pad4(blob)
    total = 12 + 8 + len(js) + (8 + len(bn) if bn else 0)
    out = struct.pack("<III", GLB_MAGIC, 2, total)
    out += struct.pack("<II", len(js), CHUNK_JSON) + js
    if bn:
        out += struct.pack("<II", len(bn), CHUNK_BIN) + bn
    path = os.path.join(HERE, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(out)
    print("%-40s %6d bytes" % (name, len(out)))


def write_gltf_pair(stem, gltf, blob):
    """A .gltf with its buffer in a sidecar .bin."""
    gltf = dict(gltf)
    bin_name = os.path.basename(stem) + ".bin"
    gltf["buffers"] = [{"byteLength": len(blob), "uri": bin_name}]
    with open(os.path.join(HERE, stem + ".bin"), "wb") as f:
        f.write(blob)
    text = json.dumps(gltf, indent=2) + "\n"
    with open(os.path.join(HERE, stem + ".gltf"), "w", encoding="utf-8") as f:
        f.write(text)
    print("%-40s %6d + %d bytes" % (stem + ".gltf/.bin", len(text), len(blob)))


class Blob:
    """Accumulates buffer bytes and hands back bufferView indices."""

    def __init__(self):
        self.data = bytearray()
        self.views = []

    def add(self, payload, target=None):
        while len(self.data) % 4:
            self.data.append(0)
        view = {"buffer": 0, "byteOffset": len(self.data), "byteLength": len(payload)}
        if target is not None:
            view["target"] = target
        self.data.extend(payload)
        self.views.append(view)
        return len(self.views) - 1

    def add_floats(self, values, target=34962):
        return self.add(struct.pack("<%df" % len(values), *values), target)

    def add_ushorts(self, values, target=34963):
        return self.add(struct.pack("<%dH" % len(values), *values), target)


# ---------------------------------------------------------------------------------- geometry

def box_faces(x0, y0, z0, x1, y1, z1):
    """Six quads, each listed counter-clockwise as seen from outside (glTF's front face)."""
    return [
        ([(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)], (1, 0, 0)),
        ([(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)], (-1, 0, 0)),
        ([(x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)], (0, 1, 0)),
        ([(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], (0, -1, 0)),
        ([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], (0, 0, 1)),
        ([(x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0)], (0, 0, -1)),
    ]


def box_mesh(x0, y0, z0, x1, y1, z1):
    """A box the way a real exporter writes one: 24 vertices split at the seams, 36 indices.
    Unwelded on purpose - it is what makes the its_merge_vertices step in the reader testable."""
    positions, normals, indices = [], [], []
    for quad, normal in box_faces(x0, y0, z0, x1, y1, z1):
        base = len(positions) // 3
        for corner in quad:
            positions.extend(corner)
            normals.extend(normal)
        indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return positions, normals, indices


def bounds(positions):
    xs = positions[0::3]
    ys = positions[1::3]
    zs = positions[2::3]
    return [min(xs), min(ys), min(zs)], [max(xs), max(ys), max(zs)]


def simple_box_gltf(x0, y0, z0, x1, y1, z1, material=None, node_name=None, scene_name=None):
    positions, normals, indices = box_mesh(x0, y0, z0, x1, y1, z1)
    blob = Blob()
    v_pos = blob.add_floats(positions)
    v_nrm = blob.add_floats(normals)
    v_idx = blob.add_ushorts(indices)
    lo, hi = bounds(positions)
    gltf = {
        "asset": {"version": "2.0", "generator": "Snapmaker Orca test fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2,
                                    "mode": MODE_TRIANGLES}]}],
        "accessors": [
            {"bufferView": v_pos, "componentType": FLOAT, "count": len(positions) // 3,
             "type": "VEC3", "min": lo, "max": hi},
            {"bufferView": v_nrm, "componentType": FLOAT, "count": len(normals) // 3, "type": "VEC3"},
            {"bufferView": v_idx, "componentType": UNSIGNED_SHORT, "count": len(indices), "type": "SCALAR"},
        ],
        "bufferViews": blob.views,
    }
    if node_name:
        gltf["nodes"][0]["name"] = node_name
    if scene_name:
        gltf["scenes"][0]["name"] = scene_name
    if material is not None:
        gltf["materials"] = [material]
        gltf["meshes"][0]["primitives"][0]["material"] = 0
    return gltf, bytes(blob.data)


# ----------------------------------------------------------------------------------- fixtures

def _external_tool(command, out_name, src_name="box_10_20_30.glb"):
    """Run one glTF-Transform command over a fixture. Needs node, so it only regenerates the file
    when npx is available; otherwise the committed one stands. The committed files were produced
    with glTF-Transform v4.5.0."""
    import shutil
    import subprocess

    out = os.path.join(HERE, out_name)
    if shutil.which("npx") is None:
        print("%-40s skipped (no npx); keeping the committed file" % out_name)
        return
    src = os.path.join(HERE, src_name)
    try:
        subprocess.run(["npx", "--yes", "@gltf-transform/cli@4", command, src, out],
                       check=True, shell=(os.name == "nt"),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("%-40s %6d bytes (gltf-transform %s)" % (out_name, os.path.getsize(out), command))
    except Exception as exc:  # noqa: BLE001 - a missing network is not a test failure
        print("%-40s skipped (%s); keeping the committed file" % (out_name, exc))


def make_box_10_20_30():
    """10 (X) x 20 (Y) x 30 (Z) in glTF space -> Vec3d(10, 30, 20) in the slicer.
    The up-axis rule and the unit rule in one assertion; asymmetric on purpose so a wrong sign
    or a swapped axis cannot pass."""
    material = {"name": "plain", "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.8, 0.8, 1.0]}}
    gltf, blob = simple_box_gltf(-5, -10, -15, 5, 10, 15, material, node_name="box", scene_name="box scene")
    write_glb("box_10_20_30.glb", gltf, blob)
    write_gltf_pair("box_10_20_30", gltf, blob)
    # The same box at a non-ASCII path, guarding the nowide file read (mirrors test_stl.cpp).
    write_glb(os.path.join("Geräte", "box-čřšřěá.glb"), gltf, blob)


def make_box_stl_twin():
    """box_10_20_30 as a binary STL, already in the slicer's Z-up frame: 10 (X) x 30 (Y) x 20 (Z).

    This is the Slice Compare control from the plan's manual checklist. Slicing it and the .glb
    with the same printer/process/filament must produce identical G-code - the end-to-end proof
    that the up-axis rule and the unit rule turn the glTF into the same solid a known-good STL
    describes. Note the axes are the glTF box's after (x, y, z) -> (x, -z, y), not before.
    """
    tris = []
    for quad, normal in box_faces(-5, -15, -10, 5, 15, 10):
        a, b, c, d = quad
        tris.append((normal, (a, b, c)))
        tris.append((normal, (a, c, d)))
    out = bytearray(b"Snapmaker Orca glTF test fixture: box 10x30x20, twin of box_10_20_30.glb")
    out += b" " * (80 - len(out))
    out += struct.pack("<I", len(tris))
    for normal, verts in tris:
        out += struct.pack("<3f", *normal)
        for v in verts:
            out += struct.pack("<3f", *v)
        out += struct.pack("<H", 0)
    path = os.path.join(HERE, "box_10_20_30.stl")
    with open(path, "wb") as f:
        f.write(out)
    print("%-40s %6d bytes" % ("box_10_20_30.stl", len(out)))


def make_box_meters():
    """0.01 x 0.02 x 0.03 units. At 1 unit = 1 mm this is 6e-6 mm3, below the 0.008 mm3 that
    makes Model::looks_like_saved_in_meters() offer to scale it - the rescue path the units
    decision relies on."""
    gltf, blob = simple_box_gltf(-0.005, -0.01, -0.015, 0.005, 0.01, 0.015, node_name="tiny box")
    write_glb("box_meters.glb", gltf, blob)


def make_two_parts_two_materials():
    """One mesh, two primitives, two materials - the reason a ModelVolume is a (node, primitive)
    pair and not a mesh."""
    pos_a, nrm_a, idx_a = box_mesh(0, 0, 0, 10, 10, 10)
    pos_b, nrm_b, idx_b = box_mesh(12, 0, 0, 22, 10, 10)
    blob = Blob()
    va_p, va_n, va_i = blob.add_floats(pos_a), blob.add_floats(nrm_a), blob.add_ushorts(idx_a)
    vb_p, vb_n, vb_i = blob.add_floats(pos_b), blob.add_floats(nrm_b), blob.add_ushorts(idx_b)
    lo_a, hi_a = bounds(pos_a)
    lo_b, hi_b = bounds(pos_b)
    gltf = {
        "asset": {"version": "2.0", "generator": "Snapmaker Orca test fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "blocks"}],
        "meshes": [{"name": "blocks", "primitives": [
            {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0, "mode": MODE_TRIANGLES},
            {"attributes": {"POSITION": 3, "NORMAL": 4}, "indices": 5, "material": 1, "mode": MODE_TRIANGLES},
        ]}],
        "materials": [
            {"name": "red", "pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.0, 0.0, 1.0]}},
            {"name": "blue", "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 1.0, 1.0]}},
        ],
        "accessors": [
            {"bufferView": va_p, "componentType": FLOAT, "count": len(pos_a) // 3, "type": "VEC3",
             "min": lo_a, "max": hi_a},
            {"bufferView": va_n, "componentType": FLOAT, "count": len(nrm_a) // 3, "type": "VEC3"},
            {"bufferView": va_i, "componentType": UNSIGNED_SHORT, "count": len(idx_a), "type": "SCALAR"},
            {"bufferView": vb_p, "componentType": FLOAT, "count": len(pos_b) // 3, "type": "VEC3",
             "min": lo_b, "max": hi_b},
            {"bufferView": vb_n, "componentType": FLOAT, "count": len(nrm_b) // 3, "type": "VEC3"},
            {"bufferView": vb_i, "componentType": UNSIGNED_SHORT, "count": len(idx_b), "type": "SCALAR"},
        ],
        "bufferViews": blob.views,
    }
    write_glb("two_parts_two_materials.glb", gltf, bytes(blob.data))


def make_three_materials():
    """One mesh, three primitives, three materials - red, green, blue.

    Used for the colour dialog: three swatches, three parts, three filaments. Also the fixture for
    the hidden-instance check, where the modal hook has to answer the dialog rather than let a
    phone-initiated import block forever on a window nobody can see.
    """
    boxes = [box_mesh(x, 0, 0, x + 10, 10, 10) for x in (0, 12, 24)]
    colors = [[1.0, 0.0, 0.0, 1.0], [0.0, 1.0, 0.0, 1.0], [0.0, 0.0, 1.0, 1.0]]
    names = ["red", "green", "blue"]
    blob = Blob()
    accessors, primitives = [], []
    for i, (pos, nrm, idx) in enumerate(boxes):
        v_p, v_n, v_i = blob.add_floats(pos), blob.add_floats(nrm), blob.add_ushorts(idx)
        lo, hi = bounds(pos)
        base = len(accessors)
        accessors += [
            {"bufferView": v_p, "componentType": FLOAT, "count": len(pos) // 3, "type": "VEC3",
             "min": lo, "max": hi},
            {"bufferView": v_n, "componentType": FLOAT, "count": len(nrm) // 3, "type": "VEC3"},
            {"bufferView": v_i, "componentType": UNSIGNED_SHORT, "count": len(idx), "type": "SCALAR"},
        ]
        primitives.append({"attributes": {"POSITION": base, "NORMAL": base + 1},
                           "indices": base + 2, "material": i, "mode": MODE_TRIANGLES})
    gltf = {
        "asset": {"version": "2.0", "generator": "Snapmaker Orca test fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0], "name": "traffic light"}],
        "nodes": [{"mesh": 0, "name": "lamps"}],
        "meshes": [{"name": "lamps", "primitives": primitives}],
        "materials": [{"name": n, "pbrMetallicRoughness": {"baseColorFactor": c}}
                      for n, c in zip(names, colors)],
        "accessors": accessors,
        "bufferViews": blob.views,
    }
    write_glb("three_materials.glb", gltf, bytes(blob.data))


def make_nested_trs():
    """parent: translate (5,0,0) then rotate +90 deg about Y; child: translate (1,0,0), scale 2;
    mesh: a 1 x 2 x 4 box centred on the child origin.

    Ry(+90) maps (x,y,z) -> (z,y,-x), so the box centre lands at (5,0,-1) with half extents
    (4,2,1) in glTF space. After the reader's (x,y,z) -> (x,-z,y) that is centre (5,1,0) and
    size (8,2,4). Nothing about that survives a wrong composition order.

    The material's baseColorFactor is linear 0.2158605, which is sRGB 0.5 - so the test also
    pins the linear-to-sRGB conversion Stage 2 depends on.
    """
    positions, normals, indices = box_mesh(-0.5, -1, -2, 0.5, 1, 2)
    blob = Blob()
    v_pos, v_nrm, v_idx = blob.add_floats(positions), blob.add_floats(normals), blob.add_ushorts(indices)
    lo, hi = bounds(positions)
    half = math.sqrt(0.5)
    gltf = {
        "asset": {"version": "2.0", "generator": "Snapmaker Orca test fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"name": "parent", "translation": [5.0, 0.0, 0.0], "rotation": [0.0, half, 0.0, half],
             "children": [1]},
            {"name": "child", "translation": [1.0, 0.0, 0.0], "scale": [2.0, 2.0, 2.0], "mesh": 0},
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2,
                                    "material": 0, "mode": MODE_TRIANGLES}]}],
        "materials": [{"name": "mid grey",
                       "pbrMetallicRoughness": {"baseColorFactor": [0.2158605, 0.2158605, 0.2158605, 1.0]}}],
        "accessors": [
            {"bufferView": v_pos, "componentType": FLOAT, "count": len(positions) // 3, "type": "VEC3",
             "min": lo, "max": hi},
            {"bufferView": v_nrm, "componentType": FLOAT, "count": len(normals) // 3, "type": "VEC3"},
            {"bufferView": v_idx, "componentType": UNSIGNED_SHORT, "count": len(indices), "type": "SCALAR"},
        ],
        "bufferViews": blob.views,
    }
    write_glb("nested_trs.glb", gltf, bytes(blob.data))


def make_strip_and_fan():
    """One TRIANGLE_STRIP (4 vertices -> 2 triangles, in glTF's XY plane facing +Z) and one
    TRIANGLE_FAN (5 vertices -> 3 triangles, in the XZ plane facing +Y), with no indices.

    The strip's second triangle only faces the same way as the first if the odd-triangle vertex
    swap is applied, so the test asserts both face normals - which is the whole point of the
    fixture."""
    strip = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0]
    fan = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, -1.0, -1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
    blob = Blob()
    v_strip = blob.add_floats(strip)
    v_fan = blob.add_floats(fan)
    lo_s, hi_s = bounds(strip)
    lo_f, hi_f = bounds(fan)
    gltf = {
        "asset": {"version": "2.0", "generator": "Snapmaker Orca test fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "sheet"}],
        "meshes": [{"primitives": [
            {"attributes": {"POSITION": 0}, "mode": MODE_TRIANGLE_STRIP},
            {"attributes": {"POSITION": 1}, "mode": MODE_TRIANGLE_FAN},
        ]}],
        "accessors": [
            {"bufferView": v_strip, "componentType": FLOAT, "count": 4, "type": "VEC3", "min": lo_s, "max": hi_s},
            {"bufferView": v_fan, "componentType": FLOAT, "count": 5, "type": "VEC3", "min": lo_f, "max": hi_f},
        ],
        "bufferViews": blob.views,
    }
    write_glb("strip_and_fan.glb", gltf, bytes(blob.data))


def tiny_png_data_uri():
    """A 1x1 opaque PNG as a data: URI. cgltf never decodes images, so the bytes only have to be a
    well-formed PNG for the file to be honest about carrying a texture."""
    import base64
    import struct
    import zlib

    def chunk(kind, payload):
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)          # 1x1, 8-bit, truecolour
    idat = zlib.compress(b"\x00\xc0\x30\x60")                    # one filtered scanline
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
    return "data:image/png;base64," + base64.b64encode(png).decode("ascii")


def make_textured_two_materials():
    """Two primitives and two materials, one of which paints from a baseColorTexture.

    Two materials on purpose: without the had_textures guard this file WOULD open the colour
    dialog, so the test proves the guard works rather than proving there was nothing to ask.
    """
    pos_a, nrm_a, idx_a = box_mesh(0, 0, 0, 10, 10, 10)
    pos_b, nrm_b, idx_b = box_mesh(12, 0, 0, 22, 10, 10)
    uv = [0.0, 0.0] * (len(pos_a) // 3)
    blob = Blob()
    va_p, va_n, va_i = blob.add_floats(pos_a), blob.add_floats(nrm_a), blob.add_ushorts(idx_a)
    va_uv = blob.add_floats(uv)
    vb_p, vb_n, vb_i = blob.add_floats(pos_b), blob.add_floats(nrm_b), blob.add_ushorts(idx_b)
    lo_a, hi_a = bounds(pos_a)
    lo_b, hi_b = bounds(pos_b)
    gltf = {
        "asset": {"version": "2.0", "generator": "Snapmaker Orca test fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "blocks"}],
        "meshes": [{"name": "blocks", "primitives": [
            {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 3}, "indices": 2,
             "material": 0, "mode": MODE_TRIANGLES},
            {"attributes": {"POSITION": 4, "NORMAL": 5}, "indices": 6, "material": 1,
             "mode": MODE_TRIANGLES},
        ]}],
        "materials": [
            {"name": "painted", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}},
            {"name": "plain", "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 1.0, 1.0]}},
        ],
        "textures": [{"source": 0}],
        "images": [{"uri": tiny_png_data_uri()}],
        "extensionsUsed": [],
        "accessors": [
            {"bufferView": va_p, "componentType": FLOAT, "count": len(pos_a) // 3, "type": "VEC3",
             "min": lo_a, "max": hi_a},
            {"bufferView": va_n, "componentType": FLOAT, "count": len(nrm_a) // 3, "type": "VEC3"},
            {"bufferView": va_i, "componentType": UNSIGNED_SHORT, "count": len(idx_a), "type": "SCALAR"},
            {"bufferView": va_uv, "componentType": FLOAT, "count": len(uv) // 2, "type": "VEC2"},
            {"bufferView": vb_p, "componentType": FLOAT, "count": len(pos_b) // 3, "type": "VEC3",
             "min": lo_b, "max": hi_b},
            {"bufferView": vb_n, "componentType": FLOAT, "count": len(nrm_b) // 3, "type": "VEC3"},
            {"bufferView": vb_i, "componentType": UNSIGNED_SHORT, "count": len(idx_b), "type": "SCALAR"},
        ],
        "bufferViews": blob.views,
    }
    del gltf["extensionsUsed"]
    write_glb("textured_two_materials.glb", gltf, bytes(blob.data))


def make_points_only():
    """No printable surface at all - the reader must say so by name, not fail generically."""
    pts = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0]
    blob = Blob()
    v = blob.add_floats(pts)
    lo, hi = bounds(pts)
    gltf = {
        "asset": {"version": "2.0", "generator": "Snapmaker Orca test fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "mode": MODE_POINTS}]}],
        "accessors": [{"bufferView": v, "componentType": FLOAT, "count": 3, "type": "VEC3",
                       "min": lo, "max": hi}],
        "bufferViews": blob.views,
    }
    write_glb("points_only.glb", gltf, bytes(blob.data))


def make_sparse_triangle():
    """A .gltf whose POSITION accessor is sparse: the base holds (0,0,0) (1,0,0) (0,1,0) and one
    sparse override moves vertex 2 to (0,5,0). A reader that ignores sparse accessors gets a
    1 x 1 triangle instead of a 1 x 5 one - the classic hand-rolled-reader failure."""
    base = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0]
    blob = Blob()
    v_idx = blob.add_ushorts([0, 1, 2])
    v_pos = blob.add_floats(base)
    v_sparse_i = blob.add_ushorts([2], target=None)
    v_sparse_v = blob.add_floats([0.0, 5.0, 0.0], target=None)
    gltf = {
        "asset": {"version": "2.0", "generator": "Snapmaker Orca test fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "sparse triangle"}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 1}, "indices": 0, "mode": MODE_TRIANGLES}]}],
        "accessors": [
            {"bufferView": v_idx, "componentType": UNSIGNED_SHORT, "count": 3, "type": "SCALAR"},
            {"bufferView": v_pos, "componentType": FLOAT, "count": 3, "type": "VEC3",
             "min": [0.0, 0.0, 0.0], "max": [1.0, 5.0, 0.0],
             "sparse": {"count": 1,
                        "indices": {"bufferView": v_sparse_i, "byteOffset": 0,
                                    "componentType": UNSIGNED_SHORT},
                        "values": {"bufferView": v_sparse_v, "byteOffset": 0}}},
        ],
        "bufferViews": blob.views,
    }
    write_gltf_pair("sparse_triangle", gltf, bytes(blob.data))


def make_box_quantized():
    """box_10_20_30.glb through KHR_mesh_quantization.

    gltf-transform rewrites POSITION as normalized 16-bit integers and moves the real scale onto
    the node, so a reader that ignores quantization silently imports the box at the wrong size -
    the one failure mode worse than an error. cgltf_accessor_unpack_floats de-quantizes for us and
    cgltf_node_transform_world picks up the node scale, so the assertion is the same
    Vec3d(10, 30, 20) as the uncompressed box.
    """
    _external_tool("quantize", "box_quantized.glb")


def make_box_meshopt():
    """box_10_20_30.glb through EXT_meshopt_compression.

    gltf-transform emits all three meshopt modes for this file - TRIANGLES for the indices,
    ATTRIBUTES for positions and (with the OCTAHEDRAL filter) normals - and quantizes on the way,
    so one fixture covers the whole decoder path plus its interaction with KHR_mesh_quantization.
    """
    _external_tool("meshopt", "box_meshopt.glb")


def make_box_draco():
    """box_10_20_30.glb run through Draco compression.

    Needs an external tool, so this only regenerates the file when node is available; the
    committed box_draco.glb was produced with glTF-Transform v4.5.0:

        npx --yes @gltf-transform/cli@4 draco box_10_20_30.glb box_draco.glb

    Stage 1 refuses it by name on extensionsRequired. Stage 3 (Draco decode) inverts that
    assertion against the same file rather than deleting it.
    """
    _external_tool("draco", "box_draco.glb")


def make_unknown_extension():
    """An unknown required extension must be named in the error, not swallowed."""
    gltf, blob = simple_box_gltf(-5, -10, -15, 5, 10, 15, node_name="box")
    gltf["extensionsUsed"] = ["KHR_texture_basisu"]
    gltf["extensionsRequired"] = ["KHR_texture_basisu"]
    write_glb("unknown_extension.glb", gltf, blob)


def make_escaping_buffer():
    """A .gltf whose buffer URI climbs out of its own directory. The reader must refuse before it
    opens anything - the phone upload endpoint writes files an attacker chose."""
    gltf, blob = simple_box_gltf(-5, -10, -15, 5, 10, 15, node_name="box")
    gltf["buffers"] = [{"byteLength": len(blob), "uri": "..%2F..%2Fsecret.bin"}]
    with open(os.path.join(HERE, "escaping_buffer.gltf"), "w", encoding="utf-8") as f:
        f.write(json.dumps(gltf, indent=2) + "\n")
    print("%-40s" % "escaping_buffer.gltf")


def make_truncated():
    """The first 200 bytes of box_10_20_30.glb: a damaged file must produce the named error and
    must not crash or leak."""
    with open(os.path.join(HERE, "box_10_20_30.glb"), "rb") as f:
        head = f.read(200)
    with open(os.path.join(HERE, "truncated.glb"), "wb") as f:
        f.write(head)
    print("%-40s %6d bytes" % ("truncated.glb", len(head)))


def main():
    make_box_10_20_30()
    make_box_stl_twin()
    make_box_meters()
    make_two_parts_two_materials()
    make_three_materials()
    make_nested_trs()
    make_strip_and_fan()
    make_textured_two_materials()
    make_points_only()
    make_sparse_triangle()
    make_box_quantized()
    make_box_meshopt()
    make_box_draco()
    make_unknown_extension()
    make_escaping_buffer()
    make_truncated()


if __name__ == "__main__":
    main()
