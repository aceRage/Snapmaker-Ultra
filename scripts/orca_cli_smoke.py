#!/usr/bin/env python3
"""Smoke test for the headless CLI (Phase A of the headless-slicer roadmap).

    python scripts/orca_cli_smoke.py --exe build/Snapmaker_Orca/snapmaker-orca.exe [--project file.3mf]

Checks: presets by name on an STL (slice + result.json estimates + progress events + 3mf export),
error reporting for an unknown preset, and optionally a project 3MF slice (all plates).
"""
import argparse
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from orca_cli import OrcaCli  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def check(cond: bool, msg: str, failures: list):
    print(("PASS " if cond else "FAIL ") + msg)
    if not cond:
        failures.append(msg)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--project", help="optional project 3mf to slice (all plates)")
    ap.add_argument("--printer", default="Snapmaker U1 (0.4 nozzle)")
    ap.add_argument("--process", default="0.20 Standard @Snapmaker U1 (0.4 nozzle)")
    ap.add_argument("--filament", default="Snapmaker PLA Matte @U1")
    ap.add_argument("--keep", action="store_true", help="keep the output directory")
    a = ap.parse_args()
    cli = OrcaCli(a.exe)
    failures: list = []
    out = tempfile.mkdtemp(prefix="orca_cli_smoke_")
    try:
        stl = os.path.join(REPO, "resources", "handy_models", "OrcaToleranceTest.stl")
        events: list = []
        res = cli.slice([stl], printer=a.printer, process=a.process, filaments=[a.filament],
                        outdir=os.path.join(out, "stl"), export_3mf="sliced.3mf", arrange=True,
                        progress=events.append, timeout=900)
        check(res.get("return_code") == 0 and res.get("exit_code") == 0, f"STL slice by preset names (rc={res.get('return_code')}, exit={res.get('exit_code')}) {res.get('error_string','')[:200]}", failures)
        plates = res.get("sliced_plates", [])
        check(len(plates) == 1, f"one sliced plate reported ({len(plates)})", failures)
        if plates:
            p = plates[0]
            check(p.get("time_s", 0) > 0 and p.get("filament_g", 0) > 0, f"estimates present: time_s={p.get('time_s')} filament_g={p.get('filament_g')}", failures)
            check(os.path.isfile(p.get("gcode", "")), f"gcode written: {p.get('gcode')}", failures)
        check(any(e.get("plate_percent", 0) > 0 for e in events), f"progress events received ({len(events)})", failures)
        check(os.path.isfile(os.path.join(out, "stl", "result.json")), "result.json written", failures)
        check(os.path.isfile(os.path.join(out, "stl", "sliced.3mf")), "3mf exported after slicing (no thumbnails)", failures)

        bad = cli.slice([stl], printer="No Such Printer 123", process=a.process, filaments=[a.filament],
                        outdir=os.path.join(out, "bad"), timeout=120)
        check(bad.get("return_code") == -5 and "not found" in (bad.get("error_string", "") + bad.get("stderr", "")).lower(),
              f"unknown preset reported (rc={bad.get('return_code')})", failures)

        if a.project:
            resp = cli.slice([a.project], outdir=os.path.join(out, "proj"), export_3mf="resliced.3mf", timeout=1800)
            check(resp.get("return_code") == 0, f"project slice all plates (rc={resp.get('return_code')}) {resp.get('error_string','')[:200]}", failures)
            check(all(p.get("time_s", 0) > 0 for p in resp.get("sliced_plates", [])), f"every plate has a time estimate ({len(resp.get('sliced_plates', []))} plates)", failures)
    finally:
        if a.keep:
            print("outputs kept in", out)
        else:
            shutil.rmtree(out, ignore_errors=True)
    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
