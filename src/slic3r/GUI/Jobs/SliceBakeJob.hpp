#ifndef slic3r_GUI_SliceBakeJob_hpp_
#define slic3r_GUI_SliceBakeJob_hpp_

// "Bake slice to mesh": the background half of the action.
//
// Spec: docs/superpowers/specs/2026-09-12-slice-bake-research.md (phase 1).
//
// The mesh is built off the UI thread from the SLICED PrintObject, which the worker may read
// because the plate is sliced and the background slicing process is idle (the caller checks both
// before queueing, and the plate is invalidated only in finalize(), on the main thread). What the
// worker produces is a plain indexed_triangle_set; everything that touches the Model happens in
// finalize().

#include "Job.hpp"

#include "slic3r/GUI/SliceBakeDialog.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/SliceBake.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <string>

namespace Slic3r {
class PrintObject;
namespace GUI {

class Plater;

class SliceBakeJob : public Job
{
public:
    // `print_object` is the sliced object to bake. It is only read on the worker thread, and
    // finalize() never touches it - it re-finds the ModelObject by id instead, so a model edited
    // while the bake ran cannot be written through a stale pointer.
    SliceBakeJob(Plater                  *plater,
                 const PrintObject       *print_object,
                 ObjectID                 model_object_id,
                 const SliceBakeSettings &settings,
                 const std::string       &object_name,
                 const std::string       &export_path = {});

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    Plater            *m_plater       = nullptr;
    const PrintObject *m_print_object = nullptr;
    ObjectID           m_object_id;
    SliceBakeSettings  m_settings;
    std::string        m_name;
    std::string        m_export_path;

    indexed_triangle_set m_mesh;
    SliceBakeReport      m_report;
    std::string          m_error;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SliceBakeJob_hpp_
