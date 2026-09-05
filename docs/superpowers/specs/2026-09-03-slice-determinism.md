# Slice determinism - why the same project sliced twice gave different G-code

Date: 2026-09-03 · Branch: `fix/slice-determinism` (cut from `feat/ultra-preferences`)

## 1. Symptom

Slicing one project twice through the CLI, with the **same executable** and the same arguments,
gave different G-code in roughly a third of comparisons. Two shapes:

1. **On all cores**, whole blocks moved and support cases drifted by hundreds or thousands of
   lines. The support-groups work could only get a byte-identity gate to pass by pinning the
   process to one CPU - `scripts/support_group_identity.py` does that and says so in its docstring -
   and by normalising the `id:<N>` token of the `; printing object` comments away.
2. **Even pinned to one CPU**, a block of ~40 lines was present in one run and absent in the next.
   Bistable, never drifting: the corpus' `no_support` case flipped between two stable outputs of
   4678 and 4641 lines, with support off, the classic wall generator and a single object.

Almost none of it is a race in the ordinary sense. Four of the seven causes below are
**uninitialised memory being read**, one is a container keyed by heap addresses, and two are
containers whose order is the order the worker threads happened to arrive in.

## 2. Diagnosis path

**Reproduce.** `no_support` (`tests/data/support_corpus/overhang_ledge.3mf`, `--enable-support=0`),
pinned to one CPU: 20 runs gave 19 × 4678 lines and 1 × 4641 - the same two hashes as reported.

**Read the difference.** Diffing the two stable outputs showed twenty hunks, all the same shape: a
travel move, a `;WIDTH:` comment and two extrusion segments about 0.05 mm long, sitting at the end
of a `;TYPE:Internal solid infill` block, followed by a different retract and wipe because the wipe
retraces the last extrusion. Variable width inside solid infill means `ipConcentricInternal` - the
narrow-internal-solid-infill pattern, which is driven by Arachne even when the wall generator is
`classic`.

**Bisect the pipeline.** `DeterminismDump.hpp` (added on this branch): with `ORCA_DET_DUMP` set to
a directory the slicer writes a byte-stable text dump of every layer's slices, fill surfaces and
extrusions after slicing, `make_perimeters`, `prepare_infill` and `infill`. Slicing until two
different outputs appeared and diffing their dumps:

```
obj0_2_make_perimeters_extrusions.txt    same
obj0_2_make_perimeters_surfaces.txt      same
obj0_3_prepare_infill_surfaces.txt       same
obj0_4_infill_extrusions.txt             DIFFERS
```

So the difference is introduced inside `PrintObject::infill()` → `Layer::make_fills()`, from
identical inputs. In the dump it reads `collection n=2 no_sort=1` against `collection n=1`, and a
`no_sort` collection of variable-width paths is what `FillConcentricInternal` emits.

**Confirm the path without rebuilding.** `--detect-narrow-internal-solid-infill=0` removes the
`ipConcentricInternal` pattern entirely: 60 consecutive slices, byte-identical.

**Two observations that named the mechanism.** With `ORCA_DET_DUMP` set, the same binary was
byte-identical over 60 runs; unset, it flipped within 30. And on one CPU every one of the twenty
beads flipped *together*, while on all cores only some of them did. A value read from memory that
nobody wrote behaves exactly like that: one thread reuses one stack slot for every layer it fills,
so every decision on that thread agrees; different threads have different stacks, so they
disagree; and any extra allocation upstream moves the frames and changes the garbage.

**The read.** `WallToolPaths::removeSmallLines()` decides whether a thin open bead survives:

```cpp
if (line.is_odd && !line.is_closed &&
    shorterThan(line, m_params.is_top_or_bottom_layer ? (min_width / 2)
                                                      : (min_width * m_params.min_length_factor)))
```

`m_params` is a `WallToolPathsParams`. The class had **no default member initialisers**, and
`FillConcentricInternal::fill_surface_extrusion()` (like `FillConcentric`) default-constructs one
and sets only the six Arachne parameters it cares about - not `min_length_factor`, not
`is_top_or_bottom_layer`. `PerimeterGenerator` is unaffected because it goes through
`Arachne::make_paths_params()`, which fills both in.

The support cases were then peeled off the same way: run the gate, look at the first differing
line, and ask whether the case still differs when the process is pinned to one CPU. A case that
differs on one CPU is an address or an uninitialised read; a case that only differs on many cores
is thread order.

## 3. Root causes and fixes

### 3.1 `PrintObject::m_id`, `PrintInstance::id`, `PrintInstance::unique_id` - uninitialised

`; printing object <name> id:<N> copy <n>` and the firmware exclude-object definitions name each
object and instance by these counters. All three were uninitialised members, and the only place
that assigned them was `GCode::set_object_info()`, which runs only when the exclude-object feature
is on and returns early for BBL printers and for G-code flavours without such a feature. On every
other printer the labels printed whatever the heap held: `id:0` in one run and
`id:5495884996896775260` in the next. Five of the ten gate cases failed on this line alone.

**Fix** (`eec7c0538f`): `GCode::assign_object_and_instance_ids()` numbers the objects and their
instances in print order, once per export, before anything can read a number; `set_object_info()`
now only reports the numbers. All three members default to 0.

### 3.2 `WallToolPathsParams` - uninitialised, and read by a geometry decision

As in §2. **Fix** (`fee2ee0ba5`): default member initialisers for every field, using the values
`make_paths_params()` falls back to (`min_length_factor` 0.5, `is_top_or_bottom_layer` false).
This is the ~40-line block, and the same signature on `normal_grid` and `raft`.

### 3.3 `SupportNode::movement` and `SupportNode::skin_direction` - uninitialised

`Point` is an Eigen vector and Eigen does not initialise one. `TreeSupportData::create_node()`
assigns `movement` only when the node has a parent, and `skin_direction` is only inherited from a
parent or set for a sharp-tail contact node - yet `movement` is read when the branch is drawn
(`TreeSupport.cpp:2050`) and `skin_direction` when a sharp tail decides which way to lean
(`TreeSupport.cpp:2845`). Classic tree support differed by ~1200 lines between two runs.

**Fix** (`19630b5629`): both members default to `{0, 0}`. That closed most of the gap on
`tree_classic` - ~1200 lines down to ~90 - but not all of it; §3.4 is the rest.

### 3.4 `MinimumSpanningTree::prim()` - ties broken by heap address

The candidate set was two `std::unordered_map`s keyed by `const Point*`, and the next vertex was
picked with `std::min_element` over one of them. An `unordered_map` keyed by a pointer iterates in
the hash order of heap addresses, so every tie in the distance - and ties are the normal case for
support points sitting on a grid - was broken by where the vertices happened to be allocated.
Classic tree support therefore put its branches in different places from one run to the next.

**Fix** (`cbb1f0d791`): keep the candidates in index order and scan them by index, so a tie goes to
the lowest index. Same O(V) scan, no hashing. With it, `tree_classic` is byte-identical over 12
consecutive slices pinned to one CPU; it differed on every pass before.

### 3.5 `bridge_over_infill` - candidate order is thread-arrival order

The internal-bridge candidates were accumulated into one shared `tbb::concurrent_vector` from a
`parallel_for` and then drained in container order, which is the order the workers arrived in. The
expansion that follows walks a layer's candidates in sequence and each one cuts the next, so the
internal bridges changed with the number of cores.

**Fix** (`12274d8264`): one bucket per layer. Each task writes only to the layers it owns, so the
result no longer depends on how the range was split.

### 3.6 Tree support's contact nodes summed in thread-arrival order

`generate_contact_points()` appended every contact node's position into one shared
`tbb::concurrent_vector` and then summed those positions into `float` accumulators to fit a line
through them - the angle the whole tree support is oriented around. Float addition is not
associative, so that angle depended on the order the workers appended in. The same loop also
incremented a plain `int` from every worker, which is a data race, and that count divides the node
total.

**Fix** (`cf81cf984b`): one bucket per layer, concatenated in layer order, and the count taken
there.

### 3.7 Two comparators that were not strict weak orderings

- `PrintObject::bridge_over_infill()` sorted line sections with `a.a.y() < b.b.y()`, mixing one
  line's start with another line's end. `std::sort` with such a predicate is undefined behaviour.
  The sections of one vertical line are disjoint by construction, so ordering them by their own two
  endpoints gives the same result and is valid (`12274d8264`).
- `Print::apply()`'s `region_set` used a `std::set` whose comparator returned *equality*
  (`l == r`), which leaves a red-black tree free to do anything (`212a8a59e3`).

### 3.8 Lightning infill's tree traversal used `rand()`

`Fill/Lightning/TreeNode.cpp`'s `convertToPolylines()` chose which child starts a polyline with
`rand() % m_children.size()`. The C runtime's generator is per thread and seeded to 1, so the
sequence depends on how many times that thread had already used it - i.e. on which layers the
thread happened to get. **Fix** (`d5c82f654e`): derive the choice from the node's own position.
Not covered by the corpus, which slices with grid infill.

## 4. Verification

`C:\Users\acesa\AppData\Local\Temp\snorca_hubtest\det_gate.py` slices every case of
`tests/data/support_corpus/corpus.json` three times with one executable and requires the G-code to
be byte-identical. Unlike `scripts/support_group_identity.py` it runs **on all cores**, normalises
**only** the `; generated by <version> on <timestamp>` header - the object-id token is compared
like any other byte now - and adds a two-**object** plate case
(`tests/data/support_corpus/two_objects.3mf`, two build items of different heights) on top of the
nine single-object corpus cases.

```
$ python det_gate.py --passes 3
exe    : C:\Dev\SnapmakerOrcaPhone\build\Snapmaker_Orca\snapmaker-orca.exe
mode   : ALL CORES, 3 passes, 10 case(s)

normal_grid          ok      9927 lines, 3 passes, 14s
normal_snug          ok      5593 lines, 3 passes, 20s
normal_ledge         ok      6187 lines, 3 passes, 17s
soluble_interface    ok      43311 lines, 3 passes, 16s
dense_interface      ok      10499 lines, 3 passes, 18s
tree_organic         ok      52808 lines, 3 passes, 25s
tree_classic         DIFFER  plate_1.gcode first differs at line 145
raft                 ok      10423 lines, 3 passes, 18s
no_support           ok      4678 lines, 3 passes, 29s
two_objects          ok      6231 lines, 3 passes, 15s

RESULT: 1 case(s) not reproducible
```

Nine of the ten cases - the two-object plate, both interface cases and organic tree support
included - are byte-identical across three passes **on all cores**, object-id token and all.
Before this branch the same gate failed six of ten on the object-id line alone, and `no_support`,
`normal_grid` and `raft` failed on the Arachne bead as well.

The same gate pinned to one CPU passes **all ten**:

```
$ python det_gate.py --passes 3 --one-cpu
mode   : one CPU, 3 passes, 10 case(s)
... tree_classic         ok      49950 lines, 3 passes, 16s ...
RESULT: byte-identical across 3 passes on all 10 case(s)
```

One thing the gate cannot see, because every pass runs the same way: `tree_organic` gives 52808
lines on all cores and 52816 on one CPU. It is reproducible *for a given number of cores* but not
*across* them, so the same project sliced on a machine with a different core count still gives
different G-code. `tree_classic` is the same, more loudly. Every other case gives the same byte
count in both modes.

Spot measurements behind the individual fixes:

| Case | Before | After |
|---|---|---|
| `no_support`, one CPU | 2 distinct outputs in 20 runs (4678 / 4641 lines) | 40 runs identical |
| `tree_classic`, one CPU | differed on every pass | 12 runs identical |
| `tree_classic`, all cores | ~1200 lines apart | 6+ distinct outputs in 8 runs; see §4.1 |

### 4.0 Unit tests

The build tree was reconfigured with `-DBUILD_TESTS=ON` (it was off) and the suite built and run:

```
$ build/tests/libslic3r/Release/libslic3r_tests.exe
test cases:   584 |   582 passed | 2 failed as expected
assertions: 52612 | 52610 passed | 2 failed as expected
```

`tests/fff_print` was built and run too, and fails - but it fails for a reason that predates this
branch and has nothing to do with it: the suite still uses PrusaSlicer's config key names, so
`Slic3r::Test::init_print()` throws `Unknown option exception: first_layer_extrusion_width` (this
fork calls it `initial_layer_line_width`). Every fff_print failure hangs off that fixture,
including the SIGSEGV in `test_skirt_brim.cpp`, and the `[Flow]` case that fails cannot be reached
by anything changed here.

### 4.1 The one case that is still not reproducible: `tree_classic` on more than one core

After the fixes above, `tree_classic` (`--support-type=tree(auto) --support-style=tree_slim`) is
byte-identical over 12 consecutive slices **pinned to one CPU** and produces six or more distinct
outputs in eight slices **on all cores**. What is left is thread order, not addresses.

`TreeSupport::drop_nodes()` walks the layers from the top down - each layer depends on the one
above, so the layer loop is sequential - and its only parallelism is two `tbb::parallel_for_each`
passes over the nodes of one layer. Both mutate state shared between the nodes they visit:

- the first pass merges nodes that are close together: it appends to `node.merged_neighbours` and
  sets `neighbour_node->valid = false` under `m_ts_data->m_mutex`, so **which of two neighbouring
  nodes wins the merge is decided by which thread gets there first**;
- the second pass creates the next layer's nodes, which are appended to
  `contact_nodes[layer_nr - 1]` in the order the tasks finish.

**The fix I would make**, and did not make here because it is a behavioural change to tree support
that wants its own change and its own timing measurements: split each pass into a parallel
read-only phase that records what each node *would* do, and a short sequential phase that applies
those decisions in `nodes_vec` order. Running the two passes sequentially as they stand would also
be deterministic and is a two-line change, but it removes the only parallelism `drop_nodes` has.

## 5. What is still order- or seed-dependent

- ~~**Classic tree support on more than one core**~~ - §4.1. **Fixed on
  `fix/tree-support-determinism`; see §7.** It no longer needs a CPU pin.
- **Organic tree support (`TreeSupport3D`) depends on the core count.** It is byte-identical over
  three passes on all cores and over three passes on one CPU, but the two answers differ (52808
  against 52816 lines). Not chased here; it is the same family of question as §4.1 and would need
  the same kind of look at that generator's parallel sections. Still true after §7, and measured
  there: 58.5 % of segments match between a 1-thread and a 20-thread slice.
- **Lightning infill's SVG-debug filename helper** still calls `srand(time(NULL))`
  (`Fill/Lightning/Generator.cpp`), reseeding that thread's C-runtime generator from the wall
  clock. The generator's own `rand()` in the tree traversal is fixed (§3.8); the `srand` is left
  because the helper is debug-only, but it is a landmine for anything else on that thread.
- **Infill rotate templates.** `calculate_infill_rotation_angle()` has `~` and `^` forms whose
  angle is `rand()`. That randomness is the feature the user asked for, but a project using them
  does not slice reproducibly.
- **Custom G-code `random()`.** `GCode.cpp` seeds the placeholder parser's RNG from
  `high_resolution_clock::now()`. Only affects custom G-code that calls `random()`.
- **Multi-nozzle filament assignment.** `MultiNozzleUtils.cpp`'s
  `find_optimal_physical_assignment()` stops searching when a wall-clock deadline expires and
  returns the best found so far, so its answer depends on how fast the machine was.
- **`Print::apply()` never inserts into `region_set`.** The lookup therefore always misses and
  every region keeps its own id, so identical regions are not deduplicated across objects. Fixing
  that would renumber regions and change output, so it is only noted here.
- **`sort_object_instances_by_model_order()`** sorts on `ModelInstance::arrange_order`, which is 0
  for every instance loaded from a plain 3MF through the CLI, and then looks instances up with
  `std::lower_bound` on that all-equal key. It is only reached when `print_order` is not
  `Default`; the default path uses `chain_print_object_instances()`, a nearest-neighbour chain that
  is a function of the instance positions alone.
- **`holePropagationInfos`** in `TreeSupport.cpp` keys a `std::map` on `const Polygon*` pointing
  into a `Polygons` vector that is `push_back`-ed to in the same loop, so the keys dangle as soon
  as the vector reallocates. Not observed to fire on the corpus, but it is the same class of
  address-dependent behaviour.
- **The corpus still passes `--disable-m73=1 --slow-down-layer-time=0`**, because the print-time
  estimate used to wobble between runs and shift whole blocks of the file. Whether that wobble
  survives these fixes was not re-measured.

## 6. The tools

- `src/libslic3r/DeterminismDump.hpp` - `ORCA_DET_DUMP=<dir>` writes the per-stage dumps described
  in §2. Nothing runs unless the variable is set; the `getenv` is read once into a static.
- `C:\Users\acesa\AppData\Local\Temp\snorca_hubtest\det_gate.py` - the gate of §4.
- `det_probe.py` - slice one case N times with one exe and count distinct outputs
  (`--all-cores`, `--extra=<slicer arg>` for ablations).
- `det_bisect.py` - slice until two different outputs appear, then report the first `ORCA_DET_DUMP`
  stage whose file differs between them.
- `make_twoobj.py` - regenerates `tests/data/support_corpus/two_objects.3mf`.

## 7. Follow-up: classic tree support across thread counts (done 2026-09-04)

Branch `fix/tree-support-determinism`, cut from `feat/ultra-preferences` at `eadc7ec8ed`.

§4.1 left one case open. Classic tree support was byte-identical only with the process pinned to
one CPU, because `TreeSupport::drop_nodes()` merged nodes and appended the next layer's nodes from
two `tbb::parallel_for_each` passes in whatever order the workers arrived in. The plan recorded
there - a parallel read-only phase followed by a short sequential apply phase in `nodes_vec` order -
is what landed, plus one cause the plan had not spotted (the grouping map, §7.1). It costs nothing
measurable (§7.4).

### 7.1 What was found

Line numbers are on the pre-fix tree, `eadc7ec8ed`.

- `Support/TreeSupport.cpp:2543` - `nodes_per_part` was a
  `std::vector<std::unordered_map<Point, SupportNode*, PointHash>>`. Its iteration order is the
  vertex order of the layer's minimum spanning tree, and `prim()` (after §3.4) breaks a tie in the
  distance by the **lowest vertex index** - so the shape of the tree was a function of the order in
  which the nodes had been appended to `contact_nodes`, which was thread-arrival order from the
  layer above. That is what makes the whole thing cascade: every layer's tree depended on the
  previous layer's push order.
- `TreeSupport.cpp:2618` and `:2705` - the two `tbb::parallel_for_each` passes, both mutating state
  shared between the nodes they visit.
- `TreeSupport.cpp:2642` - pass one's polygon-node branch sets `neighbour_node->valid = false` with
  **no lock at all**. A plain data race, not merely an ordering question.
- `TreeSupport.cpp:2685-2695` - pass one's "absorb close neighbours" merge takes
  `m_ts_data->m_mutex` and then asks `if (p_node->valid)`. Where two neighbouring nodes each want
  to absorb the other, the winner is whichever worker reached the mutex first.
- `TreeSupport.cpp:2673` - pass one's pair merge appends the replacement node to
  `contact_nodes[layer_nr_next]` in arrival order.
- `TreeSupport.cpp:2725` and `:2875` - pass two appends the next layer's nodes in the order the
  tasks finish. That vector is the next layer's grouping input **and** the order `smooth_nodes()`
  walks the branches in.
- `TreeSupport.cpp:2746` / `:2753` against `:2773` - pass two clears `p_node->valid` from one worker
  while another worker reads `neighbour_node->valid` to decide whether to move towards that node. A
  node could be pulled towards a neighbour that was about to be deleted, or not, purely on timing.
- `TreeSupport.cpp:2629`, `:2647`, `:2660`, `:2685`, `:2772` - `nodes_this_part[neighbour]` is
  `std::unordered_map::operator[]`, a non-const, potentially-inserting call, made from inside a
  `parallel_for_each` over that same map. For a key that was not there it also created a null entry
  that the very next line dereferenced.

### 7.2 What changed

`src/libslic3r/Support/TreeSupport.cpp`, `drop_nodes()`:

- `nodes_per_part` is now a `std::map<Point, SupportNode*>` (a local `NodesByPoint` alias), ordered
  by `(x, y)`. The MST's vertex order, and so its tie-breaking, is a function of the geometry alone
  rather than of hashing or of the previous layer's push order - on any standard library.
- Both passes are split into a **parallel phase that only reads**, recording what each node would
  do (`TreeMergePlan` / `TreeMovePlan`, in an anonymous namespace above the function), and a
  **sequential phase applying those decisions in `nodes_vec` order**. Pass two needs three: 2a
  decides which nodes die and cuts the polygon nodes' overhangs, 2b applies the deaths, 2c computes
  every survivor's movement, 2d creates the next layer's nodes. The expensive geometry -
  `projection_onto`, `move_out_expolys`, `diff_clipped`, the line/contour intersections - all stays
  in the parallel phases; the sequential phases only allocate nodes and push pointers.
- The two mutex sections and the unlocked `valid` write are gone. Nothing in a parallel phase now
  writes anything another task reads.
- Neighbour lookups go through a `node_at()` helper that uses `find()` and returns `nullptr`.
- One deliberate change beyond ordering: splitting pass two at 2b means every surviving node sees
  the **same settled** validity picture when it decides where to move, instead of whatever its
  worker happened to observe mid-flight. That is the only way to make the decision well defined at
  all, but it is a behaviour change and not just a reordering.

`src/libslic3r/Thread.cpp:222` - not a determinism bug, but it blocked the test.
`name_tbb_thread_pool_threads_set_locale()`, which `Print::process()` calls once, spawns one task
per hardware thread and makes each of them wait until **all** of them are running. Under a
`tbb::global_control` parallelism cap - exactly what a test that wants to slice on one thread
installs - fewer tasks than that can ever run at once, and the first one waits for ever. The count
is now clamped to `tbb::global_control::active_value(max_allowed_parallelism)`. Anything that caps
TBB before the first slice used to deadlock; now it does not.

### 7.3 Determinism evidence

`snorca_hubtest\det_tree_gate.py` slices each case once per thread count and compares the G-code
byte for byte against the first pass (only the `; generated by` header is dropped). The thread count
is set by the process affinity mask, which a child inherits and which is what TBB reads for its
arena concurrency, so 1, 2, 4 and 20 CPUs really are 1, 2, 4 and 20 workers. When bytes differ the
pair also goes through `tests/slice_compare_cli`, which is what separates the two kinds of failure
below.

Tree cases, `--threads 1,2,4,20`:

| case | model | before | after |
|---|---|---|---|
| `tree_classic_bridge` | `twopart_bridge.3mf` | DIFFER at 1 vs 4 - 14.5 % of segments matching, 49 dirty layers | **identical**, 50317 lines |
| `tree_classic_ledge` | `overhang_ledge.3mf` | DIFFER at 1 vs 2 - 30.5 %, 29 dirty layers | **identical**, 25662 lines |
| `tree_classic_strong` | `twopart_bridge.3mf`, `tree_strong` | identical | identical, 51463 lines |
| `tree_classic_hybrid` | `overhang_ledge.3mf`, `tree_hybrid` | identical | identical, 5729 lines |
| `tree_organic` | `twopart_bridge.3mf`, `organic` | DIFFER at 1 vs 20 - 58.5 %, 47 dirty layers | **still DIFFER**, unchanged - `TreeSupport3D`, §7.6 |
| `handy_benchy` | `3DBenchy.3mf` | DIFFER at 1 vs 2 - 50.8 %, 181 dirty layers | **identical**, 350664 lines |
| `handy_bunny` | `Stanford_Bunny.3mf` | DIFFER at 1 vs 2 - 64.2 %, 488 dirty layers | **identical**, 1088276 lines |
| `handy_stringhell` | `Orca_stringhell.3mf` | DIFFER at 1 vs 4 | TRAVEL only - §7.6 |
| `handy_ksr` | `ksr_fdmtest_v4.3mf` | DIFFER at 1 vs 2 - 24.0 %, 249 dirty layers | TRAVEL only - §7.6 |
| `handy_tolerance` | `OrcaToleranceTest.stl` | identical | identical, 25582 lines |

Every case whose **geometry** used to move now produces identical geometry at all four thread
counts, and seven of the ten are byte-identical. The remaining geometry difference is organic tree
support, a different generator; the two "TRAVEL" rows print exactly the same plastic in a different
visiting order and are not the support generator at all (§7.6).

`det_tree_gate.py --corpus` runs `corpus.json`'s own nine cases at the same four thread counts, as a
regression check on the paths this change does not touch: **eight of nine byte-identical at 1, 2, 4
and 20 threads**, the ninth being `tree_organic` again. That is a stronger statement than §4's,
which only ever compared runs at one thread count.

Two things worth recording. The handy models slice through the CLI perfectly well now - the "every
.3mf under `resources/handy_models/` segfaults the CLI on `--slice`" note in
`tests/data/support_corpus/corpus.json` is stale. And `handy_stringhell` used to differ only in the
I/J of a `G3` Z-lift arc, with the layers already 100 % identical; even a difference the tolerance
gate passes was downstream of `drop_nodes()`.

### 7.4 Cost

Interleaved A/B on the 20-core machine (`snorca_hubtest\det_tree_ab.py`: 7 runs of each executable,
alternated case by case so both sides meet the same background load; the figure is the minimum, the
run that got the least interference). Interleaving is not fussiness - this machine runs other
people's builds, and a naive "measure A, then measure B" pass reported a spurious 1.6x slowdown that
vanished entirely once the runs were alternated.

| case | before | after | ratio |
|---|---|---|---|
| `tree_classic_bridge` | 3.39 s | 3.22 s | 0.95x |
| `tree_classic_ledge` | 2.31 s | 2.27 s | 0.98x |
| `tree_classic_strong` | 2.42 s | 2.55 s | 1.05x |
| `tree_classic_hybrid` | 2.18 s | 2.27 s | 1.04x |
| `handy_benchy` | 6.17 s | 5.98 s | 0.97x |
| `handy_bunny` | 7.03 s | 6.95 s | 0.99x |
| `handy_stringhell` | 2.77 s | 2.75 s | 0.99x |
| `handy_ksr` | 6.05 s | 6.13 s | 1.01x |
| **total** | **32.32 s** | **32.12 s** | **0.99x** |

**The fix costs nothing measurable** - 0.99x overall, every case within ±5 %, which is inside the
run-to-run noise. That is worth explaining rather than asserting: the sequential phases only
allocate nodes and push pointers, while every geometric call - all of the actual work - stayed
inside a `tbb::parallel_for`. The cheaper alternative §4.1 offered (run both passes sequentially, a
two-line change) was therefore neither needed nor taken; it would have serialised `projection_onto`
and `move_out_expolys`, which dominate `drop_nodes()`.

The output is the same size, so the comparison is like for like
(`snorca_hubtest\det_tree_size.py`, all cores): filament used agrees to within 0.3 % on every case
(bunny 106.19 g against 105.89 g, ksr 48.72 g against 48.96 g) and the G-code line counts to within
0.5 %. The fix picks different tie-breaks, not a different amount of support.

### 7.5 Tests

`tests/fff_print/test_tree_support_determinism.cpp`, tag `[TreeSupportDeterminism]`:

- *"classic tree support is identical on one thread and on many"* - slices an 80 x 50 mm two-piece
  deck on an off-centre 10 x 30 mm pillar with `tree(auto)` / `tree_slim` under
  `tbb::global_control(max_allowed_parallelism, 1)` and then `32`, and compares every support
  layer's `print_z`, island count and support extrusion points. 1.9 s.
- *"classic tree support is identical across several thread counts"* - the same digest at 1, 2, 4
  and 20. 5.2 s.
- **Negative control**: with `TreeSupport.cpp` reverted to `eadc7ec8ed` and everything else in
  place, all four comparisons fail. Two properties of the fixture had to be found before it would
  discriminate, and both are commented in the file: the overhang must be **asymmetric** (a
  symmetric cap resolves its ties the same way however the work is split), and the deck must be
  **big** - at the corpus' 34 x 16 mm a layer holds only a few dozen contact nodes, too few for
  `tbb::parallel_for_each` to split, so the old code ran the passes on one thread whatever the cap
  said and passed. `require_real_support()` pins the digest size for the same reason: a fixture that
  quietly stopped producing support would otherwise pass on two identical empty digests.
- Built on `DynamicPrintConfig::full_print_config()` and `Print::process()`, not on
  `Slic3r::Test::init_print()`, which still throws on this fork's config key names (§4.0).

Suites, on the fixed build:

- `libslic3r_tests`: 599 cases, 597 passed, 2 failed as expected. No change.
- `fff_print_tests`: unchanged from before this branch - 9 of 18 cases fail and the run ends in the
  `test_skirt_brim.cpp:32` SIGSEGV, all of it hanging off the `init_print` fixture described in
  §4.0. Because that crash aborts the run, `[TreeSupportDeterminism]` and `[SupportGroups]` have to
  be invoked by tag; both pass.

Helpers, all in `snorca_hubtest` and none committed:

- `det_tree_gate.py` - the gate of §7.3. `--threads`, `--only`, `--corpus`, `--list`. Repeating a
  thread count (`--threads 20,20,20`) turns it into a same-machine reproducibility check.
- `det_tree_ab.py` - the interleaved A/B timer of §7.4.
- `det_tree_size.py` - the like-for-like output size comparison.
- `det_tree_time.py` - single-executable timing; `det_tree_ab.py` is the one to trust on a loaded
  machine.

### 7.6 Still open

- **Organic tree support (`TreeSupport3D`) still depends on the core count.** `tree_organic` is the
  one case that still differs in geometry - 58.5 % matching segments over 47 dirty layers, exactly
  as before. A separate generator with its own parallel sections; out of scope here. §5's entry
  stands.
- **Small islands are visited in a different order from one run to the next** - newly measured here,
  and *not* a tree support problem. On `ksr_fdmtest_v4.3mf` and `Orca_stringhell.3mf` two runs give
  identical extrusions on every layer (`slice_compare_cli`: 461 of 461 layers identical, 701228
  segments both, zero either-only) while ~14 700 lines differ, all of them `G1` travels and the `G3`
  Z-lift arcs that follow them: the little 0.83 mm pillars of the ksr test grid get printed in a
  different order. It reproduces **with `--enable-support=0`** and **at a fixed thread count**
  (`--threads 20,20`), so it is neither the support generator nor thread order - it looks like a tie
  in whatever chains the islands of a layer together. `det_tree_gate.py` reports these as `TRAVEL`
  and does not fail on them; the two `*_nosupport` cases in its list exist to prove the point.
- **`holePropagationInfos`** (`TreeSupport.cpp:2307`) still keys a `std::map` on `const Polygon*`
  pointing into a `Polygons` vector that is `push_back`-ed to in the same loop. Untouched, same
  class of bug, still not observed to fire on this corpus.
- **The two-pass split changes tree geometry.** Every classic tree-support project will slice to
  different G-code than it did before this branch - the same amount of support in a slightly
  different arrangement (§7.4). That is unavoidable for any fix here, and it is the thing a reviewer
  has to accept.
