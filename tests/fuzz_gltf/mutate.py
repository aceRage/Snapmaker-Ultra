#!/usr/bin/env python3
"""Drive tests/fuzz_gltf over mutations of the glTF fixture corpus.

    cmake --build build --target fuzz_gltf                 # Ninja / Makefiles
    msbuild build/tests/fuzz_gltf/fuzz_gltf.vcxproj /p:Configuration=Release   # Visual Studio
    python tests/fuzz_gltf/mutate.py --minutes 10

Seeds are every file in tests/data/test_gltf (plus its subdirectories). Each round writes a batch
of mutants into a scratch directory and hands the whole batch to fuzz_gltf in one process; a batch
that exits non-zero, crashes, or times out is kept for inspection and reported.

The point is not coverage-guided fuzzing - it is a cheap, reproducible smoke test that the reader
never crashes, never throws, and never returns false without a message, on input nobody authored.
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CORPUS = os.path.join(REPO, "tests", "data", "test_gltf")

SKIP_SUFFIXES = (".py", ".md")


def find_exe(explicit):
    if explicit:
        return explicit
    names = ["fuzz_gltf.exe", "fuzz_gltf"]
    roots = [os.path.join(REPO, "build", "tests", "fuzz_gltf"),
             os.path.join(REPO, "build", "tests", "fuzz_gltf", "Release"),
             os.path.join(REPO, "build", "tests", "fuzz_gltf", "Debug")]
    for root in roots:
        for name in names:
            candidate = os.path.join(root, name)
            if os.path.isfile(candidate):
                return candidate
    return None


def seeds():
    out = []
    for dirpath, _dirs, files in os.walk(CORPUS):
        for name in files:
            if name.endswith(SKIP_SUFFIXES):
                continue
            out.append(os.path.join(dirpath, name))
    return sorted(out)


def mutate(data, rng):
    """Byte flips, chunk splices and truncation - enough to reach the length and offset fields
    that a parser sizes its allocations from."""
    b = bytearray(data)
    if not b:
        return bytes(b)
    for _ in range(rng.randint(1, 12)):
        kind = rng.random()
        pos = rng.randrange(len(b))
        if kind < 0.45:                                  # single byte
            b[pos] = rng.randrange(256)
        elif kind < 0.65:                                # a 4-byte little-endian field
            value = rng.choice([0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000,
                                rng.randrange(1 << 32)])
            for k in range(4):
                if pos + k < len(b):
                    b[pos + k] = (value >> (8 * k)) & 0xFF
        elif kind < 0.85:                                # splice a run over itself
            n = rng.randint(1, min(64, len(b)))
            src = rng.randrange(len(b))
            chunk = bytes(b[src:src + n])
            b[pos:pos + len(chunk)] = chunk
        else:                                            # truncate
            b = b[:pos]
            break
    return bytes(b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=10.0)
    ap.add_argument("--batch", type=int, default=40, help="mutants handed to one process")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--exe", default=None)
    ap.add_argument("--out", default=None, help="scratch directory (default: a temp dir)")
    args = ap.parse_args()

    exe = find_exe(args.exe)
    if exe is None:
        print("fuzz_gltf not built. See the header of tests/fuzz_gltf/fuzz_gltf.cpp for the "
              "per-generator build command.")
        return 2
    corpus = seeds()
    if not corpus:
        print("no seed files in " + CORPUS)
        return 2

    scratch = args.out or os.path.join(REPO, "build", "fuzz_gltf_work")
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch, exist_ok=True)
    keep = os.path.join(scratch, "findings")
    os.makedirs(keep, exist_ok=True)

    rng = random.Random(args.seed)
    print("exe    : %s" % exe)
    print("seeds  : %d" % len(corpus))
    print("budget : %.1f min" % args.minutes)

    deadline = time.time() + args.minutes * 60.0
    rounds = cases = findings = 0
    while time.time() < deadline:
        rounds += 1
        batch = []
        for i in range(args.batch):
            src = rng.choice(corpus)
            with open(src, "rb") as f:
                data = f.read()
            ext = ".gltf" if src.endswith(".gltf") else ".glb"
            path = os.path.join(scratch, "case_%03d%s" % (i, ext))
            with open(path, "wb") as f:
                f.write(mutate(data, rng))
            batch.append(path)
        cases += len(batch)
        try:
            proc = subprocess.run([exe] + batch, capture_output=True, timeout=120)
            code, err = proc.returncode, proc.stderr.decode("utf-8", "replace")
        except subprocess.TimeoutExpired:
            code, err = "timeout", ""
        if code != 0:
            findings += 1
            dest = os.path.join(keep, "round_%04d" % rounds)
            os.makedirs(dest, exist_ok=True)
            for path in batch:
                shutil.copy2(path, dest)
            print("FINDING round %d exit=%s -> %s" % (rounds, code, dest))
            if err:
                print(err.strip()[:2000])

    print("rounds=%d cases=%d findings=%d" % (rounds, cases, findings))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
