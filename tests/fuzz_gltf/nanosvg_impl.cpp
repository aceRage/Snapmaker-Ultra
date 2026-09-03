// The nanosvg implementation, alone in its own translation unit.
//
// libslic3r references nsvgParse / nsvgParseFromFile / nsvgDelete but their definitions live in
// the GUI module, which a test binary does not link, so every executable that links libslic3r has
// to provide them. It cannot be done inside fuzz_gltf.cpp: nanosvg's implementation section pulls
// in <windows.h>, whose min/max/GetObject macros then break every libslic3r header that follows.
// tests/libslic3r/libslic3r_tests.cpp only gets away with it because no libslic3r header is
// included after its nanosvg block. Keeping it here makes that independent of include order.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
