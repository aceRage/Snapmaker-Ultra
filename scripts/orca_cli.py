#!/usr/bin/env python3
"""Drive the Snapmaker-Ultra slicer CLI from Python (or from an AI agent).

    from orca_cli import OrcaCli
    cli = OrcaCli(r"C:\\Program Files\\Snapmaker-Ultra\\snapmaker-orca.exe")
    result = cli.slice(["model.stl"],
                       printer="Snapmaker U1 (0.4 nozzle)",
                       process="0.20 Standard @Snapmaker U1 (0.4 nozzle)",
                       filaments=["Snapmaker PLA Matte @U1"],
                       outdir="out", export_3mf="out.3mf",
                       progress=lambda ev: print(ev["plate_percent"], ev.get("message")))
    print(result["return_code"], result["sliced_plates"][0]["time_s"])

Command line:  python orca_cli.py --exe <slicer> [--printer N] [--process N] [--filament N ...]
                                  [--plate N] [--outdir DIR] [--export-3mf FILE] [--arrange] files...

The slicer writes `result.json` into --outdir and, with --progress-json, one JSON object per line
on stdout: {"event":"progress", "plate_index", "plate_count", "plate_percent", "message"|"warning"}
followed by {"event":"result", "return_code", "error_string", "sliced_plates":[{"id", "time_s",
"filament_mm3", "filament_g", "gcode", "warnings", ...}], ...}. Exit code 0 = success; negative
codes are listed in src/libslic3r/Utils.hpp (CLI_*).
"""
import argparse
import json
import os
import subprocess
import sys
from typing import Callable, Dict, Iterable, List, Optional


class OrcaCli:
    def __init__(self, exe: str, datadir: Optional[str] = None):
        self.exe = exe
        self.datadir = datadir

    def _run(self, args: List[str], progress: Optional[Callable[[dict], None]] = None,
             timeout: Optional[float] = None) -> dict:
        cmd = [self.exe, "--progress-json"] + args
        if self.datadir:
            cmd += ["--datadir", self.datadir]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, encoding="utf-8", errors="replace", bufsize=1)
        result = None
        assert proc.stdout is not None
        for line in proc.stdout:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            if ev.get("event") == "result":
                result = ev
            elif progress:
                progress(ev)
        proc.wait(timeout=timeout)
        stderr = proc.stderr.read() if proc.stderr else ""
        if result is None:
            result = {"return_code": proc.returncode, "error_string": stderr.strip()[-2000:], "sliced_plates": []}
        result["exit_code"] = proc.returncode
        result["stderr"] = stderr
        return result

    def slice(self, files: Iterable[str], printer: Optional[str] = None, process: Optional[str] = None,
              filaments: Optional[List[str]] = None, plate: int = 0, outdir: Optional[str] = None,
              export_3mf: Optional[str] = None, arrange: bool = False, thumbnails: bool = False,
              extra_args: Optional[List[str]] = None,
              progress: Optional[Callable[[dict], None]] = None, timeout: Optional[float] = None) -> dict:
        """Slice plate `plate` (0 = all). Returns the result dict (see module doc)."""
        # --allow-newer-file: this fork's own 3MFs carry a 2.x version while the CLI reports the
        # Bambu-style 01.xx version, which the stock version check would refuse.
        args: List[str] = ["--slice", str(plate), "--allow-newer-file"]
        if printer:
            args += ["--printer-preset", printer]
        if process:
            args += ["--process-preset", process]
        if filaments:
            args += ["--filament-presets", ";".join(filaments)]
        if outdir:
            os.makedirs(outdir, exist_ok=True)
            args += ["--outputdir", outdir]
        if export_3mf:
            args += ["--export-3mf", export_3mf]
        if arrange:
            args += ["--arrange", "1"]
        if not thumbnails:
            args.append("--no-thumbnails")
        if extra_args:
            args += extra_args
        args += list(files)
        return self._run(args, progress=progress, timeout=timeout)

    def info(self, files: Iterable[str]) -> dict:
        return self._run(["--info"] + list(files))


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, help="path to snapmaker-orca.exe")
    ap.add_argument("--datadir", help="isolated data directory (user presets live under <datadir>/user)")
    ap.add_argument("--printer"); ap.add_argument("--process")
    ap.add_argument("--filament", action="append", default=[])
    ap.add_argument("--plate", type=int, default=0)
    ap.add_argument("--outdir", default="cli_out")
    ap.add_argument("--export-3mf")
    ap.add_argument("--arrange", action="store_true")
    ap.add_argument("--thumbnails", action="store_true")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args(argv)
    cli = OrcaCli(a.exe, a.datadir)
    res = cli.slice(a.files, printer=a.printer, process=a.process, filaments=a.filament or None, plate=a.plate,
                    outdir=a.outdir, export_3mf=a.export_3mf, arrange=a.arrange, thumbnails=a.thumbnails,
                    progress=lambda ev: print(f"  plate {ev.get('plate_index')}/{ev.get('plate_count')} "
                                              f"{ev.get('plate_percent')}% {ev.get('message') or ev.get('warning') or ''}",
                                              file=sys.stderr))
    print(json.dumps({k: v for k, v in res.items() if k != "stderr"}, indent=2))
    if res.get("return_code", 1) != 0:
        print(res.get("stderr", "")[-2000:], file=sys.stderr)
    return 0 if res.get("return_code") == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
