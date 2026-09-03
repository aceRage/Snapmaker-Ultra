# cgltf (vendored)

`cgltf.h` is [cgltf](https://github.com/jkuhlmann/cgltf) **v1.15**, taken verbatim from
<https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.15/cgltf.h> on 2026-09-02.

* Licence: MIT. The full licence text is at the end of `cgltf.h` and must be kept there.
* Single header, no dependencies beyond the C standard library (it embeds JSMN for JSON).
* Exactly one translation unit in this project defines `CGLTF_IMPLEMENTATION`:
  `src/libslic3r/Format/GLTF.cpp`. Do not define it anywhere else.

Do not edit `cgltf.h`. To update, drop in a new upstream release and re-run
`libslic3r_tests "[gltf]"`.
