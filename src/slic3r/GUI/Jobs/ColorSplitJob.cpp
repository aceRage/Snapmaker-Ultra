#include "ColorSplitJob.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Model.hpp"

#include <boost/format.hpp>

#include <algorithm>
#include <optional>

namespace Slic3r { namespace GUI {

namespace {

// The result is worth applying only if it actually carries a mesh: an all-absorbed or all-skipped split
// leaves nothing to put in the source's place, and apply_color_split would be a no-op (ColorSplit.cpp:316).
bool produces_volumes(const ColorSplitResult &r)
{
    if (! r.body.indices.empty())
        return true;
    for (const auto &piece : r.pieces)
        if (! piece.second.indices.empty())
            return true;
    return false;
}

std::string join_lines(const std::vector<std::string> &lines)
{
    std::string out;
    for (const std::string &l : lines) {
        if (l.empty())
            continue;
        if (! out.empty())
            out += "\n";
        out += l;
    }
    return out;
}

} // namespace

ColorSplitJob::ColorSplitJob(Plater *plater, std::vector<Target> targets, bool solid_interfaces, bool keep_base_sparse_infill)
    : m_plater(plater)
    , m_targets(std::move(targets))
    , m_solid_interfaces(solid_interfaces)
    , m_keep_base_sparse_infill(keep_base_sparse_infill)
{}

void ColorSplitJob::process(Ctl &ctl)
{
    const std::string status = _u8L("Splitting by painted colour");
    const int         n      = int(m_targets.size());
    for (int i = 0; i < n; ++i) {
        if (ctl.was_canceled())
            return;                                // a cancel between targets must not start the next one
        Target &t = m_targets[i];
        try {
            // Ruling 23: the mesh and the paint go in as they came off the volume; the library carries the
            // RETRIANGULATED surface into split space with to_split, never the raw mesh.
            t.result = split_volume_by_paint(t.mesh, t.paint, t.depths, t.params,
                                             [&ctl, i, n, &status](int percent) {
                                                 ctl.update_status((100 * i + percent) / n, status);
                                                 return ! ctl.was_canceled();
                                             },
                                             t.space.to_split);
            t.ok = true;
        } catch (const ColorSplitCancelled &) {
            return;                               // finalize() sees canceled == true and changes nothing
        } catch (const std::exception &e) {
            // ColorSplitError and anything Manifold/CGAL throws land here: the target is reported, the
            // remaining targets still get their chance (spec 7).
            t.error = e.what();
        }
        // The copies are only needed by the split itself; release them before the next (possibly large) one.
        t.mesh  = indexed_triangle_set();
        t.paint = TriangleSelector::TriangleSplittingData();
    }
    ctl.update_status(100, status);
}

void ColorSplitJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (canceled || eptr)
        return;

    Model                     &model = m_plater->model();
    std::vector<std::string>   warnings;
    std::vector<std::string>   errors;
    std::vector<ModelVolume *> created_all;
    std::vector<size_t>        touched_objects;
    // Taken lazily: a run in which every target failed must not leave an empty step on the undo stack.
    std::optional<Plater::TakeSnapshot> snapshot;

    for (Target &t : m_targets) {
        if (! t.ok) {
            if (! t.error.empty())
                errors.push_back((boost::format(_u8L("Could not split \"%1%\" by colour: %2%")) % t.name % t.error).str());
            continue;
        }

        // Task 7 note: apply_color_split re-assigns ObjectIDs when an object drops to a single volume, so the
        // object and the volume are looked up afresh for every target instead of being cached before the job.
        ModelObject *object  = nullptr;
        size_t       obj_idx = 0;
        size_t       vol_idx = 0;
        for (size_t oi = 0; oi < model.objects.size() && object == nullptr; ++oi) {
            if (model.objects[oi]->id() != t.object_id)
                continue;
            for (size_t vi = 0; vi < model.objects[oi]->volumes.size(); ++vi)
                if (model.objects[oi]->volumes[vi]->id() == t.volume_id) {
                    object  = model.objects[oi];
                    obj_idx = oi;
                    vol_idx = vi;
                    break;
                }
        }
        const ModelVolume *src = object == nullptr ? nullptr : object->volumes[vol_idx];
        if (src == nullptr || src->mmu_segmentation_facets.timestamp() != t.paint_timestamp ||
            src->mesh().its.vertices.size() != t.mesh_vertices || src->mesh().its.indices.size() != t.mesh_indices) {
            // Nothing of this split is applied, so its own notes would only confuse: report the abort alone.
            warnings.push_back((boost::format(_u8L("\"%1%\" changed while it was being split; it was left as it was.")) % t.name).str());
            continue;
        }
        // From here the split's result is either applied or explained, so its notes are worth showing.
        for (const std::string &w : t.result.warnings)
            warnings.push_back(w);
        if (! produces_volumes(t.result)) {
            warnings.push_back((boost::format(_u8L("The paint on \"%1%\" produced no separate part.")) % t.name).str());
            continue;
        }

        if (! snapshot)
            snapshot.emplace(m_plater, "Split by painted colour");
        std::vector<ModelVolume *> created = apply_color_split(*object, vol_idx, std::move(t.result), t.space,
                                                               m_solid_interfaces, m_keep_base_sparse_infill);
        created_all.insert(created_all.end(), created.begin(), created.end());
        if (std::find(touched_objects.begin(), touched_objects.end(), obj_idx) == touched_objects.end())
            touched_objects.push_back(obj_idx);
    }

    // One list rebuild per object, after every target has been applied: an earlier rebuild's wxDataViewItems
    // would be stale by the time the next target of the same object replaced its volumes.
    ObjectList         *obj_list = wxGetApp().obj_list();
    wxDataViewItemArray selection;
    for (size_t obj_idx : touched_objects) {
        wxDataViewItemArray items = obj_list->add_volumes_to_object_in_list(obj_idx, [&created_all](const ModelVolume *v) {
            return std::find(created_all.begin(), created_all.end(), v) != created_all.end();
        });
        for (const wxDataViewItem &item : items)
            selection.Add(item);
        // The object is no longer what its source file holds (ObjectList::split does the same).
        model.objects[obj_idx]->input_file.clear();
        obj_list->changed_object(int(obj_idx));
        obj_list->notify_instance_updated(int(obj_idx));
        obj_list->update_info_items(obj_idx);
    }
    if (! selection.IsEmpty())
        obj_list->select_items(selection);

    if (! warnings.empty())
        wxGetApp().notification_manager()->push_plater_warning_notification(join_lines(warnings));
    if (! errors.empty())
        show_error(m_plater, from_u8(join_lines(errors)));
}

}} // namespace Slic3r::GUI
