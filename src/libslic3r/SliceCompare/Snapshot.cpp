#include "Snapshot.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <cmath>

namespace Slic3r { namespace SliceCompare {

double LayerRec::seconds() const {
    double t = 0; for (auto& kv : feature_seconds) t += kv.second; return t;
}

Snapshot build_snapshot(const GCodeProcessorResult& result,
                        std::map<std::string, std::string> config,
                        const std::string& label, const std::string& source)
{
    Snapshot s;
    s.label = label; s.source = source; s.config = std::move(config);
    s.est_seconds = result.print_statistics.modes[
        (size_t)PrintEstimatedStatistics::ETimeMode::Normal].time;

    const float diam = result.filament_diameters.empty() ? 1.75f : result.filament_diameters[0];
    const float dens = result.filament_densities.empty() ? 1.24f : result.filament_densities[0];
    // grams per mm of filament = area(cm^2) * 0.1cm * density(g/cm^3)
    const double g_per_mm = M_PI * (diam / 20.0) * (diam / 20.0) * 0.1 * dens;

    const GCodeProcessorResult::MoveVertex* prev = nullptr;
    for (const auto& m : result.moves) {
        if (m.feedrate > s.max_speed) s.max_speed = m.feedrate;
        if (m.type == EMoveType::Extrude && m.delta_extruder > 0.f && prev) {
            const int zkey = (int)std::lround(m.position.z() * 100.0);
            LayerRec& l = s.layers[zkey];
            if (l.segs.empty() && l.extrusion_mm == 0.0) l.z = m.position.z();
            const float x0 = prev->position.x(), y0 = prev->position.y();
            const float x1 = m.position.x(),    y1 = m.position.y();
            l.segs.push_back({x0, y0, x1, y1, (uint8_t)m.extrusion_role});
            l.extrusion_mm += m.delta_extruder;
            const double len = std::hypot((double)x1 - x0, (double)y1 - y0);
            if (m.feedrate > 0.f)
                l.feature_seconds[(uint8_t)m.extrusion_role] += len / m.feedrate;
            l.cells.insert({(int16_t)std::floor(x1 / 10.f), (int16_t)std::floor(y1 / 10.f)});
            l.cells.insert({(int16_t)std::floor(x0 / 10.f), (int16_t)std::floor(y0 / 10.f)});
            if (!l.has_bbox) { l.bx0 = l.bx1 = x1; l.by0 = l.by1 = y1; l.has_bbox = true; }
            l.bx0 = std::min({l.bx0, x0, x1}); l.by0 = std::min({l.by0, y0, y1});
            l.bx1 = std::max({l.bx1, x0, x1}); l.by1 = std::max({l.by1, y0, y1});
            s.filament_mm += m.delta_extruder;
        }
        prev = &m;
    }
    s.filament_g = s.filament_mm * g_per_mm;
    s.layer_count = (int)s.layers.size();
    return s;
}

SnapshotStore& SnapshotStore::instance()
{
    static SnapshotStore s;
    return s;
}

int SnapshotStore::add(Snapshot snap)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const int id = m_next_id++;
    m_items.push_front({id, std::make_shared<Snapshot>(std::move(snap))});
    while (m_items.size() > 8)
        m_items.pop_back();
    return id;
}

std::shared_ptr<const Snapshot> SnapshotStore::get(int id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_items)
        if (item.first == id)
            return item.second;
    return nullptr;
}

std::vector<std::pair<int, std::string>> SnapshotStore::list() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::pair<int, std::string>> out;
    out.reserve(m_items.size());
    for (const auto& item : m_items)
        out.push_back({item.first, item.second->label});
    return out;
}

void SnapshotStore::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.clear();
}

}} // namespaces
