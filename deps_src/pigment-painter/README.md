# pigment-painter (ImageMap vendor)

GPL-3.0 mixer used by ColorSolver for pigment-accurate filament blends.
Sources are vendored from [OrcaSlicer-ImageMap](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap)
at `92548381056dbf72836b0a1bdc455f238218dbfb` (`COPYING` retained).

## LUT download-at-build (`lut_wide.png.c`, ~36 MiB)

ImageMap ships `lut_wide.png.c` as a ~36 MiB generated C array of a 256³ PNG LUT.
**That file is not in this repo.** CMake fetches it at configure time:

```
https://raw.githubusercontent.com/sentientstardust-dev/OrcaSlicer-ImageMap/92548381056dbf72836b0a1bdc455f238218dbfb/deps_src/pigment-painter/lut_wide.png.c
```

- Default destination: `${CMAKE_CURRENT_BINARY_DIR}/lut_wide.png.c` (out of tree).
- If you already have the file, pass `-DULTRA_PIGMENT_LUT_FILE=/path/to/lut_wide.png.c`
  or drop it next to this README (gitignored; do not commit).
- Manual fetch: `cmake -DLUT_URL=<url> -DLUT_OUTPUT=<path> -P scripts/download_pigment_lut.cmake`

### CI

Configure (and therefore the LUT download) needs outbound HTTPS to
`raw.githubusercontent.com`. GitHub Actions has that by default. Air-gapped
builders must pre-seed `ULTRA_PIGMENT_LUT_FILE`. A download under ~30 MiB is
treated as failure.

Do not add `lut_wide.png.c` to git. See `.gitignore`.
