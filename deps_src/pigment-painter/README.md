# pigment-painter (ImageMap FULL PR1)

Vendored from [OrcaSlicer-ImageMap](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap)
`92548381056dbf72836b0a1bdc455f238218dbfb`. Licensed GPL-3.0 (`COPYING`).

## `lut_wide.png.c` via Git LFS

`lut_wide.png.c` is a ~36.3 MiB generated C array (`38094965` bytes;
`const char lut_wide_png_data[10295238]`) used by the pigment mixer.

It is **tracked with Git LFS**, not as a normal git blob. See the repo-root
`.gitattributes` entry:

```
deps_src/pigment-painter/lut_wide.png.c filter=lfs diff=lfs merge=lfs -text
```

### Local clone

```bash
git lfs install
git lfs pull
```

If the pointer file is present but the payload is missing,
`deps_src/pigment-painter/CMakeLists.txt` **FATAL_ERROR**s at configure
(ImageMap FULL PR5). There is no silent empty LUT. Confirm the real file:

```bash
# pointer is a tiny text file; the real LUT is ~36 MiB
wc -c deps_src/pigment-painter/lut_wide.png.c
# expect: 38094965
```

### CI

Workflows that compile libslic3r / Snapmaker_Orca must materialize LFS objects
**before** cmake configure:

```yaml
- uses: actions/checkout@v5
  with:
    lfs: true
```

or, after a plain checkout:

```bash
git lfs pull
```

`build_orca.yml` and `build_deps.yml` already set `lfs: true`.
`build_all.yml` is updated to do the same.
`build_orca.yml` also verifies the LUT is not still an LFS pointer after checkout.

REAPER / local test hosts: `git lfs pull` **then rebuild** before
`[texturemapping]` and `[paintdepth]`. See `docs/imagemap-full-pr5.md`.

GLTF / tinygltf are **not** part of this vendored set.
