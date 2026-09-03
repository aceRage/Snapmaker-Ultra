#ifndef slic3r_Format_GLTF_hpp_
#define slic3r_Format_GLTF_hpp_

#include "libslic3r/Color.hpp"

#include <admesh/stl.h>            // ImportstlProgressFn

#include <string>
#include <vector>

namespace Slic3r {

class Model;

// One entry per ModelVolume the reader created, in volume order.
struct GltfPart
{
    std::string name;                // the ModelVolume name we assigned
    int         material_index{-1};  // index into GltfInfo::material_colors, -1 = no material
    size_t      triangles{0};
};

struct GltfInfo
{
    // Stage 2 inputs. material_colors is one sRGB RGBA per glTF material actually used,
    // deduplicated; parts[i].material_index indexes into it.
    std::vector<RGBA>        material_colors;
    std::vector<GltfPart>    parts;
    // Per-vertex COLOR_0, concatenated in volume order, only filled when EVERY drawn
    // primitive has COLOR_0. Parsed in v1, used from v2.
    std::vector<RGBA>        vertex_colors;
    // One sRGB colour per surviving triangle, concatenated in volume order. Filled only when a
    // baseColorTexture was actually sampled; triangles of untextured primitives get their flat
    // material colour so the array always covers every triangle of the object.
    std::vector<RGBA>        face_colors;
    bool                     is_single_material{false};
    bool                     had_textures{false};      // report, never used in v1/v2
    size_t                   dropped_primitives{0};    // points / lines / line loops / strips
    size_t                   skipped_nodes{0};         // over the instance cap
    std::vector<std::string> unsupported_extensions;   // from extensionsRequired
};

// Load a .glb or .gltf into `model` as exactly one ModelObject.
// Returns false and sets `message` (already translated) on every failure - never returns
// false with an empty message, because Model::read_from_file would then throw the generic
// "Loading of a model file failed." instead of something the user can act on.
extern bool load_gltf(const char *path, Model *model, GltfInfo &info, std::string &message,
                      const char *object_name = nullptr, ImportstlProgressFn progressFn = nullptr);

} // namespace Slic3r

#endif /* slic3r_Format_GLTF_hpp_ */
