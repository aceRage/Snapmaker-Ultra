#!/usr/bin/env python3
"""Guard the preset-name invariants that the upstream profile validator enforces.

Two distinct failure modes, both of which the validator reports as errors and both of
which are invisible at runtime in this fork:

1.  Duplicate display names across vendors.  ``PresetCollection::merge_presets``
    (src/libslic3r/Preset.cpp) special-cases ``SM_BUNDLE == "Snapmaker"`` so a clashing
    Snapmaker preset is kept alongside BBL's instead of being dropped.  Nothing is lost,
    but ``find_preset_internal`` does a ``lower_bound`` over a sorted deque and therefore
    returns whichever of the two sorted first -- i.e. the winner depends on load order,
    which depends on the filesystem.  Upstream's validator has no such special case and
    reports ``Found duplicated preset: <name> in vendor: <vendor>``.

2.  Ambiguous ``renamed_from`` claims.  ``update_map_system_profile_renamed``
    (src/libslic3r/Preset.cpp) inserts each ``renamed_from`` entry into a map and logs an
    error when two presets claim the same old name.  A ``renamed_from`` naming a preset
    that is still live is worse than useless: the live name always wins, because every
    lookup tries ``find_preset_internal`` before ``find_preset_renamed``, so the alias can
    never fire.

Exit code 0 when clean, 1 on any error.  Registered as the ``profile_names`` ctest.
"""

import argparse
import json
import os
import sys
from collections import defaultdict

# Presets that legitimately share a name across vendors because one is a non-instantiated
# base/common profile. Only instantiated presets are user-selectable and can clash.
INSTANTIATED = "true"


def iter_presets(profiles_dir):
    """Yield (vendor, path, parsed_json) for every filament/process/machine preset."""
    for vendor in sorted(os.listdir(profiles_dir)):
        vdir = os.path.join(profiles_dir, vendor)
        if not os.path.isdir(vdir):
            continue
        for sub in ("filament", "process", "machine"):
            sdir = os.path.join(vdir, sub)
            if not os.path.isdir(sdir):
                continue
            for root, _dirs, files in os.walk(sdir):
                for fn in sorted(files):
                    if not fn.endswith(".json"):
                        continue
                    path = os.path.join(root, fn)
                    try:
                        with open(path, encoding="utf-8-sig") as fh:
                            yield vendor, path, json.load(fh)
                    except Exception as exc:  # noqa: BLE001 - report, do not abort
                        print("[ERROR] %s: cannot parse: %s" % (path, exc))
                        yield vendor, path, None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--profiles", default=None,
                    help="path to resources/profiles (default: inferred from this script)")
    args = ap.parse_args()

    profiles = args.profiles
    if profiles is None:
        here = os.path.dirname(os.path.abspath(__file__))
        profiles = os.path.join(os.path.dirname(here), "resources", "profiles")
    profiles = os.path.abspath(profiles)
    if not os.path.isdir(profiles):
        print("[ERROR] no such profiles directory: %s" % profiles)
        return 1

    # name -> list of (vendor, path); only instantiated (user-selectable) presets
    by_name = defaultdict(list)
    # renamed_from old-name -> list of (vendor, path, new_name)
    claims = defaultdict(list)
    live_names = set()
    errors = 0
    parse_failures = 0

    for vendor, path, data in iter_presets(profiles):
        if data is None:
            parse_failures += 1
            continue
        name = data.get("name")
        if not name:
            continue
        live_names.add(name)
        if str(data.get("instantiation", "")).lower() == INSTANTIATED:
            by_name[name].append((vendor, path))
        rf = data.get("renamed_from")
        if rf:
            # renamed_from is a c-style-escaped, ';'-separated string list
            parts = rf.split(";") if isinstance(rf, str) else list(rf)
            for old in (p.strip() for p in parts):
                if old:
                    claims[old].append((vendor, path, name))

    errors += parse_failures

    # 1. cross-vendor duplicate display names
    for name, owners in sorted(by_name.items()):
        vendors = {v for v, _ in owners}
        if len(owners) > 1 and len(vendors) > 1:
            errors += 1
            print("[ERROR] duplicated preset name %r claimed by %d vendors:" % (name, len(vendors)))
            for v, p in owners:
                print("            %-22s %s" % (v, os.path.relpath(p, profiles)))

    # 2a. two presets claiming the same old name
    for old, owners in sorted(claims.items()):
        if len(owners) > 1:
            errors += 1
            print("[ERROR] renamed_from %r claimed by %d presets:" % (old, len(owners)))
            for v, p, new in owners:
                print("            %-22s %-34s %s" % (v, new, os.path.relpath(p, profiles)))

    # 2b. a renamed_from that names a preset which is still live. The live name always wins,
    #     because every lookup tries find_preset_internal before find_preset_renamed, so the
    #     alias can never fire. Reported as a warning, not an error: the tree carries a number
    #     of long-standing cases of this (Creality, Flashforge, OrcaFilamentLibrary) that
    #     upstream's own validator accepts, and they are inert rather than wrong. It is still
    #     worth seeing, because it is the trap that makes `renamed_from: "Generic PLA"` the
    #     wrong way to alias a renamed Snapmaker generic -- BBL's "Generic PLA" is still live.
    shadowed = 0
    for old, owners in sorted(claims.items()):
        if old in live_names:
            for v, p, new in owners:
                if old == new:
                    continue
                shadowed += 1
                print("[WARNING] renamed_from %r (of %r, %s) shadows a preset that is still "
                      "live; the alias can never fire" % (old, new, os.path.relpath(p, profiles)))

    print("=" * 52)
    print("Checked %d instantiated preset names, %d rename claims" % (len(by_name), len(claims)))
    if shadowed:
        print("%d inert rename claim(s) shadowed by a live preset (warning only)" % shadowed)
    if errors:
        print("FAILED: %d error(s)" % errors)
    else:
        print("OK: no duplicate names, no ambiguous rename claims")
    print("=" * 52)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
