#include "SliceBakeJob.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

SliceBakeJob::SliceBakeJob(Plater                  *plater,
                           const PrintObject       *print_object,
                           ObjectID                 model_object_id,
                           const SliceBakeSettings &settings,
                           const std::string       &object_name,
                           const std::string       &export_path)
    : m_plater(plater)
    , m_print_object(print_object)
    , m_object_id(model_object_id)
    , m_settings(settings)
    , m_name(object_name)
    , m_export_path(export_path)
{}

void SliceBakeJob::process(Ctl &ctl)
{
    if (m_print_object == nullptr)
        return;

    const std::string status = _u8L("Baking the slice to a mesh");
    ctl.update_status(0, status);

    try {
        m_mesh = slice_bake_to_mesh(*m_print_object, m_settings.options, &m_report,
                                    [&ctl, &status](int percent) {
                                        ctl.update_status(percent, status);
                                        return ! ctl.was_canceled();
                                    });
    } catch (const SliceBakeCancelled &) {
        return;                               // finalize() sees canceled == true and changes nothing
    } catch (const std::exception &e) {
        // Clipper and the loft can both throw on degenerate input; the failure is reported rather
        // than unwound out through the worker.
        m_error = e.what();
        return;
    }

    ctl.update_status(100, _u8L("Slice baked."));
}

void SliceBakeJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (canceled || eptr)
        return;

    if (! m_error.empty()) {
        show_error(m_plater, from_u8((boost::format(_u8L("Could not bake \"%1%\": %2%")) % m_name % m_error).str()));
        return;
    }
    if (m_mesh.indices.empty()) {
        wxGetApp().notification_manager()->push_plater_warning_notification(
            (boost::format(_u8L("\"%1%\" had no outer wall to bake: %2%")) % m_name
             % (m_report.note.empty() ? _u8L("slice the plate first") : m_report.note)).str());
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "slice_bake: '" << m_name << "' " << m_report.layers_baked << " layers -> "
                            << m_report.triangles << " triangles, "
                            << (m_report.watertight ? "watertight" : "NOT watertight");

    const std::string summary =
        (boost::format(_u8L("Baked %1% layers of \"%2%\" into %3% triangles.")) % m_report.layers_baked % m_name
         % m_report.triangles).str();

    // ---- export ---------------------------------------------------------------------------------
    // Nothing in the scene changes, so there is no snapshot and no plate invalidation.
    if (m_settings.result == SliceBakeResultMode::ExportSTL) {
        if (m_export_path.empty())
            return;                            // the user cancelled the file dialog
        if (! its_write_stl_binary(m_export_path.c_str(), m_name.c_str(), m_mesh)) {
            show_error(m_plater, from_u8((boost::format(_u8L("Could not write %1%.")) % m_export_path).str()));
            return;
        }
        wxGetApp().notification_manager()->push_notification(
            summary + " " + (boost::format(_u8L("Saved to %1%.")) % m_export_path).str());
        return;
    }

    // ---- add as a new object --------------------------------------------------------------------
    // The bake is a fresh object, so nothing about the original is disturbed and the plate's slice
    // stays valid for the original - but the new object is unsliced, so the plate is invalidated
    // all the same, exactly as adding any other object does.
    if (m_settings.result == SliceBakeResultMode::AddNew) {
        Plater::TakeSnapshot snapshot(m_plater, "Bake slice to mesh");
        wxGetApp().obj_list()->load_mesh_object(TriangleMesh(m_mesh), from_u8(m_name + " (baked)"));
        wxGetApp().notification_manager()->push_notification(summary);
        return;
    }

    // ---- replace the object ----------------------------------------------------------------------
    Model &model = m_plater->model();
    int    obj_idx = -1;
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i]->id() == m_object_id) {
            obj_idx = int(i);
            break;
        }
    if (obj_idx < 0) {
        wxGetApp().notification_manager()->push_plater_warning_notification(
            (boost::format(_u8L("\"%1%\" was removed while it was being baked; nothing was changed.")) % m_name).str());
        return;
    }
    ModelObject *object = model.objects[size_t(obj_idx)];

    // One snapshot for the whole replacement, so a single Undo puts the original mesh back.
    Plater::TakeSnapshot snapshot(m_plater, "Bake slice to mesh");

    // The bake's vertex indices bear no relation to the source's, so every per-triangle painted
    // layer (supports, seam, fuzzy skin, MMU colour) would be nonsense on the new mesh. This is
    // the same call repair_by_remesh makes before it replaces a volume's mesh, and it is what
    // drops the paint.
    m_plater->clear_before_change_mesh(obj_idx);

    // The bake replaces the object's model PARTS with one volume. Modifiers, negative volumes and
    // support blockers are left alone: they are placement data the user set up around the part,
    // and the bake does not invalidate them.
    ModelVolume *first_part = nullptr;
    for (ModelVolume *v : object->volumes)
        if (v->is_model_part()) { first_part = v; break; }
    if (first_part == nullptr) {
        wxGetApp().notification_manager()->push_plater_warning_notification(
            (boost::format(_u8L("\"%1%\" has no part to replace.")) % m_name).str());
        return;
    }

    // The bake came back in the OBJECT's frame (SliceBakeOptions::in_object_frame), i.e. in the
    // same space the source volume's own mesh lives in once its volume transform is applied. The
    // source volume's transform is therefore reset to identity and the mesh takes its place
    // verbatim - which is what keeps the object where it was.
    //
    // Every part after the first is dropped: the bake is ONE solid covering all of them (the
    // per-layer union ran over every region of every layer, whatever volume produced it), so
    // keeping the others would leave duplicate geometry inside the bake.
    std::vector<ModelVolume *> keep;
    for (ModelVolume *v : object->volumes)
        if (! v->is_model_part())
            keep.push_back(v);

    first_part->set_mesh(indexed_triangle_set(m_mesh));
    first_part->set_transformation(Geometry::Transformation());
    first_part->set_new_unique_id();
    first_part->calculate_convex_hull();
    first_part->name = m_name + " (baked)";

    for (auto it = object->volumes.begin(); it != object->volumes.end();) {
        if (*it != first_part && (*it)->is_model_part()) {
            delete *it;
            it = object->volumes.erase(it);
        } else {
            ++it;
        }
    }

    object->invalidate_bounding_box();
    object->ensure_on_bed();
    // The object is no longer what its source file holds (ObjectList::split does the same).
    object->input_file.clear();

    m_plater->changed_mesh(obj_idx);
    m_plater->get_partplate_list().notify_instance_update(obj_idx, 0);
    wxGetApp().obj_list()->update_item_error_icon(obj_idx, -1);
    wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
    wxGetApp().obj_list()->update_plate_values_for_items();

    // The plate's G-code was computed from the OLD mesh; re-slicing has to happen against the new
    // one. changed_mesh() already schedules the background process, but the plate's own
    // "the slice is still valid" flag is what the UI reads, so it is cleared explicitly.
    if (PartPlate *plate = m_plater->get_partplate_list().get_curr_plate())
        plate->update_slice_result_valid_state(false);

    wxGetApp().notification_manager()->push_notification(
        summary + " " + _u8L("Painted data was cleared and the plate needs slicing again."));
}

}} // namespace Slic3r::GUI
