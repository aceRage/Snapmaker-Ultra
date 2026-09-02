# SDD ledger — plan: docs/superpowers/plans/2026-09-01-color-split.md
(Colour Split — Split by painted colour)

- 2026-09-01: research phase. Three read-only code audits (paint API, model/plater/slicing plumbing, boolean/OpenVDB/jobs infrastructure) + prior-art search; user's brainstorm notes (C:\Dev\SnapmakerOrcaSlicerFixes\docs\colorsplitting_topics.md) found unsuitable (cap-and-boolean gives zero-volume pieces on flat faces). Findings: docs/superpowers/specs/2026-09-01-color-split-research.md.
- 2026-09-01: user decisions — engine A (exact shells + Manifold), depth mirrors paint-depth, parts in the same object, spike first in the new worktree.
- 2026-09-01: worktree C:\Dev\SnapmakerOrcaNext switched to feat/color-split off origin/main dff2c65eab (paint-depth merge). Build tree in build/ is stale (built from feat/paint-depth); build slot was busy (another session) so no rebuild started.
- 2026-09-01: spec rev 1 → adversarial review (see spec-review.md) → rev 2 committed d6f4e79566. Rev 2 adds: half-thickness clamp d(v)=min(D,t/2) + fold guard + CGAL self-intersection check (C1), pinch-vertex duplication (I1), live 2D cap rule tanθ<h/(3ws) and N_eff cap depth incl. N=0 (I2/I3), new volumes via public add_volume + rotation-safe offset + reset of text/emboss/cut info (I4/I5/I7), empty body handling (I6), job id/timestamp re-check + gizmo guard (I8), paint_infill_override mirror (I9), wall-stack step at horizontal-painted/vertical-neighbour boundaries (I10), sequential Manifold::Split partition + enclosed-island absorption (I11 + C1), depth inputs spelled out (I12), MeshGL64/tolerance/Volume() (M6), ExecutionContext cancel (M7).
- NEXT: user reviews the spec (brainstorming gate) → writing-plans → spike S1–S4 as plan task 1 (needs a rebuild of build/ on the new branch; check the build slot first).
- 2026-09-01: user said "Continue" on the spec review gate → spec rev 2.1 (§3.6 crease rule extended with the painted-side/more-horizontal-neighbour case) and implementation plan docs/superpowers/plans/2026-09-01-color-split.md written (9 tasks; Tasks 1-4 double as the spike with a decision checkpoint in Task 4 step 5). Execution mode (subagent-driven vs inline) pending user choice. Build tree still stale; build slot busy at last check.

## Pre-flight scan (2026-09-01, before Task 1)

| Pair / task | Produces vs consumes | Finding |
|---|---|---|
| T1 → T2 | ColorPatches{surface, facet_state, states} / compute_vertex_depths(const ColorPatches&, normals, D) | consistent |
| T2 → T3 | color_split_normals(its), compute_vertex_depths(...) / build_color_shells calls both | consistent after Ruling 1 (per-state depth call) |
| T3 → T4 | ColorShell{state, capped, mesh}, build_color_shells(patches, depths, params, progress) / partition_by_shells + split_volume_by_paint | consistent |
| T3 ↔ T5 | ShellBuilder internals; T3 tests pin cap/step OFF via no_cap_no_step(), T4 partition tests too | consistent; T4 spike cases use defaults → behaviour changes after T5 by design (numbers re-recorded in T9) |
| T4 S1 boss ↔ T2 depth rule | "whole boss after island absorption" vs d = min(D, t/2) | CONFLICT: with t/2 the boss core stays a body sliver connected to the block → not absorbed; with a full-thickness rule the ruled shell crosses itself (nested shells, Manifold semantics unknown). Ruling 1 below. |
| T3 fold guard area criterion | bottom area < 1e-3 top area | defect: legitimate deep offsets on small spheres collapse to tiny-but-valid triangles and would be halved; Ruling 2 |
| T5 crease Case B | spec rev 2.1 §3.6 | plan text carries both cases; consistent |
| T6 → T8 | apply_color_split(ModelObject&, size_t, ColorSplitResult&&, const ColorSplitSpace&, bool, bool), color_split_space, scale_depths / Plater::split_by_color + ColorSplitJob::finalize | consistent |
| T2 → T8 | color_split_depths(cfg, filaments) / Plater builds filaments from v->get_extruders() | consistent (1-based) |
| T7 | e2e uses ColorSplitParams{} (cap+step on) after T5 | consistent |
| Each task self-consistency | tests vs code | T1 select_patch call flagged "verify overload" (brief carries the real signature) |

Rulings:
- Ruling 1 (depth rule): d(v) = min(D, t(v)/2 − δ), δ = 0.002 mm, computed PER STATE (a vertex's thickness ray is the same, the clamp is universal). Through-painted features (both sides the same state, or a pin/boss) become two half-shells whose bottoms stop δ short of the mid-surface; the 2δ sliver between them is either an enclosed body island (absorbed into the colour, §3.8) or a sub-resolution body sliver the slicer drops. No shell ever crosses the mid-surface, so no nested/inverted shells reach Manifold. — Why: keeps every shell valid by construction and mirrors the 2D Voronoi mid-split; avoids depending on Manifold's undocumented winding semantics. — Cost if wrong: hidden micro-slivers of body colour inside pins (invisible; measured by the S1 boss test at ≤5% volume).
- Ruling 2 (fold guard): the area criterion becomes "bottom area < 1e-6 × top area" (truly collapsed only); the orientation criterion nb·(−nt) ≤ 0 stays. — Cost if wrong: a few more CGAL-check retries.
- Ruling 3 (spec rev 2.2): §3.4 text updated to Ruling 1 in both copies (worktree spec is binding).
- Ruling 4 (models): implementers T1/T2 sonnet, T3–T8 opus; task reviewers sonnet for T1/T2/T6/T7/T9, opus for T3/T4/T5/T8; final whole-branch review fable.
