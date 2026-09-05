#!/usr/bin/env python3
"""Build twopart_groups.3mf: the corpus case whose parts carry support groups.

    python tests/data/support_corpus/make_group_fixture.py --exe <snapmaker-orca.exe> \
           [--datadir <isolated dd>]

Why it is built this way, and not by make_fixtures.py:

  A support group lives in a volume's ModelConfig, and the only place a 3MF has for that is
  Metadata/model_settings.config. Hand-writing one is guesswork - the <object id> and <part id>
  attributes have to match the ids the exporter assigned in 3D/3dmodel.model - so this script
  does not guess. It asks the SLICER ITSELF for a project:

    1. `--export-3mf` on twopart_bridge.3mf, which the importer turns into ONE object with three
       MODEL_PART volumes. The application writes the whole project, ids included.
    2. The support-group metadata is injected into one <part> of that file's model_settings.
    3. `--export-3mf` again, on the result: if the application reads the group back and writes it
       out, the round trip is proven end to end by the application, not by this script.

Deliberately absent: support_top_z_distance. It is the one part-level key that ACTS in Stage 2 -
the soluble rule of the plan's 3.6 makes the whole object soluble when any group asks for a zero
gap - so a corpus case carrying it would (correctly) change the G-code and the off-mode gate
would (correctly) go red. The keys here are tier A ones the Stage 2 generator ignores, which is
exactly what the gate is there to prove. tests/fff_print/test_support_groups.cpp covers the
soluble rule instead.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "twopart_bridge.3mf")
TARGET = os.path.join(HERE, "twopart_groups.3mf")

PRINTER = "Snapmaker U1 (0.4 nozzle)"
PROCESS = "0.20 Standard @Snapmaker U1 (0.4 nozzle)"

# Two profiles, because Stage 3 needs two kinds of ON-mode evidence:
#
#   filament - the original fixture. The group changes its interface geometry AND pins its own
#              interface filament, so the candidate must also schedule a tool change the baseline
#              never had. Needs two filaments loaded.
#   geometry - the same group without support_interface_filament, on ONE filament. The candidate
#              must differ from the baseline purely in support-interface geometry, with no tool
#              change anywhere - which is the cleaner proof that the interface stage, and nothing
#              else, moved.
PROFILES = {
    "filament": {
        "out":       TARGET,
        "filaments": "Generic PLA;Generic PLA",
        "metadata": [
            ("support_group", "B"),
            ("support_interface_top_layers", "5"),
            ("support_interface_bottom_layers", "4"),
            ("support_interface_spacing", "0.15"),
            ("support_interface_filament", "2"),
        ],
    },
    "geometry": {
        "out":       os.path.join(HERE, "twopart_groups_geom.3mf"),
        "filaments": "Generic PLA",
        "metadata": [
            ("support_group", "B"),
            ("support_interface_top_layers", "5"),
            ("support_interface_bottom_layers", "4"),
            ("support_interface_spacing", "0.15"),
        ],
    },
    # SINGLE PART: the object has exactly one MODEL_PART volume and that volume carries the group,
    # so the default group owns no parts at all. There is no neighbour to measure the result
    # against, which is why the too-small claim of the first Stage 3 build could hand this object's
    # contacts to the default group and leave the G-code looking like the object's own settings -
    # the shape of the bug §2c records. onepart_ledge.3mf is written by make_fixtures.py.
    "singlepart": {
        "source":    os.path.join(HERE, "onepart_ledge.3mf"),
        "out":       os.path.join(HERE, "onepart_group.3mf"),
        "filaments": "Generic PLA",
        "metadata": [
            ("support_group", "B"),
            ("support_interface_top_layers", "5"),
            ("support_interface_bottom_layers", "4"),
            ("support_interface_spacing", "0.15"),
        ],
    },
}

# Set by main() from --profile; the module-level name is kept so the helpers below read the same.
FILAMENTS = PROFILES["filament"]["filaments"]
GROUP_METADATA = PROFILES["filament"]["metadata"]


def export_3mf(exe, datadir, src, dst):
    cmd = [exe]
    if datadir:
        cmd += ["--datadir", datadir]
    cmd += ["--allow-newer-file", "--no-thumbnails", "--export-3mf", dst,
            "--printer-preset", PRINTER, "--process-preset", PROCESS,
            "--filament-presets", FILAMENTS, src]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0 or not os.path.exists(dst):
        sys.exit("--export-3mf failed (%d):\n%s" % (proc.returncode, proc.stderr.decode("utf-8", "replace")[-2000:]))


def inject(src, dst, part_id):
    zin = zipfile.ZipFile(src)
    settings = zin.read("Metadata/model_settings.config").decode("utf-8")
    marker = '<part id="%d"' % part_id
    at = settings.find(marker)
    if at < 0:
        sys.exit("no %s in the exported model_settings.config" % marker)
    insert_at = settings.index("\n", at) + 1
    lines = "".join('      <metadata key="%s" value="%s"/>\n' % kv for kv in GROUP_METADATA)
    settings = settings[:insert_at] + lines + settings[insert_at:]

    zout = zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED)
    for item in zin.infolist():
        data = settings.encode("utf-8") if item.filename == "Metadata/model_settings.config" \
            else zin.read(item.filename)
        zout.writestr(item, data)
    zout.close()
    zin.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, help="path to snapmaker-orca.exe (a side install, never the user's tree)")
    ap.add_argument("--datadir", help="isolated data directory - always pass this")
    ap.add_argument("--part", type=int, default=2, help="which <part id> gets the group (default 2)")
    ap.add_argument("--profile", choices=sorted(PROFILES), default="filament",
                    help="filament (default): the group pins its own interface filament; "
                         "geometry: the same group on one filament, interface geometry only")
    ap.add_argument("--out")
    a = ap.parse_args()

    global FILAMENTS, GROUP_METADATA
    profile = PROFILES[a.profile]
    FILAMENTS = profile["filaments"]
    GROUP_METADATA = profile["metadata"]
    if not a.out:
        a.out = profile["out"]

    source = profile.get("source", SOURCE)
    tmp = tempfile.mkdtemp(prefix="sgfixture_")
    try:
        exported = os.path.join(tmp, "exported.3mf")
        print("1. exporting a project from", os.path.basename(source))
        export_3mf(a.exe, a.datadir, source, exported)

        print("2. injecting the support group onto part %d" % a.part)
        inject(exported, a.out, a.part)

        print("3. re-exporting it, to prove the application reads the group back")
        verify = os.path.join(tmp, "verify.3mf")
        export_3mf(a.exe, a.datadir, a.out, verify)
        back = zipfile.ZipFile(verify).read("Metadata/model_settings.config").decode("utf-8")
        for key, value in GROUP_METADATA:
            needle = '<metadata key="%s" value="%s"/>' % (key, value)
            if needle not in back:
                sys.exit("the application did not write %s back - the injection did not take" % key)
        print("   ok: every injected key survived a load+save by the slicer")
        print("wrote %s (%d bytes)" % (a.out, os.path.getsize(a.out)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
