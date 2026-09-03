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

- **Classic tree support on more than one core** - §4.1. The one corpus case that is not
  byte-identical and the only one that still needs a CPU pin.
- **Organic tree support (`TreeSupport3D`) depends on the core count.** It is byte-identical over
  three passes on all cores and over three passes on one CPU, but the two answers differ (52808
  against 52816 lines). Not chased here; it is the same family of question as §4.1 and would need
  the same kind of look at that generator's parallel sections.
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

## 7. Follow-up (decided 2026-09-03)

Merged into the integration branch as is. The remaining multi-core case, classic tree support's
`TreeSupport::drop_nodes()` merging nodes from two `tbb::parallel_for_each` passes in arrival order
(section 4.1), is deliberately left for a later change of its own: the proposed fix - a parallel
read-only phase followed by a short sequential apply phase in `nodes_vec` order - changes which node
wins a merge and therefore tree geometry and timing, so it wants its own tests and a measurement of
the effect on real tree-supported prints before it lands. Until then classic tree support is
reproducible on one core only; organic tree support stays self-consistent per core count.
