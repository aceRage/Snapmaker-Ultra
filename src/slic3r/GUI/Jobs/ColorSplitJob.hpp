#ifndef slic3r_GUI_ColorSplitJob_hpp_
#define slic3r_GUI_ColorSplitJob_hpp_

// "Split by painted colour": the background half of the action (spec 5).
// Spec: docs/superpowers/specs/2026-09-01-color-split-design.md

#include "Job.hpp"

#include "libslic3r/ColorSplit.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

class Plater;

// One painted MODEL_PART to split. Everything the worker thread touches is a COPY taken on the main thread,
// so the model may be edited (or the job cancelled) while the split runs; finalize() re-finds the volume by
// ObjectID and refuses to apply anything if the paint or the mesh moved on in the meantime.
class ColorSplitJob : public Job
{
public:
    struct Target
    {
        ObjectID    object_id;
        ObjectID    volume_id;
        ObjectBase::Timestamp paint_timestamp = 0; // mmu_segmentation_facets.timestamp() when the job was queued
        size_t      mesh_vertices   = 0;    // cheap "the mesh was replaced" guard, with the paint timestamp
        size_t      mesh_indices    = 0;
        std::string name;                   // the volume's name, for the error message

        // Ruling 23: mesh and paint are ALWAYS handed over in MESH space; space.to_split carries the split
        // into world millimetres when the transform is anisotropic (spec 3.9).
        indexed_triangle_set                    mesh;
        TriangleSelector::TriangleSplittingData paint;
        ColorSplitSpace                         space;
        ColorSplitDepths                        depths;   // already scaled for the split space
        ColorSplitParams                        params;   // already scaled for the split space

        ColorSplitResult result;            // filled in by process()
        bool             ok = false;
        std::string      error;
        // Ruling 27(2): a target that had nothing to split is reported as a warning, not as an error box.
        ColorSplitErrorKind error_kind = ColorSplitErrorKind::generic;
    };

    ColorSplitJob(Plater *plater, std::vector<Target> targets, bool solid_interfaces, bool keep_base_sparse_infill);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    Plater             *m_plater = nullptr;
    std::vector<Target> m_targets;
    bool                m_solid_interfaces       = true;
    bool                m_keep_base_sparse_infill = false;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ColorSplitJob_hpp_
