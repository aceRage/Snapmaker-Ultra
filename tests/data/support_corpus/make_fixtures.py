#!/usr/bin/env python3
"""Generate the small 3MF fixtures of the support-group identity corpus.

The corpus deliberately reuses the models already in resources/handy_models/ so it adds no new
binary data to the repository; the two fixtures written here are the shapes those models do not
provide - one object made of several MODEL_PART volumes, which is what support groups are about.

They are plain core-spec 3MFs (one <object> holding several <component>s). Orca imports each
component as a ModelVolume of one ModelObject, which is exactly the multi-part object the group
resolver reasons over.

    python tests/data/support_corpus/make_fixtures.py

Regenerating is deterministic - byte-identical output for the same source - so the committed
fixtures can be checked against a fresh run.
"""
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

CONTENT_TYPES = """<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
 <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
 <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>
</Types>
"""

RELS = """<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Target="/3D/3dmodel.model" Id="rel-1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
</Relationships>
"""


def box(x0, y0, z0, dx, dy, dz):
    """Axis-aligned box as (vertices, triangles) with outward-facing winding."""
    v = [(x0, y0, z0), (x0 + dx, y0, z0), (x0 + dx, y0 + dy, z0), (x0, y0 + dy, z0),
         (x0, y0, z0 + dz), (x0 + dx, y0, z0 + dz), (x0 + dx, y0 + dy, z0 + dz), (x0, y0 + dy, z0 + dz)]
    t = [(0, 2, 1), (0, 3, 2),          # bottom
         (4, 5, 6), (4, 6, 7),          # top
         (0, 1, 5), (0, 5, 4),          # -Y
         (1, 2, 6), (1, 6, 5),          # +X
         (2, 3, 7), (2, 7, 6),          # +Y
         (3, 0, 4), (3, 4, 7)]          # -X
    return v, t


def mesh_xml(vertices, triangles, indent="     "):
    out = [indent + "<mesh>", indent + " <vertices>"]
    for x, y, z in vertices:
        out.append('%s  <vertex x="%g" y="%g" z="%g"/>' % (indent, x, y, z))
    out.append(indent + " </vertices>")
    out.append(indent + " <triangles>")
    for a, b, c in triangles:
        out.append('%s  <triangle v1="%d" v2="%d" v3="%d"/>' % (indent, a, b, c))
    out.append(indent + " </triangles>")
    out.append(indent + "</mesh>")
    return "\n".join(out)


def write_3mf(path, parts, assembly_id=100, part_config=None):
    """parts: list of (name, vertices, triangles). One assembly object holds them all.

    part_config: optional {part index -> {key: value}} written into
    Metadata/model_settings.config, the same place bbs_3mf stores per-volume settings.
    """
    objs = []
    for i, (name, v, t) in enumerate(parts, start=1):
        objs.append('  <object id="%d" name="%s" type="model">\n%s\n  </object>'
                    % (i, name, mesh_xml(v, t)))
    comps = "\n".join('    <component objectid="%d"/>' % (i + 1) for i in range(len(parts)))
    objs.append('  <object id="%d" name="assembly" type="model">\n   <components>\n%s\n   </components>\n  </object>'
                % (assembly_id, comps))
    model = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<model unit="millimeter" xml:lang="en-US" '
        'xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">\n'
        ' <resources>\n' + "\n".join(objs) + '\n </resources>\n'
        ' <build>\n  <item objectid="%d"/>\n </build>\n'
        '</model>\n' % assembly_id)

    files = [("[Content_Types].xml", CONTENT_TYPES),
             ("_rels/.rels", RELS),
             ("3D/3dmodel.model", model)]

    if part_config:
        # <part id> is the object id of the component's own <object>, i.e. 1-based part index.
        rows = []
        for idx, cfg in sorted(part_config.items()):
            rows.append('  <part id="%d" subtype="ModelPart">' % (idx + 1))
            rows.append('   <metadata key="name" value="%s"/>' % parts[idx][0])
            for key in sorted(cfg):
                rows.append('   <metadata key="%s" value="%s"/>' % (key, cfg[key]))
            rows.append("  </part>")
        files.append(("Metadata/model_settings.config",
                      '<?xml version="1.0" encoding="UTF-8"?>\n<config>\n'
                      ' <object id="%d">\n%s\n </object>\n</config>\n'
                      % (assembly_id, "\n".join(rows))))

    # Fixed timestamps so regenerating gives a byte-identical archive.
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files:
            info = zipfile.ZipInfo(name, date_time=(2026, 9, 3, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o600 << 16
            z.writestr(info, data)
    print("wrote", path, os.path.getsize(path), "bytes")


def main():
    # A single pillar carrying a two-piece deck. Everything is one connected solid resting on the
    # bed, so each layer has ONE island - two disconnected islands made the print order (and with
    # it whole blocks of G-code) vary between two runs of the same binary. The deck overhangs the
    # pillar on both sides by different amounts, which both breaks ties and gives each half its own
    # support region: the A/B shape the plan's Stage 3 gate measures.
    pillar_v, pillar_t = box(-4, -8, 0, 8, 16, 10)
    deck_a_v, deck_a_t = box(-20, -8, 10, 20, 16, 2)
    deck_b_v, deck_b_t = box(0, -8, 10, 14, 16, 2)
    write_3mf(os.path.join(HERE, "twopart_bridge.3mf"),
              [("pillar", pillar_v, pillar_t),
               ("partA", deck_a_v, deck_a_t),
               ("partB", deck_b_v, deck_b_t)])

    # The same bridge with support-group data on part B. Nothing in Stage 2 consumes a group,
    # so this must slice exactly like twopart_bridge.3mf - that is the whole Stage 2 gate. The
    # values deliberately avoid support_top_z_distance: a zero gap trips the soluble rule of 3.6,
    # which DOES change the object config and therefore the output, by design.
    write_3mf(os.path.join(HERE, "twopart_bridge_grouped.3mf"),
              [("pillar", pillar_v, pillar_t),
               ("partA", deck_a_v, deck_a_t),
               ("partB", deck_b_v, deck_b_t)],
              part_config={2: {"support_group": "B",
                               "support_interface_top_layers": "5",
                               "support_interface_spacing": "0"}})

    # Positive control: the same file with a soluble part. The soluble rule makes the whole object
    # soluble, so this one MUST differ between a build that knows support groups and one that does
    # not - which is how we know the metadata above is really being read rather than ignored.
    write_3mf(os.path.join(HERE, "twopart_bridge_soluble.3mf"),
              [("pillar", pillar_v, pillar_t),
               ("partA", deck_a_v, deck_a_t),
               ("partB", deck_b_v, deck_b_t)],
              part_config={2: {"support_group": "B", "support_top_z_distance": "0"}})

    # A stubby leg under one end of a long ledge: a single overhang region, two volumes, and the
    # simplest thing that still exercises contact / base / interface generation.
    p_v, p_t = box(-15, -6, 0, 6, 12, 6)
    o_v, o_t = box(-15, -6, 6, 30, 12, 2)
    write_3mf(os.path.join(HERE, "overhang_ledge.3mf"),
              [("pillar", p_v, p_t), ("ledge", o_v, o_t)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
