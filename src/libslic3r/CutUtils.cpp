
#include "CutUtils.hpp"
#include "Geometry.hpp"
#include "libslic3r.h"
#include "Model.hpp"
#include "FlexiJoint.hpp"
#include "CurvedCut.hpp"
#include "MeshBoolean.hpp"
#include "TriangleMeshSlicer.hpp"
#include "TriangleSelector.hpp"
#include "ObjectID.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {

using namespace Geometry;

static void apply_tolerance(ModelVolume* vol)
{
    ModelVolume::CutInfo& cut_info = vol->cut_info;

    assert(cut_info.is_connector);
    if (!cut_info.is_processed)
        return;

    Vec3d sf = vol->get_scaling_factor();

    // make a "hole" wider
    sf[X] += double(cut_info.radius_tolerance);
    sf[Y] += double(cut_info.radius_tolerance);

    // make a "hole" dipper
    sf[Z] += double(cut_info.height_tolerance);

    vol->set_scaling_factor(sf);

    // correct offset in respect to the new depth
    Vec3d rot_norm = rotation_transform(vol->get_rotation()) * Vec3d::UnitZ();
    if (rot_norm.norm() != 0.0)
        rot_norm.normalize();

    double z_offset = 0.5 * static_cast<double>(cut_info.height_tolerance);
    if (cut_info.connector_type == CutConnectorType::Plug || 
        cut_info.connector_type == CutConnectorType::Snap)
        z_offset -= 0.05; // add small Z offset to better preview

    vol->set_offset(vol->get_offset() + rot_norm * z_offset);
}

static void add_cut_volume(TriangleMesh& mesh, ModelObject* object, const ModelVolume* src_volume, const Transform3d& cut_matrix, const std::string& suffix = {}, ModelVolumeType type = ModelVolumeType::MODEL_PART)
{
    if (mesh.empty())
        return;

    mesh.transform(cut_matrix);
    ModelVolume* vol = object->add_volume(mesh);
    vol->set_type(type);

    vol->name = src_volume->name + suffix;
    // Don't copy the config's ID.
    vol->config.assign_config(src_volume->config);
    assert(vol->config.id().valid());
    assert(vol->config.id() != src_volume->config.id());
    vol->set_material(src_volume->material_id(), *src_volume->material());
    vol->cut_info = src_volume->cut_info;
}

// PHASE 3: `thickness` is the kerf. A flat cut with a kerf is TWO half-space
// slices at +/- t/2 instead of one at zero, each keeping only its own outer half
// - the band between them is what the saw took away. That reuses cut_mesh() as
// it stands (the flexi joint's gap already does exactly this at CutUtils.cpp's
// perform_with_flexi_joints), so no new geometry code and no boolean is needed.
//
// t == 0 takes the single-slice branch, which is the original call verbatim, so
// the no-kerf output is not "equivalent to" today's - it IS today's.
static void process_volume_cut( ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                                ModelObjectCutAttributes attributes, TriangleMesh& upper_mesh, TriangleMesh& lower_mesh,
                                double thickness = 0.0, CutThicknessOffset offset = CutThicknessOffset::Centred)
{
    const auto volume_matrix = volume->get_matrix();

    const Transformation cut_transformation = Transformation(cut_matrix);
    const Transform3d invert_cut_matrix = cut_transformation.get_rotation_matrix().inverse() * translation_transform(-1 * cut_transformation.get_offset());

    // Transform the mesh by the combined transformation matrix.
    // Flip the triangles in case the composite transformation is left handed.
    TriangleMesh mesh(volume->mesh());
    mesh.transform(invert_cut_matrix * instance_matrix * volume_matrix, true);

    indexed_triangle_set upper_its, lower_its;
    if (thickness > 0.0) {
        double face_lo = 0.0, face_hi = 0.0;
        curved_cut_thickness_faces(thickness, offset, face_lo, face_hi);
        cut_mesh(mesh.its, float(face_lo), nullptr, &lower_its);
        cut_mesh(mesh.its, float(face_hi), &upper_its, nullptr);
    }
    else
        cut_mesh(mesh.its, 0.0f, &upper_its, &lower_its);
    if (attributes.has(ModelObjectCutAttribute::KeepUpper))
        upper_mesh = TriangleMesh(upper_its);
    if (attributes.has(ModelObjectCutAttribute::KeepLower))
        lower_mesh = TriangleMesh(lower_its);
}

static void process_connector_cut(  ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                                    ModelObjectCutAttributes attributes, ModelObject* upper, ModelObject* lower,
                                    std::vector<ModelObject*>& dowels)
{
    assert(volume->cut_info.is_connector);
    volume->cut_info.set_processed();

    const auto volume_matrix = volume->get_matrix();

    // ! Don't apply instance transformation for the conntectors.
    // This transformation is already there
    if (volume->cut_info.connector_type != CutConnectorType::Dowel) {
        if (attributes.has(ModelObjectCutAttribute::KeepUpper)) {
            ModelVolume* vol = nullptr;
            if (volume->cut_info.connector_type == CutConnectorType::Snap) {
                TriangleMesh mesh = TriangleMesh(its_make_cylinder(1.0, 1.0, PI / 180.));

                vol = upper->add_volume(std::move(mesh));
                vol->set_transformation(volume->get_transformation());
                vol->set_type(ModelVolumeType::NEGATIVE_VOLUME);

                vol->cut_info = volume->cut_info;
                vol->name = volume->name;
            }
            else
                vol = upper->add_volume(*volume);

            vol->set_transformation(volume_matrix);
            apply_tolerance(vol);
        }
        if (attributes.has(ModelObjectCutAttribute::KeepLower)) {
            ModelVolume* vol = lower->add_volume(*volume);
            vol->set_transformation(volume_matrix);
            // for lower part change type of connector from NEGATIVE_VOLUME to MODEL_PART if this connector is a plug
            vol->set_type(ModelVolumeType::MODEL_PART);
        }
    }
    else {
        if (attributes.has(ModelObjectCutAttribute::CreateDowels)) {
            ModelObject* dowel{ nullptr };
            // Clone the object to duplicate instances, materials etc.
            volume->get_object()->clone_for_cut(&dowel);

            // add one more solid part same as connector if this connector is a dowel
            ModelVolume* vol = dowel->add_volume(*volume);
            vol->set_type(ModelVolumeType::MODEL_PART);

            // But discard rotation and Z-offset for this volume
            vol->set_rotation(Vec3d::Zero());
            vol->set_offset(Z, 0.0);

            dowels.push_back(dowel);
        }

        // Cut the dowel
        apply_tolerance(volume);

        // Perform cut
        TriangleMesh upper_mesh, lower_mesh;
        process_volume_cut(volume, Transform3d::Identity(), cut_matrix, attributes, upper_mesh, lower_mesh);

        // add small Z offset to better preview
        upper_mesh.translate((-0.05 * Vec3d::UnitZ()).cast<float>());
        lower_mesh.translate((0.05 * Vec3d::UnitZ()).cast<float>());

        // Add cut parts to the related objects
        add_cut_volume(upper_mesh, upper, volume, cut_matrix, "_A", volume->type());
        add_cut_volume(lower_mesh, lower, volume, cut_matrix, "_B", volume->type());
    }
}

static void process_modifier_cut(ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& inverse_cut_matrix,
                                 ModelObjectCutAttributes attributes, ModelObject* upper, ModelObject* lower)
{
    const auto volume_matrix = instance_matrix * volume->get_matrix();

    // Modifiers are not cut, but we still need to add the instance transformation
    // to the modifier volume transformation to preserve their shape properly.
    volume->set_transformation(Transformation(volume_matrix));

    if (attributes.has(ModelObjectCutAttribute::KeepAsParts)) {
        upper->add_volume(*volume);
        return;
    }

    // Some logic for the negative volumes/connectors. Add only needed modifiers
    auto bb = volume->mesh().transformed_bounding_box(inverse_cut_matrix * volume_matrix);
    bool is_crossed_by_cut = bb.min[Z] <= 0 && bb.max[Z] >= 0;
    if (attributes.has(ModelObjectCutAttribute::KeepUpper) && (bb.min[Z] >= 0 || is_crossed_by_cut))
        upper->add_volume(*volume);
    if (attributes.has(ModelObjectCutAttribute::KeepLower) && (bb.max[Z] <= 0 || is_crossed_by_cut))
        lower->add_volume(*volume);
}

static void process_solid_part_cut(ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                            ModelObjectCutAttributes attributes, ModelObject* upper, ModelObject* lower,
                            double thickness = 0.0, CutThicknessOffset offset = CutThicknessOffset::Centred)
{
    // Perform cut
    TriangleMesh upper_mesh, lower_mesh;
    process_volume_cut(volume, instance_matrix, cut_matrix, attributes, upper_mesh, lower_mesh, thickness, offset);

    // Add required cut parts to the objects

    if (attributes.has(ModelObjectCutAttribute::KeepAsParts)) {
        add_cut_volume(upper_mesh, upper, volume, cut_matrix, "_A");
        if (!lower_mesh.empty()) {
            add_cut_volume(lower_mesh, upper, volume, cut_matrix, "_B");
            upper->volumes.back()->cut_info.is_from_upper = false;
        }
        return;
    }

    if (attributes.has(ModelObjectCutAttribute::KeepUpper))
        add_cut_volume(upper_mesh, upper, volume, cut_matrix);

    if (attributes.has(ModelObjectCutAttribute::KeepLower) && !lower_mesh.empty())
        add_cut_volume(lower_mesh, lower, volume, cut_matrix);
}

static void reset_instance_transformation(ModelObject* object, size_t src_instance_idx, 
                                          const Transform3d& cut_matrix = Transform3d::Identity(),
                                          bool place_on_cut = false, bool flip = false)
{
    // Reset instance transformation except offset and Z-rotation

    for (size_t i = 0; i < object->instances.size(); ++i) {
        auto& obj_instance = object->instances[i];
        const double rot_z = obj_instance->get_rotation().z();
        
        Transformation inst_trafo = Transformation(obj_instance->get_transformation().get_matrix_no_scaling_factor());
        // add respect to mirroring
        if (obj_instance->is_left_handed())
            inst_trafo = inst_trafo * Transformation(scale_transform(Vec3d(-1, 1, 1)));

        obj_instance->set_transformation(inst_trafo);

        Vec3d rotation = Vec3d::Zero();
        if (!flip && !place_on_cut) {
            if ( i != src_instance_idx)
            rotation[Z] = rot_z;
        }
        else {
            Transform3d rotation_matrix = Transform3d::Identity();
            if (flip)
                rotation_matrix = rotation_transform(PI * Vec3d::UnitX());

            if (place_on_cut)
                rotation_matrix = rotation_matrix * Transformation(cut_matrix).get_rotation_matrix().inverse();

            if (i != src_instance_idx)
                rotation_matrix = rotation_transform(rot_z * Vec3d::UnitZ()) * rotation_matrix;

            rotation = Transformation(rotation_matrix).get_rotation();
        }

        obj_instance->set_rotation(rotation);
    }
}


Cut::Cut(const ModelObject* object, int instance, const Transform3d& cut_matrix,
         ModelObjectCutAttributes attributes/*= ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepAsParts*/)
    : m_instance(instance), m_cut_matrix(cut_matrix), m_attributes(attributes)
{
    m_model = Model();
    if (object)
        m_model.add_object(*object);
}

void Cut::post_process(ModelObject* object, ModelObjectPtrs& cut_object_ptrs, bool keep, bool place_on_cut, bool flip)
{
    if (!object) return;

    if (keep && !object->volumes.empty()) {
        reset_instance_transformation(object, m_instance, m_cut_matrix, place_on_cut, flip);
        cut_object_ptrs.push_back(object);
    }
    else
        m_model.objects.push_back(object); // will be deleted in m_model.clear_objects();
}

void Cut::post_process(ModelObject* upper, ModelObject* lower, ModelObjectPtrs& cut_object_ptrs)
{
    post_process(upper, cut_object_ptrs,
        m_attributes.has(ModelObjectCutAttribute::KeepUpper),
        m_attributes.has(ModelObjectCutAttribute::PlaceOnCutUpper),
        m_attributes.has(ModelObjectCutAttribute::FlipUpper));

    post_process(lower, cut_object_ptrs,
        m_attributes.has(ModelObjectCutAttribute::KeepLower),
        m_attributes.has(ModelObjectCutAttribute::PlaceOnCutLower),
        m_attributes.has(ModelObjectCutAttribute::PlaceOnCutLower) || m_attributes.has(ModelObjectCutAttribute::FlipLower));
}


void Cut::finalize(const ModelObjectPtrs& objects)
{
    //clear model from temporarry objects
    m_model.clear_objects();

    // add to model result objects
    m_model.objects = objects;
}


// ---------------------------------------------------------------------------- flexi joints

static bool is_unprocessed_flexi_volume(const ModelVolume* v)
{
    return !v->is_model_part() && v->cut_info.is_connector && !v->cut_info.is_processed &&
           is_flexi_connector_type(v->cut_info.connector_type);
}

bool has_flexi_joint(const ModelObject* mo)
{
    if (!mo)
        return false;
    for (const ModelVolume* v : mo->volumes)
        if (is_unprocessed_flexi_volume(v))
            return true;
    return false;
}

ModelVolume* add_flexi_joint_volume(ModelObject* mo, const CutConnector& connector, const std::string& name)
{
    using namespace Geometry;

    indexed_triangle_set body;
    for (const indexed_triangle_set& its : flexi_lower_bodies(connector.flexi))
        its_merge(body, its);
    // The chain link also puts a body on the upper segment; carry it too so the negative
    // volume the gizmo leaves behind is the whole joint, not half of it.
    for (const indexed_triangle_set& its : flexi_upper_bodies(connector.flexi))
        its_merge(body, its);

    // modify_to_center_geometry = false: the flexi bodies are already positioned relative to
    // the cut plane (they straddle z == 0), so re-centring them would move the joint.
    ModelVolume* vol = mo->add_volume(TriangleMesh(std::move(body)), ModelVolumeType::NEGATIVE_VOLUME, false);
    // The bodies are generated in millimetres already, so no scaling here: the transform only
    // moves the joint onto the cut plane and spins it about the plane normal.
    vol->set_transformation(translation_transform(connector.pos) * connector.rotation_m *
                            rotation_transform(-connector.z_angle * Vec3d::UnitZ()));
    vol->cut_info = ModelVolume::CutInfo(connector.attribs.type, connector.radius_tolerance, connector.height_tolerance);
    vol->cut_info.flexi = connector.flexi;
    vol->name = name;
    return vol;
}

// Manifold first, mcut as the fallback - the same chain the Mesh Boolean gizmo uses.
static bool flexi_boolean(TriangleMesh& a, const TriangleMesh& b, const std::string& op)
{
    std::vector<TriangleMesh> dst;
    bool ok = MeshBoolean::mfd::make_boolean(a, b, dst, op);
    if (!ok) {
        BOOST_LOG_TRIVIAL(warning) << "Flexi joint: Manifold boolean " << op << " failed, falling back to mcut";
        dst.clear();
        try {
            MeshBoolean::mcut::make_boolean(a, b, dst, op);
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "Flexi joint: mcut boolean " << op << " failed: " << ex.what();
            return false;
        }
    }
    if (dst.empty())
        return false;
    TriangleMesh out = dst.front();
    for (size_t i = 1; i < dst.size(); ++ i)
        out.merge(dst[i]);
    if (out.empty())
        return false;
    a = std::move(out);
    return true;
}

const ModelObjectPtrs& Cut::perform_with_flexi_joints()
{
    using namespace Geometry;

    ModelObject* mo = m_model.objects.front();

    BOOST_LOG_TRIVIAL(trace) << "Cut::perform_with_flexi_joints - start";

    // The two halves must stay parts of ONE object or the joint is not print-in-place:
    // this cut type forces keep-as-parts, keep-upper and keep-lower on regardless of the
    // gizmo's checkboxes (they are disabled in the UI while a flexi joint is placed).
    ModelObject* out{ nullptr };
    mo->clone_for_cut(&out);

    const auto           instance_matrix    = mo->instances[m_instance]->get_transformation().get_matrix_no_offset();
    const Transformation cut_transformation = Transformation(m_cut_matrix);
    const Transform3d    inverse_cut_matrix = cut_transformation.get_rotation_matrix().inverse() * translation_transform(-1. * cut_transformation.get_offset());

    // Phase 1: exactly one joint per cut. Extra joints are ignored with a warning.
    FlexiJointParams params;
    Transform3d      joint_matrix = Transform3d::Identity();
    ModelVolume*     first_solid  = nullptr;
    int              joints       = 0;
    for (ModelVolume* v : mo->volumes) {
        if (is_unprocessed_flexi_volume(v)) {
            if (++ joints == 1) {
                params       = v->cut_info.flexi;
                joint_matrix = inverse_cut_matrix * v->get_matrix();
            }
            v->cut_info.set_processed();
        } else if (v->is_model_part() && !v->mesh().empty() && first_solid == nullptr)
            first_solid = v;
    }
    if (joints > 1)
        BOOST_LOG_TRIVIAL(warning) << "Flexi joint: " << joints << " joints placed, only the first is applied (phase 1)";

    // PHASE 3: the kerf ADDS to the joint's own gap, in the params, BEFORE anything
    // is generated. flexi_effective_gap() sets where the two flat faces go, and
    // every flexi body (ring, ball, thread, chain link) is laid out relative to
    // those faces through flexi_frame() - so widening the gap in the params moves
    // the faces and the bodies together and the joint stays assembled, which is
    // what "the ring bodies bridge the gap" has to mean. Applying the kerf on top
    // of the gap instead would move the faces out from under the bodies.
    //
    // NOT supported, and why: a Thread's pitch is an absolute length that does not
    // scale with the gap, so a kerf at or above one pitch eats a whole turn and
    // the two halves no longer screw together. The gizmo warns; nothing here
    // refuses it, because a thread with a deliberately loose fit is a legitimate
    // thing to ask for.
    if (m_kerf > 0.0)
        params.gap = float(double(flexi_effective_gap(params)) + m_kerf);
    if (first_solid == nullptr) {
        m_model = Model();
        m_model.objects.push_back(out);
        return m_model.objects;
    }

    const double clearance = double(params.clearance);
    // THE GAP: the two segments' flat cut faces are set back from the cut plane by gap/2
    // each, so they end up `gap` apart and the joint bodies bridge that distance. The
    // revolved kinds additionally need the female face to clear the male body by the
    // clearance, so their upper face goes to gap/2 + clearance... no: the cavity already
    // carries the clearance, and the male body itself is generated relative to the FACES,
    // so gap/2 on each side is the whole story. See FlexiJoint.hpp.
    const double gap     = double(flexi_effective_gap(params));
    const double face_lo = -0.5 * gap;
    const double face_hi = +0.5 * gap;

    // Merge every solid part of the object into one mesh in the cut frame. Phase 1 limitation:
    // an object made of several model parts comes out of a flexi cut as two parts, not as
    // (parts x 2); per-volume config follows the first solid volume.
    TriangleMesh solid;
    for (ModelVolume* v : mo->volumes) {
        if (!v->is_model_part() || v->mesh().empty())
            continue;
        TriangleMesh m(v->mesh());
        m.transform(inverse_cut_matrix * instance_matrix * v->get_matrix(), true);
        solid.merge(m);
    }

    // The lower segment ends at -gap/2, the upper one starts at +gap/2: that is what opens
    // the gap between the two flat mating faces.
    indexed_triangle_set upper_its, lower_its;
    cut_mesh(solid.its, float(face_lo), nullptr, &lower_its);
    cut_mesh(solid.its, float(face_hi), &upper_its, nullptr);

    TriangleMesh lower_mesh(std::move(lower_its));
    TriangleMesh upper_mesh(std::move(upper_its));

    bool boolean_ok = !lower_mesh.empty() && !upper_mesh.empty();

    // Union each segment's own joint body into it, then subtract the OTHER side's body
    // inflated by the clearance. For the revolved kinds the lower relief list is empty and
    // the upper relief is the dilated male body, so this reduces to the phase 1 behaviour;
    // for the chain link both segments get a body and both get a relief.
    if (boolean_ok)
        for (const indexed_triangle_set& its : flexi_lower_bodies(params)) {
            TriangleMesh body(its);
            body.transform(joint_matrix);
            if (!flexi_boolean(lower_mesh, body, "UNION")) { boolean_ok = false; break; }
        }
    if (boolean_ok)
        for (const indexed_triangle_set& its : flexi_upper_bodies(params)) {
            TriangleMesh body(its);
            body.transform(joint_matrix);
            if (!flexi_boolean(upper_mesh, body, "UNION")) { boolean_ok = false; break; }
        }
    if (boolean_ok)
        for (const indexed_triangle_set& its : flexi_lower_reliefs(params)) {
            TriangleMesh relief(its);
            relief.transform(joint_matrix);
            if (!flexi_boolean(lower_mesh, relief, "A_NOT_B")) { boolean_ok = false; break; }
        }
    if (boolean_ok)
        for (const indexed_triangle_set& its : flexi_upper_reliefs(params)) {
            TriangleMesh relief(its);
            relief.transform(joint_matrix);
            if (!flexi_boolean(upper_mesh, relief, "A_NOT_B")) { boolean_ok = false; break; }
        }

    if (!boolean_ok)
        BOOST_LOG_TRIVIAL(error) << "Flexi joint: boolean failed, the cut falls back to two plain halves";

    add_cut_volume(upper_mesh, out, first_solid, m_cut_matrix, "_A");
    if (!lower_mesh.empty()) {
        add_cut_volume(lower_mesh, out, first_solid, m_cut_matrix, "_B");
        out->volumes.back()->cut_info.is_from_upper = false;
    }
    // Keep the modifiers, drop the (now consumed) connector volumes.
    for (ModelVolume* v : mo->volumes)
        if (!v->is_model_part() && !v->cut_info.is_connector) {
            ModelVolume* copy = out->add_volume(*v);
            copy->set_transformation(Transformation(instance_matrix * v->get_matrix()));
        }

    ModelObjectPtrs cut_object_ptrs;
    if (!out->volumes.empty()) {
        reset_instance_transformation(out, m_instance, m_cut_matrix);
        cut_object_ptrs.push_back(out);
    } else
        m_model.objects.push_back(out);

    BOOST_LOG_TRIVIAL(trace) << "Cut::perform_with_flexi_joints - end";

    finalize(cut_object_ptrs);
    return m_model.objects;
}

// ---------------------------------------------------------------------------- curved cut

// The curved counterpart of process_volume_cut(): same frame, same output shape,
// but the flat cut_mesh() half-space slice is replaced with two booleans against
// the sheet's slab (Manifold, mcut fallback). See CurvedCut.hpp.
static void process_volume_curved_cut(ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                                      ModelObjectCutAttributes attributes, const CurvedCutSheet& sheet,
                                      TriangleMesh& upper_mesh, TriangleMesh& lower_mesh, double thickness = 0.0,
                                      CutThicknessOffset offset = CutThicknessOffset::Centred)
{
    const auto volume_matrix = volume->get_matrix();

    const Transformation cut_transformation = Transformation(cut_matrix);
    const Transform3d invert_cut_matrix = cut_transformation.get_rotation_matrix().inverse() * translation_transform(-1 * cut_transformation.get_offset());

    TriangleMesh mesh(volume->mesh());
    mesh.transform(invert_cut_matrix * instance_matrix * volume_matrix, true);

    indexed_triangle_set upper_its, lower_its;
    const bool want_upper = attributes.has(ModelObjectCutAttribute::KeepUpper);
    const bool want_lower = attributes.has(ModelObjectCutAttribute::KeepLower);
    if (!curved_cut_split(mesh.its, sheet, want_upper ? &upper_its : nullptr, want_lower ? &lower_its : nullptr,
                          CurvedCutSheet::CutSamples, thickness, offset))
        BOOST_LOG_TRIVIAL(error) << "Curved cut: boolean failed for volume " << volume->name;

    if (want_upper)
        upper_mesh = TriangleMesh(upper_its);
    if (want_lower)
        lower_mesh = TriangleMesh(lower_its);
}

static void process_solid_part_curved_cut(ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                                          ModelObjectCutAttributes attributes, const CurvedCutSheet& sheet,
                                          ModelObject* upper, ModelObject* lower, double thickness = 0.0,
                                          CutThicknessOffset offset = CutThicknessOffset::Centred)
{
    TriangleMesh upper_mesh, lower_mesh;
    process_volume_curved_cut(volume, instance_matrix, cut_matrix, attributes, sheet, upper_mesh, lower_mesh, thickness, offset);

    if (attributes.has(ModelObjectCutAttribute::KeepAsParts)) {
        add_cut_volume(upper_mesh, upper, volume, cut_matrix, "_A");
        if (!lower_mesh.empty()) {
            add_cut_volume(lower_mesh, upper, volume, cut_matrix, "_B");
            upper->volumes.back()->cut_info.is_from_upper = false;
        }
        return;
    }

    if (attributes.has(ModelObjectCutAttribute::KeepUpper))
        add_cut_volume(upper_mesh, upper, volume, cut_matrix);

    if (attributes.has(ModelObjectCutAttribute::KeepLower) && !lower_mesh.empty())
        add_cut_volume(lower_mesh, lower, volume, cut_matrix);
}

const ModelObjectPtrs& Cut::perform_with_curved_sheet(const CurvedCutSheet& sheet, double thickness, CutThicknessOffset offset)
{
    // THE PHASE 1 INVARIANT: no displacement means no curve, and no curve means
    // the plain plane cut - the same function, not a boolean that happens to
    // give the same answer. Output is therefore bit-identical to a flat cut.
    // The kerf rides along: a flat sheet with a thickness is a flat cut with the
    // same thickness, which is two half-space slices rather than two booleans.
    if (sheet.is_flat())
        return perform_with_plane(thickness, offset);

    if (!m_attributes.has(ModelObjectCutAttribute::KeepUpper) && !m_attributes.has(ModelObjectCutAttribute::KeepLower)) {
        m_model.clear_objects();
        return m_model.objects;
    }

    // PHASE 4: a flexi joint on a curved cut. The joint's two segments are
    // separated by its OWN gap - two flat faces `gap` apart - and the bodies are
    // generated relative to those faces, so a flexi cut cannot also be a curved
    // one: the surface between the segments IS the joint's face pair. A flexi
    // joint therefore takes the flexi path (with the joint standing on the
    // sheet's frame, which the gizmo has already baked into the connector's
    // rotation_m), the same way a flat cut with a flexi joint does.
    m_kerf = std::max(0.0, thickness);
    if (!m_model.objects.empty() && has_flexi_joint(m_model.objects.front()))
        return perform_with_flexi_joints();

    ModelObject* mo = m_model.objects.front();

    BOOST_LOG_TRIVIAL(trace) << "Cut::perform_with_curved_sheet - start";

    ModelObject* upper{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepUpper))
        mo->clone_for_cut(&upper);

    ModelObject* lower{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepLower) && !m_attributes.has(ModelObjectCutAttribute::KeepAsParts))
        mo->clone_for_cut(&lower);

    std::vector<ModelObject*> dowels;

    const auto           instance_matrix    = mo->instances[m_instance]->get_transformation().get_matrix_no_offset();
    const Transformation cut_transformation = Transformation(m_cut_matrix);
    const Transform3d    inverse_cut_matrix = cut_transformation.get_rotation_matrix().inverse() * translation_transform(-1. * cut_transformation.get_offset());

    // PHASE 4: connectors. A connector volume reaches here already carrying its
    // own frame - translation_transform(pos) * rotation_m * ... - because the
    // gizmo builds it that way, and on a curved cut rotation_m is the SHEET's
    // frame at the connector's (u,v) rather than the plane's. So nothing in the
    // connector path itself has to know about the sheet: the plug/dowel/snap
    // solids and their pockets are built by exactly the same helper the flat cut
    // uses, and the tilt rides in through the volume's transform.
    //
    // The one place the sheet does matter is the DOWEL, which is cut in half so
    // that each half goes to its own object. process_connector_cut() slices it
    // with the flat cut_mesh(), which is the RIGHT thing here: the dowel's own
    // frame is already the sheet's frame, so "flat, in the dowel's frame" IS
    // "tangent to the sheet at the dowel". See process_connector_curved_cut().
    for (ModelVolume* volume : mo->volumes) {
        volume->reset_extra_facets();

        if (!volume->is_model_part()) {
            if (volume->cut_info.is_processed)
                process_modifier_cut(volume, instance_matrix, inverse_cut_matrix, m_attributes, upper, lower);
            else
                process_connector_cut(volume, instance_matrix, m_cut_matrix, m_attributes, upper, lower, dowels);
        }
        else if (!volume->mesh().empty())
            process_solid_part_curved_cut(volume, instance_matrix, m_cut_matrix, m_attributes, sheet, upper, lower, thickness, offset);
    }

    if (m_attributes.has(ModelObjectCutAttribute::KeepAsParts) && upper->volumes.empty()) {
        m_model = Model();
        m_model.objects.push_back(upper);
        return m_model.objects;
    }

    ModelObjectPtrs cut_object_ptrs;

    if (m_attributes.has(ModelObjectCutAttribute::KeepAsParts) && !upper->volumes.empty()) {
        reset_instance_transformation(upper, m_instance, m_cut_matrix);
        cut_object_ptrs.push_back(upper);
    }
    else {
        auto delete_extra_modifiers = [this](ModelObject* mo) {
            if (!mo) return;
            const BoundingBoxf3 obj_bb = mo->instance_bounding_box(m_instance);
            const Transform3d inst_matrix = mo->instances[m_instance]->get_transformation().get_matrix();

            for (int i = int(mo->volumes.size()) - 1; i >= 0; --i)
                if (const ModelVolume* vol = mo->volumes[i];
                    !vol->is_model_part() && !vol->is_cut_connector()) {
                    auto bb = vol->mesh().transformed_bounding_box(inst_matrix * vol->get_matrix());
                    if (!obj_bb.intersects(bb))
                        mo->delete_volume(i);
                }
        };

        post_process(upper, lower, cut_object_ptrs);
        delete_extra_modifiers(upper);
        delete_extra_modifiers(lower);

        // PHASE 4: a Dowel asks for a third object carrying the pin itself, the
        // same as on a flat cut. The pin is already generated in the sheet's own
        // frame, so it comes out standing along the surface normal at its (u,v),
        // and reset_instance_transformation() puts it flat on the bed for
        // printing exactly the way the flat path does.
        if (m_attributes.has(ModelObjectCutAttribute::CreateDowels) && !dowels.empty()) {
            for (auto dowel : dowels) {
                reset_instance_transformation(dowel, m_instance);
                dowel->name += "-Dowel-" + dowel->volumes[0]->name;
                cut_object_ptrs.push_back(dowel);
            }
        }
    }

    BOOST_LOG_TRIVIAL(trace) << "Cut::perform_with_curved_sheet - end";

    finalize(cut_object_ptrs);

    return m_model.objects;
}

// ---------------------------------------------------------------------------- draw cut

// The drawn counterpart of process_volume_curved_cut(): identical in shape, and
// the only line that differs is the split call. The frame maths, the want_upper /
// want_lower gating and the output shape are the curved path's, unchanged.
static void process_volume_draw_cut(ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                                    ModelObjectCutAttributes attributes, const DrawCutStroke& stroke,
                                    const DrawCutParams& params, TriangleMesh& upper_mesh, TriangleMesh& lower_mesh)
{
    const auto volume_matrix = volume->get_matrix();

    const Transformation cut_transformation = Transformation(cut_matrix);
    const Transform3d invert_cut_matrix = cut_transformation.get_rotation_matrix().inverse() * translation_transform(-1 * cut_transformation.get_offset());

    TriangleMesh mesh(volume->mesh());
    mesh.transform(invert_cut_matrix * instance_matrix * volume_matrix, true);

    indexed_triangle_set upper_its, lower_its;
    const bool want_upper = attributes.has(ModelObjectCutAttribute::KeepUpper);
    const bool want_lower = attributes.has(ModelObjectCutAttribute::KeepLower);
    DrawCutError err = DrawCutError::None;
    if (!draw_cut_split(mesh.its, stroke, params, want_upper ? &upper_its : nullptr, want_lower ? &lower_its : nullptr, &err))
        BOOST_LOG_TRIVIAL(error) << "Draw cut: boolean failed for volume " << volume->name
                                 << " (" << draw_cut_error_message(err) << ")";

    if (want_upper)
        upper_mesh = TriangleMesh(upper_its);
    if (want_lower)
        lower_mesh = TriangleMesh(lower_its);
}

static void process_solid_part_draw_cut(ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                                        ModelObjectCutAttributes attributes, const DrawCutStroke& stroke,
                                        const DrawCutParams& params, ModelObject* upper, ModelObject* lower)
{
    TriangleMesh upper_mesh, lower_mesh;
    process_volume_draw_cut(volume, instance_matrix, cut_matrix, attributes, stroke, params, upper_mesh, lower_mesh);

    if (attributes.has(ModelObjectCutAttribute::KeepAsParts)) {
        add_cut_volume(upper_mesh, upper, volume, cut_matrix, "_A");
        if (!lower_mesh.empty()) {
            add_cut_volume(lower_mesh, upper, volume, cut_matrix, "_B");
            upper->volumes.back()->cut_info.is_from_upper = false;
        }
        return;
    }

    if (attributes.has(ModelObjectCutAttribute::KeepUpper))
        add_cut_volume(upper_mesh, upper, volume, cut_matrix);

    if (attributes.has(ModelObjectCutAttribute::KeepLower) && !lower_mesh.empty())
        add_cut_volume(lower_mesh, lower, volume, cut_matrix);
}

const ModelObjectPtrs& Cut::perform_with_draw_stroke(const DrawCutStroke& stroke, const DrawCutParams& params)
{
    // There is no "zero stroke" that degenerates to the flat cut the way a zero
    // sheet does - an empty stroke is simply no cut - so unlike
    // perform_with_curved_sheet() there is nothing to route back into
    // perform_with_plane(). An unusable stroke is a refusal.
    if (!stroke.valid()) {
        BOOST_LOG_TRIVIAL(error) << "Cut::perform_with_draw_stroke - the stroke is not usable: "
                                 << draw_cut_error_message(stroke.error());
        m_model.clear_objects();
        return m_model.objects;
    }

    if (!m_attributes.has(ModelObjectCutAttribute::KeepUpper) && !m_attributes.has(ModelObjectCutAttribute::KeepLower)) {
        m_model.clear_objects();
        return m_model.objects;
    }

    // A flexi joint on a drawn cut, exactly as on a curved one: the joint's two
    // segments are separated by its OWN gap - two flat faces `gap` apart - and the
    // bodies are generated relative to those faces, so the surface between the
    // segments IS the joint's face pair and cannot also be the drawn strip. It
    // takes the flexi path, with the joint standing on whatever frame the gizmo
    // baked into the connector's rotation_m.
    m_kerf = std::max(0.0, params.thickness);
    if (!m_model.objects.empty() && has_flexi_joint(m_model.objects.front()))
        return perform_with_flexi_joints();

    ModelObject* mo = m_model.objects.front();

    BOOST_LOG_TRIVIAL(trace) << "Cut::perform_with_draw_stroke - start";

    ModelObject* upper{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepUpper))
        mo->clone_for_cut(&upper);

    ModelObject* lower{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepLower) && !m_attributes.has(ModelObjectCutAttribute::KeepAsParts))
        mo->clone_for_cut(&lower);

    std::vector<ModelObject*> dowels;

    const auto           instance_matrix    = mo->instances[m_instance]->get_transformation().get_matrix_no_offset();
    const Transformation cut_transformation = Transformation(m_cut_matrix);
    const Transform3d    inverse_cut_matrix = cut_transformation.get_rotation_matrix().inverse() * translation_transform(-1. * cut_transformation.get_offset());

    // PHASE 1 SHIPS CONNECTOR-FREE on the drawn surface (the gizmo's panel does not
    // offer them), but the loop keeps the curved path's shape so a connector volume
    // that reached here some other way is still processed rather than dropped -
    // which is also what phase 2 will need once the drawn surface grows a frame.
    for (ModelVolume* volume : mo->volumes) {
        volume->reset_extra_facets();

        if (!volume->is_model_part()) {
            if (volume->cut_info.is_processed)
                process_modifier_cut(volume, instance_matrix, inverse_cut_matrix, m_attributes, upper, lower);
            else
                process_connector_cut(volume, instance_matrix, m_cut_matrix, m_attributes, upper, lower, dowels);
        }
        else if (!volume->mesh().empty())
            process_solid_part_draw_cut(volume, instance_matrix, m_cut_matrix, m_attributes, stroke, params, upper, lower);
    }

    if (m_attributes.has(ModelObjectCutAttribute::KeepAsParts) && upper->volumes.empty()) {
        m_model = Model();
        m_model.objects.push_back(upper);
        return m_model.objects;
    }

    ModelObjectPtrs cut_object_ptrs;

    if (m_attributes.has(ModelObjectCutAttribute::KeepAsParts) && !upper->volumes.empty()) {
        reset_instance_transformation(upper, m_instance, m_cut_matrix);
        cut_object_ptrs.push_back(upper);
    }
    else {
        auto delete_extra_modifiers = [this](ModelObject* mo) {
            if (!mo) return;
            const BoundingBoxf3 obj_bb = mo->instance_bounding_box(m_instance);
            const Transform3d inst_matrix = mo->instances[m_instance]->get_transformation().get_matrix();

            for (int i = int(mo->volumes.size()) - 1; i >= 0; --i)
                if (const ModelVolume* vol = mo->volumes[i];
                    !vol->is_model_part() && !vol->is_cut_connector()) {
                    auto bb = vol->mesh().transformed_bounding_box(inst_matrix * vol->get_matrix());
                    if (!obj_bb.intersects(bb))
                        mo->delete_volume(i);
                }
        };

        post_process(upper, lower, cut_object_ptrs);
        delete_extra_modifiers(upper);
        delete_extra_modifiers(lower);

        if (m_attributes.has(ModelObjectCutAttribute::CreateDowels) && !dowels.empty()) {
            for (auto dowel : dowels) {
                reset_instance_transformation(dowel, m_instance);
                dowel->name += "-Dowel-" + dowel->volumes[0]->name;
                cut_object_ptrs.push_back(dowel);
            }
        }
    }

    BOOST_LOG_TRIVIAL(trace) << "Cut::perform_with_draw_stroke - end";

    finalize(cut_object_ptrs);

    return m_model.objects;
}

const ModelObjectPtrs& Cut::perform_with_plane(double thickness, CutThicknessOffset offset)
{
    if (!m_attributes.has(ModelObjectCutAttribute::KeepUpper) && !m_attributes.has(ModelObjectCutAttribute::KeepLower)) {
        m_model.clear_objects();
        return m_model.objects;
    }

    // A flexi joint opens its OWN gap between the two faces (flexi_effective_gap),
    // and the joint bodies are generated relative to those faces, so a kerf on top
    // of it would move the faces out from under the bodies. The kerf is therefore
    // added to the joint's gap rather than applied separately - see
    // perform_with_flexi_joints(m_kerf).
    m_kerf = std::max(0.0, thickness);
    if (!m_model.objects.empty() && has_flexi_joint(m_model.objects.front()))
        return perform_with_flexi_joints();

    ModelObject* mo = m_model.objects.front();

    BOOST_LOG_TRIVIAL(trace) << "ModelObject::cut - start";

    // Clone the object to duplicate instances, materials etc.
    ModelObject* upper{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepUpper))
        mo->clone_for_cut(&upper);

    ModelObject* lower{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepLower) && !m_attributes.has(ModelObjectCutAttribute::KeepAsParts))
        mo->clone_for_cut(&lower);

    std::vector<ModelObject*> dowels;

    // Because transformations are going to be applied to meshes directly,
    // we reset transformation of all instances and volumes,
    // except for translation and Z-rotation on instances, which are preserved
    // in the transformation matrix and not applied to the mesh transform.

    const auto              instance_matrix = mo->instances[m_instance]->get_transformation().get_matrix_no_offset();
    const Transformation    cut_transformation = Transformation(m_cut_matrix);
    const Transform3d       inverse_cut_matrix = cut_transformation.get_rotation_matrix().inverse() * translation_transform(-1. * cut_transformation.get_offset());

    for (ModelVolume* volume : mo->volumes) {
        volume->reset_extra_facets();

        if (!volume->is_model_part()) {
            if (volume->cut_info.is_processed)
                process_modifier_cut(volume, instance_matrix, inverse_cut_matrix, m_attributes, upper, lower);
            else
                process_connector_cut(volume, instance_matrix, m_cut_matrix, m_attributes, upper, lower, dowels);
        }
        else if (!volume->mesh().empty())
            process_solid_part_cut(volume, instance_matrix, m_cut_matrix, m_attributes, upper, lower, std::max(0.0, thickness), offset);
    }

    // Post-process cut parts

    if (m_attributes.has(ModelObjectCutAttribute::KeepAsParts) && upper->volumes.empty()) {
        m_model = Model();
        m_model.objects.push_back(upper);
        return m_model.objects;
    }

    ModelObjectPtrs cut_object_ptrs;

    if (m_attributes.has(ModelObjectCutAttribute::KeepAsParts) && !upper->volumes.empty()) {
        reset_instance_transformation(upper, m_instance, m_cut_matrix);
        cut_object_ptrs.push_back(upper);
    }
    else {
        // Delete all modifiers which are not intersecting with solid parts bounding box
        auto delete_extra_modifiers = [this](ModelObject* mo) {
            if (!mo) return;
            const BoundingBoxf3 obj_bb = mo->instance_bounding_box(m_instance);
            const Transform3d inst_matrix = mo->instances[m_instance]->get_transformation().get_matrix();

            for (int i = int(mo->volumes.size()) - 1; i >= 0; --i)
                if (const ModelVolume* vol = mo->volumes[i];
                    !vol->is_model_part() && !vol->is_cut_connector()) {
                    auto bb = vol->mesh().transformed_bounding_box(inst_matrix * vol->get_matrix());
                    if (!obj_bb.intersects(bb))
                        mo->delete_volume(i);
                }
        };

        post_process(upper, lower, cut_object_ptrs);
        delete_extra_modifiers(upper);
        delete_extra_modifiers(lower);

        if (m_attributes.has(ModelObjectCutAttribute::CreateDowels) && !dowels.empty()) {
            for (auto dowel : dowels) {
                reset_instance_transformation(dowel, m_instance);
                dowel->name += "-Dowel-" + dowel->volumes[0]->name;
                cut_object_ptrs.push_back(dowel);
            }
        }
    }

    BOOST_LOG_TRIVIAL(trace) << "ModelObject::cut - end";

    finalize(cut_object_ptrs);

    return m_model.objects;
}

static void distribute_modifiers_from_object(ModelObject* from_obj, const int instance_idx, ModelObject* to_obj1, ModelObject* to_obj2)
{
    auto              obj1_bb = to_obj1 ? to_obj1->instance_bounding_box(instance_idx) : BoundingBoxf3();
    auto              obj2_bb = to_obj2 ? to_obj2->instance_bounding_box(instance_idx) : BoundingBoxf3();
    const Transform3d inst_matrix = from_obj->instances[instance_idx]->get_transformation().get_matrix();

    for (ModelVolume* vol : from_obj->volumes)
        if (!vol->is_model_part()) {
            // Don't add modifiers which are processed connectors
            if (vol->cut_info.is_connector && !vol->cut_info.is_processed)
                continue;
            auto bb = vol->mesh().transformed_bounding_box(inst_matrix * vol->get_matrix());
            // Don't add modifiers which are not intersecting with solid parts
            if (obj1_bb.intersects(bb))
                to_obj1->add_volume(*vol);
            if (obj2_bb.intersects(bb))
                to_obj2->add_volume(*vol);
        }
}

static void merge_solid_parts_inside_object(ModelObjectPtrs& objects)
{
    for (ModelObject* mo : objects) {
        TriangleMesh mesh;
        // Merge all SolidPart but not Connectors
        for (const ModelVolume* mv : mo->volumes) {
            if (mv->is_model_part() && !mv->is_cut_connector()) {
                TriangleMesh m = mv->mesh();
                m.transform(mv->get_matrix());
                mesh.merge(m);
            }
        }
        if (!mesh.empty()) {
            ModelVolume* new_volume = mo->add_volume(mesh);
            new_volume->name = mo->name;
            // Delete all merged SolidPart but not Connectors
            for (int i = int(mo->volumes.size()) - 2; i >= 0; --i) {
                const ModelVolume* mv = mo->volumes[i];
                if (mv->is_model_part() && !mv->is_cut_connector())
                    mo->delete_volume(i);
            }
            // Ensuring that volumes start with solid parts for proper slicing
            mo->sort_volumes(true);
        }
    }
}


const ModelObjectPtrs& Cut::perform_by_contour(std::vector<Part> parts, int dowels_count)
{
    ModelObject* cut_mo = m_model.objects.front();

    // Clone the object to duplicate instances, materials etc.
    ModelObject* upper{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepUpper)) cut_mo->clone_for_cut(&upper);
    ModelObject* lower{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepLower)) cut_mo->clone_for_cut(&lower);

    if (upper && lower) {
        upper->name = upper->name + "_A";
        lower->name = lower->name + "_B";
    }

    const size_t cut_parts_cnt = parts.size();
    bool has_modifiers = false;

    // Distribute SolidParts to the Upper/Lower object
    for (size_t id = 0; id < cut_parts_cnt; ++id) {
        if (parts[id].is_modifier)
            has_modifiers = true; // modifiers will be added later to the related parts
        else if (ModelObject* obj = (parts[id].selected ? upper : lower))
            obj->add_volume(*(cut_mo->volumes[id]));
    }

    if (has_modifiers) {
        // Distribute Modifiers to the Upper/Lower object
        distribute_modifiers_from_object(cut_mo, m_instance, upper, lower);
    }

    ModelObjectPtrs cut_object_ptrs;

    ModelVolumePtrs& volumes = cut_mo->volumes;
    if (volumes.size() == cut_parts_cnt) {
        // Means that object is cut without connectors

        // Just add Upper and Lower objects to cut_object_ptrs
        post_process(upper, lower, cut_object_ptrs);

        // Now merge all model parts together:
        merge_solid_parts_inside_object(cut_object_ptrs);

        // replace initial objects in model with cut object 
        finalize(cut_object_ptrs);
    }
    else if (volumes.size() > cut_parts_cnt) {
        // Means that object is cut with connectors

        // All volumes are distributed to Upper / Lower object,
        // So we don’t need them anymore
        for (size_t id = 0; id < cut_parts_cnt; id++)
            delete* (volumes.begin() + id);
        volumes.erase(volumes.begin(), volumes.begin() + cut_parts_cnt);

        // Perform cut just to get connectors
        Cut cut(cut_mo, m_instance, m_cut_matrix, m_attributes);
        const ModelObjectPtrs& cut_connectors_obj = cut.perform_with_plane();
        assert(dowels_count > 0 ? cut_connectors_obj.size() >= 3 : cut_connectors_obj.size() == 2);

        // Connectors from upper object
        for (const ModelVolume* volume : cut_connectors_obj[0]->volumes)
            upper->add_volume(*volume, volume->type());

        // Connectors from lower object
        for (const ModelVolume* volume : cut_connectors_obj[1]->volumes)
            lower->add_volume(*volume, volume->type());

        // Add Upper and Lower objects to cut_object_ptrs
        post_process(upper, lower, cut_object_ptrs);

        // Now merge all model parts together:
        merge_solid_parts_inside_object(cut_object_ptrs);

        // replace initial objects in model with cut object
        finalize(cut_object_ptrs);

        // Add Dowel-connectors as separate objects to model
        if (cut_connectors_obj.size() >= 3)
            for (size_t id = 2; id < cut_connectors_obj.size(); id++)
                m_model.add_object(*cut_connectors_obj[id]);
    }

    return m_model.objects;
}


const ModelObjectPtrs& Cut::perform_with_groove(const Groove& groove, const Transform3d& rotation_m, bool keep_as_parts/* = false*/)
{
    ModelObject* cut_mo = m_model.objects.front();

    // Clone the object to duplicate instances, materials etc.
    ModelObject* upper{ nullptr };
    cut_mo->clone_for_cut(&upper);
    ModelObject* lower{ nullptr };
    cut_mo->clone_for_cut(&lower);

    if (upper && lower) {
        upper->name = upper->name + "_A";
        lower->name = lower->name + "_B";
    }
    const double groove_half_depth = 0.5 * double(groove.depth);

    Model tmp_model_for_cut = Model();

    Model tmp_model = Model();
    tmp_model.add_object(*cut_mo);
    ModelObject* tmp_object = tmp_model.objects.front();

    auto add_volumes_from_cut = [](ModelObject* object, const ModelObjectCutAttribute attribute, const Model& tmp_model_for_cut) {
        const auto& volumes = tmp_model_for_cut.objects.front()->volumes;
        for (const ModelVolume* volume : volumes)
            if (volume->is_model_part()) {
                if ((attribute == ModelObjectCutAttribute::KeepUpper && volume->is_from_upper()) ||
                    (attribute != ModelObjectCutAttribute::KeepUpper && !volume->is_from_upper())) {
                    ModelVolume* new_vol = object->add_volume(*volume);
                    new_vol->reset_from_upper();
                }
            }
    };

    auto cut = [this, add_volumes_from_cut]
                (ModelObject* object, const Transform3d& cut_matrix, const ModelObjectCutAttribute add_volumes_attribute, Model& tmp_model_for_cut) {
        Cut cut(object, m_instance, cut_matrix);

        tmp_model_for_cut = Model();
        tmp_model_for_cut.add_object(*cut.perform_with_plane().front());
        assert(!tmp_model_for_cut.objects.empty());

        object->clear_volumes();
        add_volumes_from_cut(object, add_volumes_attribute, tmp_model_for_cut);
        reset_instance_transformation(object, m_instance);
    };

    // cut by upper plane

    const Transform3d cut_matrix_upper = translation_transform(rotation_m * (groove_half_depth * Vec3d::UnitZ())) * m_cut_matrix;
    {
        cut(tmp_object, cut_matrix_upper, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
        add_volumes_from_cut(upper, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
    }

    // cut by lower plane

    const Transform3d cut_matrix_lower = translation_transform(rotation_m * (-groove_half_depth * Vec3d::UnitZ())) * m_cut_matrix;
    {
        cut(tmp_object, cut_matrix_lower, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
        add_volumes_from_cut(lower, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
    }

    // cut middle part with 2 angles and add parts to related upper/lower objects

    const double h_side_shift = 0.5 * double(groove.width + groove.depth / tan(groove.flaps_angle));

    // cut by angle1 plane
    {
        const Transform3d cut_matrix_angle1 = translation_transform(rotation_m * (-h_side_shift * Vec3d::UnitX())) * m_cut_matrix * rotation_transform(Vec3d(0, -groove.flaps_angle, -groove.angle));

        cut(tmp_object, cut_matrix_angle1, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
        add_volumes_from_cut(lower, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
    }

    // cut by angle2 plane
    {
        const Transform3d cut_matrix_angle2 = translation_transform(rotation_m * (h_side_shift * Vec3d::UnitX())) * m_cut_matrix * rotation_transform(Vec3d(0, groove.flaps_angle, groove.angle));

        cut(tmp_object, cut_matrix_angle2, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
        add_volumes_from_cut(lower, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
    }

    // apply tolerance to the middle part
    {
        const double h_groove_shift_tolerance = groove_half_depth - (double)groove.depth_tolerance;

        const Transform3d cut_matrix_lower_tolerance = translation_transform(rotation_m * (-h_groove_shift_tolerance * Vec3d::UnitZ())) * m_cut_matrix;
        cut(tmp_object, cut_matrix_lower_tolerance, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);

        const double h_side_shift_tolerance = h_side_shift - 0.5 * double(groove.width_tolerance);

        const Transform3d cut_matrix_angle1_tolerance = translation_transform(rotation_m * (-h_side_shift_tolerance * Vec3d::UnitX())) * m_cut_matrix * rotation_transform(Vec3d(0, -groove.flaps_angle, -groove.angle));
        cut(tmp_object, cut_matrix_angle1_tolerance, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);

        const Transform3d cut_matrix_angle2_tolerance = translation_transform(rotation_m * (h_side_shift_tolerance * Vec3d::UnitX())) * m_cut_matrix * rotation_transform(Vec3d(0, groove.flaps_angle, groove.angle));
        cut(tmp_object, cut_matrix_angle2_tolerance, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
    }

    // this part can be added to the upper object now
    add_volumes_from_cut(upper, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);

    ModelObjectPtrs cut_object_ptrs;

    if (keep_as_parts) {
        // add volumes from lower object to the upper, but mark them as a lower
        const auto& volumes = lower->volumes;
        for (const ModelVolume* volume : volumes) {
            ModelVolume* new_vol = upper->add_volume(*volume);
            new_vol->cut_info.is_from_upper = false;
        }

        // add modifiers
        for (const ModelVolume* volume : cut_mo->volumes)
            if (!volume->is_model_part())
                upper->add_volume(*volume);

        cut_object_ptrs.push_back(upper);

        // add lower object to the cut_object_ptrs just to correct delete it from the Model destructor and avoid memory leaks
        cut_object_ptrs.push_back(lower);
    }
    else {
        // add modifiers if object has any
        for (const ModelVolume* volume : cut_mo->volumes)
            if (!volume->is_model_part()) {
                distribute_modifiers_from_object(cut_mo, m_instance, upper, lower);
                break;
            }

        assert(!upper->volumes.empty() && !lower->volumes.empty());

        // Add Upper and Lower parts to cut_object_ptrs

        post_process(upper, lower, cut_object_ptrs);

        // Now merge all model parts together:
        merge_solid_parts_inside_object(cut_object_ptrs);
    }

    finalize(cut_object_ptrs);

    return m_model.objects;
}

} // namespace Slic3r

