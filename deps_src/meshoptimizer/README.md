# meshoptimizer (vendored, decoder only)

From [meshoptimizer](https://github.com/zeux/meshoptimizer) **v1.2**, taken verbatim from
`https://raw.githubusercontent.com/zeux/meshoptimizer/v1.2/src/` on 2026-09-03.

* Licence: MIT (`LICENSE.md`, kept alongside the sources).
* Only the four files the `EXT_meshopt_compression` decoder needs are vendored:
  `meshoptimizer.h`, `vertexcodec.cpp`, `indexcodec.cpp`, `vertexfilter.cpp`. The encoder,
  simplifier, clusterizer and everything else are deliberately absent.
* Used from `src/libslic3r/Format/GLTF.cpp` only. cgltf recognises the extension and exposes
  `cgltf_buffer_view::meshopt_compression`, but does not decode it; the reader fills
  `cgltf_buffer_view::data` with the decoded bytes, which is the hook cgltf documents for
  exactly this ("overrides buffer->data if present, filled by extensions").

Do not edit these files. To update, drop in a newer upstream release and re-run
`libslic3r_tests "[gltf]"`.
