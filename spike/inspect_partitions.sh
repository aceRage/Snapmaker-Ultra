#!/usr/bin/env bash
# spike/inspect_partitions.sh
#
# Chameleon Support Interface Auto-Match — GUI-slice triage helper
# (v2.3 Task 4, spec C9's "gcode emission discriminator" validation aid).
#
# Given ANY sliced gcode file (a real GUI export of mcsupport_test.3mf, or
# a spike/verify_chameleon.sh CLI harness output), prints two reports to
# help a human decide "did the matcher actually do what the log counters
# say it did" BEFORE blaming the vote/gate resolvers for a bad-looking
# slice:
#
#   1. Partition emission blocks: every "; printing object ..." label
#      comment (GCode.cpp's gcode_label_objects marker), paired with the
#      T<n> toolchange that was active when it was emitted and the
#      layer-z it fell on. GCode.cpp emits the chameleon-matched
#      support-interface/base partition for extruder T<n> (if this layer
#      matched anything to it) immediately after that extruder's OWN
#      toolchange, before that same extruder's ordinary per-instance
#      print loop for the layer — so seeing TWO "; printing object X"
#      lines under the same T<n> is expected (one is the chameleon
#      partition block, one is the object's own normal geometry) and is
#      NOT itself a bug. What to actually look for: an object appearing
#      under a T<n> that is NOT that object's usual/home extruder — that
#      is a chameleon match, and the z/T sequence across layers is what
#      shows whether nearest_wall is sector-faithful (few, stable T<n>
#      changes per column) or still striping (T<n> flapping every layer).
#
#   2. Per-tool length summary: total XY extrusion length (mm) of every
#      G1 move seen while the active TYPE tag (";TYPE:...") was "Support"
#      or "Support interface", bucketed by whichever T<n> was active at
#      the time. Assumes relative extrusion mode (M83 — this codebase's
#      only mode for these fixtures), so a G1's own E value is already a
#      per-move delta: "E present and > 0" alone means "this is an
#      extruding move", no running-E-total bookkeeping needed. This is
#      the log-counter cross-check from spec C9: compare these per-tool
#      totals against the "Chameleon support match: ..." summary line's
#      buckets_dropped_min_benefit/buckets_trimmed_cap/free_set_size (see
#      Print.cpp) for the same slice — a tool with plausible printed
#      length here but that never shows up committed in the log (or vice
#      versa) is exactly the kind of resolver-vs-emission mismatch this
#      script exists to surface quickly.
#
# Layer-z marker: supports both gcode flavors seen in this repo's own
# fixtures — Klipper/BBL (";LAYER_CHANGE" then ";Z:<value>", what a real
# GUI export uses) and Marlin/PrusaSlicer-style ("; Z_HEIGHT: <value>",
# what some CLI harness fixtures use). Whichever appears in the file is
# picked up automatically; no flag needed.
#
# This is a plain grep/awk triage AID, not a structural gcode parser or a
# replacement for spike/verify_chameleon.sh's pass/fail checks — read its
# output next to the spec's "expected improvements per symptom" list
# (docs/superpowers/specs/2026-08-30-support-match-v23-design.md) and the
# v23-task-4-report.md GUI handoff section.
#
# Usage:
#   spike/inspect_partitions.sh <gcode-file>
#
# Example:
#   spike/inspect_partitions.sh tests/mcsupport_test.gcode | less
#   spike/inspect_partitions.sh tests/mcsupport_test.gcode | grep 'z=12\.'

set -u

if [ $# -ne 1 ]; then
    echo "usage: $0 <gcode-file>" >&2
    exit 2
fi

GCODE="$1"
if [ ! -f "$GCODE" ]; then
    echo "ERROR: not a file: $GCODE" >&2
    exit 2
fi

echo "=== Partition emission blocks (layer-z / active tool / object label) ==="
echo "=== file: $GCODE ==="
awk '
    # Klipper/BBL flavor: ";LAYER_CHANGE" then ";Z:<value>" on its own line.
    /^;Z:/ { z = $0; sub(/^;Z:/, "", z); next }
    # Marlin/PrusaSlicer-style flavor: "; Z_HEIGHT: <value>".
    /^; Z_HEIGHT: / { z = $0; sub(/^; Z_HEIGHT: /, "", z); next }
    # Toolchange command - strip any trailing "; change extruder" comment,
    # keep only the digits after "T".
    /^T[0-9]/ {
        tool = $0
        sub(/^T/, "", tool)
        sub(/[^0-9].*$/, "", tool)
        next
    }
    /^; printing object / {
        printf "z=%-10s T%-4s %s\n", (z == "" ? "?" : z), (tool == "" ? "?" : tool), $0
    }
' "$GCODE"

echo ""
echo "=== Per-tool length summary (Support + Support interface TYPE-tagged extrusion, mm) ==="
awk '
    /^T[0-9]/ {
        tool = $0
        sub(/^T/, "", tool)
        sub(/[^0-9].*$/, "", tool)
        next
    }
    /^;TYPE:/ {
        t = $0
        sub(/^;TYPE:/, "", t)
        in_support = (t == "Support" || t == "Support interface")
        next
    }
    /^G1 / {
        x = curx; y = cury
        e = 0
        has_xy = 0
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^X-?[0-9.]+/)      { x = substr($i, 2) + 0; has_xy = 1 }
            else if ($i ~ /^Y-?[0-9.]+/) { y = substr($i, 2) + 0; has_xy = 1 }
            else if ($i ~ /^E-?[0-9.]+/) { e = substr($i, 2) + 0 }
        }
        if (in_support && has_xy && e > 0 && tool != "" && curx != "" && cury != "") {
            dx = x - curx; dy = y - cury
            len[tool] += sqrt(dx * dx + dy * dy)
        }
        curx = x; cury = y
        next
    }
    END {
        for (t in len)
            printf "%s %.2f\n", t, len[t]
    }
' "$GCODE" | sort -k1,1n | awk '{ printf "T%-4s %10.2f mm\n", $1, $2 }'
