# spike/normalize_strip_slivers.awk
#
# Part of verify_chameleon.sh's normalize() (v2.1 Task 4, item 1b). Strips
# degenerate micro-extrusion blocks whose XY footprint is < 0.2mm in both
# dimensions, so byte-diff regression checks aren't polluted by the
# solid-infill address-layout knife edge documented in
# C:\Dev\SnapmakerOrcaSupports\solid_infill_nondeterminism.md — a
# degenerate corner sliver (~0.3mm zigzag, footprint ~0.09x0.09mm) whose
# very existence in a given slice is heap-layout-dependent, not a real
# geometry change. The group_fills area filter (efb7902553) now removes
# most of these at the source, but this normalize pass is defense-in-depth
# per the doc's own recommendation ("Practical consequences", item 1):
# tolerate the known signature without masking real regressions (any
# genuine geometry change is far larger than 0.2mm).
#
# Definition (plan Task 4, item 1b): a "block" is a maximal run of
# consecutive *extruding* G1 lines (G1 with E and at least one of X/Y),
# bounded by non-extruding MOVES — travel (G0, or G1 with X/Y but no E) or
# retract/prime (G1 with E but no X/Y). Comments, M-codes, and any other
# non-G0/G1 line (e.g. "; LINE_WIDTH: ..." — this slicer emits a fresh
# LINE_WIDTH comment for every flow-width change *within* a single
# continuous path, including ordinary large perimeter/infill loops) are
# NOT boundaries: they are passed through untouched and do not interrupt
# block accumulation. Getting this distinction right matters — treating
# every interleaved comment as a boundary was tried first and wrongly
# fragmented real multi-centimeter loops into many single-segment
# "blocks", stripping legitimate short edges of a large path. Only an
# actual travel/retract G0/G1 move ends a block.
#
# The block's XY bbox includes the position the tool was already at when
# the block started (the last X/Y seen on any prior G0/G1 line) — not
# just the endpoints of its own extruding lines — so a single-segment
# block's true (start->end) length is measured, not a degenerate
# single-point bbox.
#
# Only the extruding G1 lines identified as belonging to a sub-threshold
# block are ever removed; every other buffered line (comments, M-codes)
# is always kept, in its original position. Run this AFTER stripping M73
# lines (see normalize()'s sed pass): M73 progress lines are a G0/G1-free
# comment-like line so they would already be transparent to this pass,
# but stripping them first keeps the two normalize stages independent and
# avoids any coupling between them.

function flush_pending(   i, wx, wy, strip) {
    strip = 0
    if (extrude_count > 0) {
        wx = maxx - minx
        wy = maxy - miny
        if (wx < 0.2 && wy < 0.2) strip = 1
    }
    for (i = 1; i <= bufn; i++) {
        if (strip && is_extrude[i]) continue
        print buf[i]
    }
    bufn = 0
    extrude_count = 0
    have_minx = 0
    have_miny = 0
}

{
    line = $0
    cmd3 = substr(line, 1, 3)   # "G1 " or "G0 "
    if (cmd3 == "G1 " || cmd3 == "G0 ") {
        hasX = 0; hasY = 0; hasE = 0
        n = split(line, f, /[ \t]+/)
        for (i = 2; i <= n; i++) {
            c = substr(f[i], 1, 1)
            if (c == "X") { hasX = 1; xv = substr(f[i], 2) + 0 }
            else if (c == "Y") { hasY = 1; yv = substr(f[i], 2) + 0 }
            else if (c == "E") { hasE = 1 }
        }
        extruding = (cmd3 == "G1 " && hasE && (hasX || hasY))
        if (extruding) {
            if (extrude_count == 0) {
                # Seed the bbox with the pre-block position so a
                # single-segment block measures its true travelled
                # distance, not a degenerate zero-size point.
                if (have_curX) { minx = curX; maxx = curX; have_minx = 1 }
                if (have_curY) { miny = curY; maxy = curY; have_miny = 1 }
            }
            bufn++
            buf[bufn] = line
            is_extrude[bufn] = 1
            extrude_count++
            if (hasX) {
                if (!have_minx || xv < minx) minx = xv
                if (!have_minx || xv > maxx) maxx = xv
                have_minx = 1
            }
            if (hasY) {
                if (!have_miny || yv < miny) miny = yv
                if (!have_miny || yv > maxy) maxy = yv
                have_miny = 1
            }
            if (hasX) { curX = xv; have_curX = 1 }
            if (hasY) { curY = yv; have_curY = 1 }
            next
        } else {
            # Genuine boundary: a travel or retract/prime move.
            flush_pending()
            print line
            if (hasX) { curX = xv; have_curX = 1 }
            if (hasY) { curY = yv; have_curY = 1 }
            next
        }
    } else {
        # Comment / M-code / other non-move line: buffer transparently,
        # does NOT bound a block and is never itself stripped.
        bufn++
        buf[bufn] = line
        is_extrude[bufn] = 0
        next
    }
}

END { flush_pending() }
