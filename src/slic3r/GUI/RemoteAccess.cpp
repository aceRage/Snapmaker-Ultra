#include "RemoteAccess.hpp"

#include "DeviceManager.hpp"
#include "GLCanvas3D.hpp"
#include "GLToolbar.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "OptionsGroup.hpp"
#include "PresetComboBoxes.hpp"
#include "RemoteHub.hpp"
#include "Tab.hpp"
#include <climits>
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <future>
#include <memory>
#include <sstream>
#include <thread>

#include <wx/utils.h>

namespace Slic3r {
namespace GUI {

namespace asio = boost::asio;
namespace fs   = boost::filesystem;
using tcp      = asio::ip::tcp;

// ---------------------------------------------------------------- helpers ----

static std::string percent_decode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char) s[i + 1]) && std::isxdigit((unsigned char) s[i + 2])) {
            out += (char) std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::string query_param(const std::string& query, const std::string& key)
{
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string kv = query.substr(pos, amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key)
            return percent_decode(kv.substr(eq + 1));
        pos = amp + 1;
    }
    return "";
}

static void write_all(tcp::socket& s, const std::string& data)
{
    asio::write(s, asio::buffer(data));
}

static void respond(tcp::socket& s, const char* status, const std::string& type, const std::string& body,
                    const std::string& extra_headers = "")
{
    std::ostringstream o;
    o << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << extra_headers
      << "Connection: close\r\n\r\n"
      << body;
    write_all(s, o.str());
}

// Run fn on the GUI thread and wait for it (Plater, plates and devices are GUI-thread only).
// Returns false on timeout — e.g. a modal dialog is blocking the app — and fn may still run
// later, so callers only capture shared state.
static int s_auto_confirm_depth = 0; // GUI thread only
bool RemoteAccess::auto_confirm() { return s_auto_confirm_depth > 0; }
RemoteAccess::AutoConfirmScope::AutoConfirmScope() { ++s_auto_confirm_depth; }
RemoteAccess::AutoConfirmScope::~AutoConfirmScope() { --s_auto_confirm_depth; }

static bool run_on_main(std::function<void()> fn, int timeout_ms = 15000)
{
    auto done = std::make_shared<std::promise<void>>();
    auto fut  = done->get_future();
    wxGetApp().CallAfter([done, fn]() {
        RemoteAccess::AutoConfirmScope auto_yes;
        try { fn(); } catch (...) {}
        done->set_value();
    });
    return fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready;
}

static std::string json_error(const std::string& msg)
{
    nlohmann::json j;
    j["error"] = msg;
    return j.dump();
}

// ------------------------------------------------------------ RemoteAccess ----

RemoteAccess& RemoteAccess::get()
{
    static RemoteAccess instance;
    return instance;
}

void RemoteAccess::start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_on)
        return;
    try {
        static asio::io_context ioc; // lives for the process
        auto* acceptor = new tcp::acceptor(ioc, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        acceptor->listen();
        m_acceptor = acceptor;
        m_port     = acceptor->local_endpoint().port();
        m_on       = true;
        std::thread([this]() { accept_loop(); }).detach();
        write_instance_file();
        BOOST_LOG_TRIVIAL(info) << "RemoteAccess: instance API on 127.0.0.1:" << m_port;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "RemoteAccess: failed to start: " << e.what();
        m_on   = false;
        m_port = 0;
    }
}

void RemoteAccess::stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_on)
        return;
    m_on = false;
    boost::system::error_code ig;
    fs::remove(fs::path(RemoteHub::instances_dir()) / (std::to_string(wxGetProcessId()) + ".json"), ig);
    if (auto* acceptor = static_cast<tcp::acceptor*>(m_acceptor)) {
        acceptor->close(ig); // unblocks accept_loop, which deletes the acceptor
        m_acceptor = nullptr;
    }
    m_port = 0;
}

int RemoteAccess::port()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_port;
}

// <datadir>/hub/instances/<pid>.json — how the hub finds this instance (m_mutex held).
void RemoteAccess::write_instance_file()
{
    boost::system::error_code ec;
    fs::create_directories(RemoteHub::instances_dir(), ec);
    nlohmann::json j;
    j["pid"]     = (long) wxGetProcessId();
    j["port"]    = m_port;
    j["started"] = (long long) std::time(nullptr);
    j["version"] = std::string(SLIC3R_VERSION);
    boost::nowide::ofstream f((fs::path(RemoteHub::instances_dir()) / (std::to_string(wxGetProcessId()) + ".json")).string(), std::ios::trunc);
    f << j.dump(2);
}

void RemoteAccess::note_project(const std::string& title, const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_title = title;
    m_path  = path;
}

void RemoteAccess::note_error(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_last_error = message;
}

std::string RemoteAccess::take_error()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string e;
    e.swap(m_last_error);
    return e;
}

// ---------------------------------------------------------------- JSON API ----

void RemoteAccess::note_slice_progress(int plate, int percent, const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_slicing = true;
    for (Job& j : m_jobs)
        if (j.state == "running" && (j.plate == -1 || j.plate == plate)) {
            if (percent >= 0) j.percent = std::max(j.percent, std::min(percent, 99));
            j.text = text;
        }
}

void RemoteAccess::note_slice_done(bool finished_all, bool ok, const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_slicing = false;
    for (Job& j : m_jobs)
        if (j.state == "running") {
            if (!ok) {
                j.state = error.empty() ? "cancelled" : "error";
                j.error = error;
            } else if (finished_all || j.plate != -1) {
                j.state   = "done";
                j.percent = 100;
            }
        }
}

RemoteAccess::ApiResponse RemoteAccess::api_plates()
{
    auto out = std::make_shared<nlohmann::json>();
    bool ok  = run_on_main([out]() {
        Plater*        plater = wxGetApp().plater();
        PresetBundle*  bundle = wxGetApp().preset_bundle;
        PartPlateList& plates = plater->get_partplate_list();
        nlohmann::json& j     = *out;
        j["project"]       = plater->get_project_filename().ToUTF8().data();
        j["printer"]       = bundle->printers.get_selected_preset_name();
        j["current_plate"] = plates.get_curr_plate_index();
        j["slicing"]       = plater->is_background_process_slicing();
        j["filaments"]     = nlohmann::json::array();
        std::vector<double> density;
        if (auto* d = bundle->full_config().option<ConfigOptionFloats>("filament_density"))
            density = d->values;
        const ConfigOptionStrings* colors = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
            nlohmann::json f;
            f["name"]  = bundle->filament_presets[i];
            f["color"] = (colors && i < colors->values.size()) ? colors->values[i] : "";
            j["filaments"].push_back(f);
        }
        j["plates"] = nlohmann::json::array();
        for (int i = 0; i < plates.get_plate_count(); ++i) {
            PartPlate*     p = plates.get_plate(i);
            nlohmann::json jp;
            jp["index"]   = i;
            jp["name"]    = p->get_plate_name();
            jp["objects"] = nlohmann::json::array();
            jp["boxes"]   = nlohmann::json::array(); // per object: [min_x, min_y, max_x, max_y] in plate-list mm
            for (const ModelObject* o : p->get_objects_on_this_plate()) {
                jp["objects"].push_back(o->name);
                const BoundingBoxf3 bb = o->bounding_box_exact();
                jp["boxes"].push_back({ bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y() });
            }
            const BoundingBoxf3& pb = p->get_bounding_box();
            jp["plate_box"] = { pb.min.x(), pb.min.y(), pb.max.x(), pb.max.y() };
            jp["printable"]       = p->has_printable_instances();
            jp["locked"]          = p->is_locked();
            jp["sliced"]          = p->is_slice_result_valid();
            jp["ready_for_print"] = p->is_slice_result_ready_for_print();
            jp["slicing_percent"] = p->get_slicing_percent();
            if (p->is_slice_result_valid() && p->get_slice_result()) {
                const auto& st = p->get_slice_result()->print_statistics;
                if (!st.modes.empty())
                    jp["time_s"] = st.modes.front().time;
                double mm3 = 0, grams = 0;
                for (const auto& kv : st.total_volumes_per_extruder) {
                    mm3 += kv.second;
                    const double dens = kv.first < density.size() ? density[kv.first] : 1.24;
                    grams += kv.second / 1000.0 * dens;
                }
                jp["filament_mm3"] = mm3;
                jp["filament_g"]   = grams;
            }
            j["plates"].push_back(jp);
        }
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else       r.body = out->dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_plate_thumbnail(int plate)
{
    auto data = std::make_shared<ThumbnailData>();
    bool ok   = run_on_main([data, plate]() {
        Plater*        plater = wxGetApp().plater();
        PartPlateList& plates = plater->get_partplate_list();
        if (plate < 0 || plate >= plates.get_plate_count())
            return;
        plater->update_all_plate_thumbnails(false);
        *data = plates.get_plate(plate)->thumbnail_data;
    }, 30000);
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (!data->is_valid()) { r.status = 404; r.body = json_error("no thumbnail for this plate"); return r; }
    auto png = GCodeThumbnails::compress_thumbnail(*data, GCodeThumbnailsFormat::PNG);
    r.type   = "image/png";
    r.body.assign(static_cast<const char*>(png->data), png->size);
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_printers()
{
    auto out = std::make_shared<nlohmann::json>();
    bool ok  = run_on_main([out]() {
        nlohmann::json& j = *out;
        j["printers"]     = nlohmann::json::array();
        DeviceManager* dm = wxGetApp().getDeviceManager();
        if (!dm)
            return;
        MachineObject* selected = dm->get_selected_machine();
        std::map<std::string, MachineObject*> all = dm->get_my_machine_list();
        for (const auto& kv : dm->get_local_machine_list())
            all.insert(kv);
        for (const auto& kv : all) {
            MachineObject* m = kv.second;
            if (!m) continue;
            nlohmann::json p;
            p["id"]           = m->dev_id;
            p["name"]         = m->dev_name;
            p["model"]        = m->printer_type;
            p["online"]       = m->is_online();
            p["connected"]    = m->is_connected();
            p["status"]       = m->print_status;
            p["printing"]     = m->is_in_printing();
            p["percent"]      = m->mc_print_percent;
            p["left_time_s"]  = m->mc_left_time;
            p["layer"]        = m->curr_layer;
            p["total_layers"] = m->total_layers;
            p["task"]         = m->subtask_name;
            p["bed_temp"]     = m->bed_temp;
            p["bed_target"]   = m->bed_temp_target;
            p["nozzles"]      = nlohmann::json::array();
            for (const Extder& e : m->m_extder_data.extders) {
                nlohmann::json n;
                n["temp"]   = e.temp;
                n["target"] = e.target_temp;
                p["nozzles"].push_back(n);
            }
            p["selected"] = (selected == m);
            j["printers"].push_back(p);
        }
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else       r.body = out->dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_slice(int plate, bool all)
{
    auto result = std::make_shared<std::pair<int, std::string>>(500, "");
    bool ok     = run_on_main([result, plate, all]() {
        Plater*        plater = wxGetApp().plater();
        PartPlateList& plates = plater->get_partplate_list();
        if (plater->is_background_process_slicing()) { *result = { 409, "already slicing" }; return; }
        if (!all && (plate < 0 || plate >= plates.get_plate_count())) { *result = { 404, "no such plate" }; return; }
        if (all && !plater->has_sliceable_plate_for_slice_all()) { *result = { 409, "nothing to slice" }; return; }
        if (!all) {
            if (!plates.get_plate(plate)->has_printable_instances()) { *result = { 409, "plate is empty" }; return; }
            plater->select_plate(plate);
        }
        plater->exit_gizmo();
        plater->update(true, true);
        wxPostEvent(plater, SimpleEvent(all ? EVT_GLTOOLBAR_SLICE_ALL : EVT_GLTOOLBAR_SLICE_PLATE));
        *result = { 200, "" };
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second); return r; }
    Job job;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job.id    = m_next_job++;
        job.plate = all ? -1 : plate;
        job.state = "running";
        m_jobs.push_back(job);
        if (m_jobs.size() > 50)
            m_jobs.erase(m_jobs.begin());
    }
    nlohmann::json j;
    j["job"]   = job.id;
    j["plate"] = job.plate;
    r.body     = j.dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_jobs(int id)
{
    ApiResponse r;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto to_json = [](const Job& j) {
        nlohmann::json o;
        o["id"] = j.id; o["plate"] = j.plate; o["state"] = j.state; o["percent"] = j.percent;
        o["text"] = j.text; o["error"] = j.error;
        return o;
    };
    if (id < 0) {
        nlohmann::json j;
        j["jobs"] = nlohmann::json::array();
        for (const Job& job : m_jobs) j["jobs"].push_back(to_json(job));
        r.body = j.dump();
        return r;
    }
    for (const Job& job : m_jobs)
        if (job.id == id) { r.body = to_json(job).dump(); return r; }
    r.status = 404;
    r.body   = json_error("no such job");
    return r;
}

// The sidebar's own combo boxes are the source of truth for "what can be picked" and selecting
// through them runs the exact code path a click in the sidebar does (Plater::priv::on_select_preset).
static bool is_label_item(PresetComboBox* combo, unsigned int i)
{
    const size_t marker = reinterpret_cast<size_t>(combo->GetClientData(i));
    return marker >= PresetComboBox::LABEL_ITEM_MARKER && marker < PresetComboBox::LABEL_ITEM_MAX;
}

static std::string combo_item_value(PresetComboBox* combo, unsigned int i, Preset::Type type)
{
    const std::string display = combo->GetString(i).ToUTF8().data();
    return wxGetApp().preset_bundle->get_preset_name_by_alias(type, Preset::remove_suffix_modified(display));
}

// The process preset has no sidebar combo in this fork; its list lives on the Process tab.
static PresetComboBox* process_combo()
{
    Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
    return tab ? tab->get_combo_box() : nullptr;
}

static nlohmann::json combo_items(PresetComboBox* combo, Preset::Type type)
{
    nlohmann::json items = nlohmann::json::array();
    if (!combo)
        return items;
    const int sel = combo->GetSelection();
    for (unsigned int i = 0; i < combo->GetCount(); ++i) {
        if (is_label_item(combo, i))
            continue;
        nlohmann::json it;
        it["name"]     = combo->GetString(i).ToUTF8().data();
        it["value"]    = combo_item_value(combo, i, type);
        it["selected"] = ((int) i == sel);
        items.push_back(it);
    }
    return items;
}

RemoteAccess::ApiResponse RemoteAccess::api_presets()
{
    auto out = std::make_shared<nlohmann::json>();
    bool ok  = run_on_main([out]() {
        nlohmann::json& j      = *out;
        Sidebar&        sb     = wxGetApp().sidebar();
        PresetBundle*   bundle = wxGetApp().preset_bundle;
        j["printer"]      = combo_items(sb.combo_printer(), Preset::TYPE_PRINTER);
        j["process"]      = combo_items(process_combo(), Preset::TYPE_PRINT);
        j["printer_name"] = bundle->printers.get_selected_preset_name();
        j["process_name"] = bundle->prints.get_selected_preset_name();
        auto& combos = sb.combos_filament();
        j["filament_choices"] = combos.empty() ? nlohmann::json::array() : combo_items(combos.front(), Preset::TYPE_FILAMENT);
        j["filaments"]        = nlohmann::json::array();
        const ConfigOptionStrings* colors = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
            nlohmann::json f;
            f["index"] = i;
            f["value"] = bundle->filament_presets[i];
            f["color"] = (colors && i < colors->values.size()) ? colors->values[i] : "";
            j["filaments"].push_back(f);
        }
        j["dirty"]["printer"]  = bundle->printers.current_is_dirty();
        j["dirty"]["process"]  = bundle->prints.current_is_dirty();
        j["dirty"]["filament"] = bundle->filaments.current_is_dirty();
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else       r.body = out->dump();
    return r;
}

// Unsaved modifications of a preset collection, ready to be re-applied to the newly selected
// preset (what the transfer/discard dialog's "Transfer" button does).
static DynamicPrintConfig capture_dirty(PresetCollection& presets)
{
    DynamicPrintConfig dirty;
    if (!presets.current_is_dirty())
        return dirty;
    const Preset& edited = presets.get_edited_preset();
    for (const std::string& opt : presets.current_dirty_options())
        if (const ConfigOption* o = edited.config.option(opt))
            dirty.set_key_value(opt, o->clone());
    return dirty;
}

// Phone selections never raise the transfer/discard dialog: modifications are carried over to
// the new preset (they stay "modified" there, so Revert on the PC still discards them).
RemoteAccess::ApiResponse RemoteAccess::api_select_preset(const std::string& type, const std::string& name_in, int index)
{
    auto result = std::make_shared<std::pair<int, std::string>>(500, "");
    bool ok     = run_on_main([result, type, name_in, index]() {
        const std::string& name = name_in;
        Sidebar&      sb     = wxGetApp().sidebar();
        Plater*       plater = wxGetApp().plater();
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (plater->is_background_process_slicing()) { *result = { 409, "slicing in progress" }; return; }
        auto reapply = [](Preset::Type t, const DynamicPrintConfig& dirty) {
            if (!dirty.empty())
                wxGetApp().get_tab(t)->load_config(dirty);
        };
        if (type == "printer") {
            std::string name = name_in;
            if (name == bundle->printers.get_selected_preset_name()) { *result = { 200, "" }; return; }
            if (!bundle->printers.find_preset(name)) {
                // The sidebar lists printer models ("Bambu Lab H2D") as well as presets; resolve a
                // model to its preset the way Plater::priv::on_select_preset does.
                Preset* similar = bundle->get_similar_printer_preset(name, {});
                if (!similar) { *result = { 404, "preset not in the list: " + name }; return; }
                similar->is_visible = true;
                name = similar->name;
                if (name == bundle->printers.get_selected_preset_name()) { *result = { 200, "" }; return; }
            }
            const DynamicPrintConfig printer_dirty = capture_dirty(bundle->printers);
            const DynamicPrintConfig process_dirty = capture_dirty(bundle->prints);
            const DynamicPrintConfig filament_dirty = capture_dirty(bundle->filaments);
            bundle->physical_printers.unselect_printer();
            // force_select: no dialogs; incompatible process/filament presets are remapped silently.
            wxGetApp().get_tab(Preset::TYPE_PRINTER)->select_preset(name, false, "", true);
            reapply(Preset::TYPE_PRINTER, printer_dirty);
            reapply(Preset::TYPE_PRINT, process_dirty);
            reapply(Preset::TYPE_FILAMENT, filament_dirty);
            plater->on_config_change(bundle->full_config());
            wxGetApp().app_config->set("preferred_printer", bundle->printers.get_selected_preset_name());
            *result = { 200, "" };
        } else if (type == "process") {
            if (name == bundle->prints.get_selected_preset_name()) { *result = { 200, "" }; return; }
            if (!bundle->prints.find_preset(name)) { *result = { 404, "preset not in the list: " + name }; return; }
            const DynamicPrintConfig process_dirty = capture_dirty(bundle->prints);
            wxGetApp().get_tab(Preset::TYPE_PRINT)->select_preset(name, false, "", true);
            reapply(Preset::TYPE_PRINT, process_dirty);
            plater->on_config_change(bundle->full_config());
            plater->record_preferred_print_profile();
            *result = { 200, "" };
        } else if (type == "filament") {
            auto& combos = sb.combos_filament();
            if (index < 0 || index >= (int) combos.size()) { *result = { 404, "no such filament slot" }; return; }
            if (index < (int) bundle->filament_presets.size() && bundle->filament_presets[index] == name) { *result = { 200, "" }; return; }
            if (!bundle->filaments.find_preset(name)) { *result = { 404, "preset not in the list: " + name }; return; }
            // Same steps as Plater::priv::on_select_preset for TYPE_FILAMENT, with force on the tab.
            const DynamicPrintConfig filament_dirty = capture_dirty(bundle->filaments);
            bundle->set_filament_preset(index, name);
            plater->update_project_dirty_from_presets();
            bundle->export_selections(*wxGetApp().app_config);
            sb.update_dynamic_filament_list();
            sb.update_color_mix_panel();
            if (sb.is_multifilament())
                combos[index]->update();
            else {
                wxGetApp().get_tab(Preset::TYPE_FILAMENT)->select_preset(name, false, "", true);
                reapply(Preset::TYPE_FILAMENT, filament_dirty);
            }
            plater->on_config_change(bundle->full_config());
            *result = { 200, "" };
        } else {
            *result = { 404, "type must be printer, process or filament" };
        }
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second); return r; }
    r.body = "{\"ok\":true}";
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_filament_color(int index, const std::string& color)
{
    ApiResponse r;
    if (color.size() != 7 || color[0] != '#' || color.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos) {
        r.status = 404; r.body = json_error("color must be #RRGGBB"); return r;
    }
    auto result = std::make_shared<int>(500);
    bool ok     = run_on_main([result, index, color]() {
        auto& combos = wxGetApp().sidebar().combos_filament();
        if (index < 0 || index >= (int) combos.size()) { *result = 404; return; }
        combos[index]->ApplyFilamentColor(FilamentColor::FromColors({ color }, FilamentColorMode::Segment));
        *result = 200;
    });
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (*result != 200) { r.status = *result; r.body = json_error("no such filament slot"); return r; }
    r.body = "{\"ok\":true}";
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_filament_add()
{
    auto count = std::make_shared<size_t>(0);
    bool ok    = run_on_main([count]() {
        wxGetApp().sidebar().add_filament();
        *count = wxGetApp().preset_bundle->filament_presets.size();
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    nlohmann::json j;
    j["filaments"] = *count;
    r.body = j.dump();
    return r;
}

// ------------------------------------------------- process settings editor ----

static const char* option_type_name(ConfigOptionType t)
{
    switch (t) {
    case coFloat: return "float";           case coFloats: return "floats";
    case coInt: return "int";               case coInts: return "ints";
    case coString: return "string";         case coStrings: return "strings";
    case coPercent: return "percent";       case coPercents: return "percents";
    case coFloatOrPercent: return "float_or_percent"; case coFloatsOrPercents: return "floats_or_percents";
    case coPoint: return "point";           case coPoints: return "points";
    case coBool: return "bool";             case coBools: return "bools";
    case coEnum: return "enum";             case coEnums: return "enums";
    default: return "other";
    }
}

static const char* mode_name(ConfigOptionMode m)
{
    return m == comSimple ? "simple" : m == comAdvanced ? "advanced" : "develop";
}

static nlohmann::json process_state_json()
{
    nlohmann::json j;
    PresetBundle* bundle = wxGetApp().preset_bundle;
    const Preset& edited = bundle->prints.get_edited_preset();
    j["preset"]    = edited.name;
    j["is_system"] = edited.is_system;
    j["dirty"]     = bundle->prints.current_dirty_options();
    return j;
}

// The Process tab's pages / option groups / lines, with the definition, current value and the
// last-saved value of every option — the phone renders exactly the slicer's layout from this.
RemoteAccess::ApiResponse RemoteAccess::api_process_settings()
{
    auto out = std::make_shared<nlohmann::json>();
    bool ok  = run_on_main([out]() {
        nlohmann::json& j      = *out;
        PresetBundle*   bundle = wxGetApp().preset_bundle;
        Tab*            tab    = wxGetApp().get_tab(Preset::TYPE_PRINT);
        if (!tab) return;
        j        = process_state_json();
        j["mode"] = mode_name(wxGetApp().get_mode());
        const DynamicPrintConfig& edited = bundle->prints.get_edited_preset().config;
        const DynamicPrintConfig& saved  = bundle->prints.get_selected_preset().config;
        std::set<std::string> dirty;
        for (const std::string& k : bundle->prints.current_dirty_options()) dirty.insert(k);
        j["pages"] = nlohmann::json::array();
        for (const PageShp& page : tab->get_pages()) {
            nlohmann::json jp;
            jp["title"]  = page->title().ToUTF8().data();
            jp["groups"] = nlohmann::json::array();
            for (const ConfigOptionsGroupShp& group : page->m_optgroups) {
                nlohmann::json jg;
                jg["title"] = group->title.ToUTF8().data();
                jg["lines"] = nlohmann::json::array();
                for (const Line& line : group->get_lines()) {
                    if (line.is_separator()) continue;
                    nlohmann::json jl;
                    jl["label"]   = line.label.ToUTF8().data();
                    jl["tooltip"] = line.label_tooltip.ToUTF8().data();
                    jl["options"] = nlohmann::json::array();
                    for (const Option& opt : line.get_options()) {
                        std::string key = opt.opt_id;
                        int         idx = -1;
                        const size_t hash = key.find('#');
                        if (hash != std::string::npos) { idx = std::atoi(key.c_str() + hash + 1); key = key.substr(0, hash); }
                        const ConfigOption* cur = edited.option(key);
                        if (!cur) continue;
                        const ConfigOption* sav = saved.option(key);
                        const ConfigOptionDef& def = opt.opt;
                        nlohmann::json jo;
                        jo["key"]     = key;
                        if (idx >= 0) jo["index"] = idx;
                        jo["label"]   = _(def.label).ToUTF8().data();
                        jo["tooltip"] = _(def.tooltip).ToUTF8().data();
                        jo["type"]    = option_type_name(def.type);
                        jo["unit"]    = _(def.sidetext).ToUTF8().data();
                        jo["mode"]    = mode_name(def.mode);
                        jo["readonly"] = def.readonly || opt.readonly;
                        if (def.min != INT_MIN) jo["min"] = def.min;
                        if (def.max != INT_MAX) jo["max"] = def.max;
                        if (!def.enum_values.empty()) {
                            jo["enum"] = nlohmann::json::array();
                            for (size_t e = 0; e < def.enum_values.size(); ++e) {
                                nlohmann::json je;
                                je["value"] = def.enum_values[e];
                                je["label"] = e < def.enum_labels.size() ? _(def.enum_labels[e]).ToUTF8().data() : def.enum_values[e];
                                jo["enum"].push_back(je);
                            }
                        }
                        jo["value"] = cur->serialize();
                        jo["saved"] = sav ? sav->serialize() : "";
                        jo["dirty"] = dirty.count(key) > 0;
                        jl["options"].push_back(jo);
                    }
                    if (!jl["options"].empty())
                        jg["lines"].push_back(jl);
                }
                if (!jg["lines"].empty())
                    jp["groups"].push_back(jg);
            }
            if (!jp["groups"].empty())
                j["pages"].push_back(jp);
        }
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else if (out->empty()) { r.status = 404; r.body = json_error("no process tab"); }
    else r.body = out->dump();
    return r;
}

// Apply key=value pairs (form-encoded, serialized option values) to the edited process
// preset, exactly like typing them into the Process tab; the preset becomes "modified".
RemoteAccess::ApiResponse RemoteAccess::api_process_set(const std::string& form_body)
{
    std::vector<std::pair<std::string, std::string>> pairs;
    size_t pos = 0;
    while (pos <= form_body.size()) {
        size_t amp = form_body.find('&', pos);
        if (amp == std::string::npos) amp = form_body.size();
        const std::string kv = form_body.substr(pos, amp - pos);
        const size_t      eq = kv.find('=');
        if (eq != std::string::npos && eq > 0)
            pairs.emplace_back(percent_decode(kv.substr(0, eq)), percent_decode(kv.substr(eq + 1)));
        pos = amp + 1;
    }
    ApiResponse r;
    if (pairs.empty()) { r.status = 404; r.body = json_error("no key=value pairs"); return r; }
    auto result = std::make_shared<std::pair<int, std::string>>(500, "");
    auto state  = std::make_shared<nlohmann::json>();
    bool ok     = run_on_main([result, state, pairs]() {
        Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
        if (!tab) { *result = { 404, "no process tab" }; return; }
        DynamicPrintConfig cfg;
        for (const auto& kv : pairs) {
            if (!print_config_def.has(kv.first)) { *result = { 404, "unknown setting: " + kv.first }; return; }
            try {
                cfg.set_deserialize_strict(kv.first, kv.second);
            } catch (const std::exception& e) {
                *result = { 400, "bad value for " + kv.first + ": " + e.what() };
                return;
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        tab->load_config(cfg);
        const auto t1 = std::chrono::steady_clock::now();
        wxGetApp().plater()->on_config_change(cfg);
        const auto t2 = std::chrono::steady_clock::now();
        BOOST_LOG_TRIVIAL(info) << "RemoteAccess: process settings applied, load_config "
                                << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << " ms, on_config_change "
                                << std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count() << " ms";
        *state  = process_state_json();
        *result = { 200, "" };
    }, 60000); // the first apply after a project load has been seen to take >15 s
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second); return r; }
    r.body = state->dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_process_revert()
{
    auto state = std::make_shared<nlohmann::json>();
    bool ok    = run_on_main([state]() {
        Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
        if (!tab) return;
        tab->on_roll_back_value(false); // "reset all settings to the last saved preset"
        wxGetApp().plater()->on_config_change(wxGetApp().preset_bundle->full_config());
        *state = process_state_json();
    }, 60000);
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else       r.body = state->dump();
    return r;
}

// Save under the same name; a system preset saves to its "<name> - Custom" shadow (the fork's
// own no-prompt rule in Tab::save_preset), which then becomes the selected preset.
RemoteAccess::ApiResponse RemoteAccess::api_process_save()
{
    auto state = std::make_shared<nlohmann::json>();
    bool ok    = run_on_main([state]() {
        Tab*          tab    = wxGetApp().get_tab(Preset::TYPE_PRINT);
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (!tab) return;
        const Preset& edited = bundle->prints.get_edited_preset();
        if (edited.is_dirty)
            tab->save_preset(edited.is_system ? std::string() : edited.name);
        *state = process_state_json();
    }, 30000);
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else       r.body = state->dump();
    return r;
}

// --------------------------------------------------------- instance + files ----

// Cheap (no GUI thread): what the hub lists for this instance.
RemoteAccess::ApiResponse RemoteAccess::api_info()
{
    ApiResponse r;
    nlohmann::json j;
    std::lock_guard<std::mutex> lock(m_mutex);
    j["pid"]     = (long) wxGetProcessId();
    j["port"]    = m_port;
    j["title"]   = m_title;
    j["path"]    = m_path;
    j["slicing"] = m_slicing;
    j["version"] = std::string(SLIC3R_VERSION);
    r.body       = j.dump();
    return r;
}

// Open a file the hub spooled from the phone. mode=load (.3mf only): the project on screen
// is saved first — to its own file, or to <datadir>/hub/saves when it was never saved —
// then the uploaded project replaces it. mode=import: the model is added to the current
// plate. Prompts along the way answer themselves (auto_confirm).
RemoteAccess::ApiResponse RemoteAccess::api_project_open(const std::string& path_in, const std::string& mode_in)
{
    ApiResponse r;
    boost::system::error_code ec;
    const fs::path path    = fs::weakly_canonical(fs::path(path_in), ec);
    const fs::path uploads = fs::weakly_canonical(fs::path(RemoteHub::uploads_dir()), ec);
    if (path_in.empty() || !fs::is_regular_file(path, ec)) { r.status = 404; r.body = json_error("no such file"); return r; }
    if (path.parent_path().parent_path() != uploads) { r.status = 400; r.body = json_error("only files uploaded through the hub can be opened"); return r; }
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    const std::string mode = mode_in.empty() ? (ext == ".3mf" ? "load" : "import") : mode_in;
    if (mode != "load" && mode != "import") { r.status = 400; r.body = json_error("mode must be load or import"); return r; }
    if (mode == "load" && ext != ".3mf") { r.status = 400; r.body = json_error("only a .3mf can be loaded as a project; use mode=import"); return r; }

    auto result = std::make_shared<std::pair<int, std::string>>(500, "");
    auto out    = std::make_shared<nlohmann::json>();
    take_error();
    bool ok     = run_on_main([result, out, path, mode]() {
        Plater* plater = wxGetApp().plater();
        if (plater->is_background_process_slicing()) { *result = { 409, "slicing in progress" }; return; }
        const wxString file = wxString::FromUTF8(path.string());
        if (mode == "load") {
            if (!plater->model().objects.empty() || plater->is_project_dirty()) {
                wxString current = plater->get_project_filename(".3mf");
                if (current.IsEmpty()) {
                    boost::system::error_code ig;
                    fs::create_directories(RemoteHub::saves_dir(), ig);
                    std::time_t t = std::time(nullptr);
                    std::tm     tm {};
#ifdef _WIN32
                    localtime_s(&tm, &t);
#else
                    localtime_r(&t, &tm);
#endif
                    char stamp[32];
                    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
                    current = wxString::FromUTF8((fs::path(RemoteHub::saves_dir()) / (std::string("Untitled_") + stamp + ".3mf")).string());
                    plater->set_project_filename(current);
                }
                if (plater->save_project(false) != wxID_YES) { *result = { 500, "could not save the current project" }; return; }
                (*out)["saved"] = current.ToUTF8().data();
            }
            // "<loadall>" skips the open-project-or-import-geometry question.
            plater->load_project(file, "<loadall>");
            boost::system::error_code ig;
            const fs::path now(plater->get_project_filename(".3mf").ToUTF8().data());
            if (!fs::exists(now, ig) || !fs::equivalent(now, path, ig)) { *result = { 500, "the project did not load" }; return; }
        } else {
            const std::vector<std::string> files { path.string() };
            const std::vector<size_t>      res = plater->load_files(files, LoadStrategy::LoadModel);
            if (res.empty()) { *result = { 500, "nothing was imported" }; return; }
            (*out)["objects"] = res.size();
        }
        wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
        (*out)["project"] = plater->get_project_filename(".3mf").ToUTF8().data();
        *result           = { 200, "" };
    }, 15 * 60 * 1000); // big projects take a while to save and load
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    const std::string shown = take_error(); // what the slicer would have put in a dialog
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second + (shown.empty() ? "" : ": " + shown)); return r; }
    r.body = out->dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::handle_api(const std::string& method, const std::string& path, const std::string& query, const std::string& body)
{
    ApiResponse r;
    auto num = [](const std::string& s, int def) { try { return s.empty() ? def : std::stoi(s); } catch (...) { return def; } };
    if (path.empty() || path == "/") {
        nlohmann::json j;
        j["name"]    = "Snapmaker-Ultra remote API";
        j["version"] = 2;
        j["routes"]  = nlohmann::json::array({
            { {"method", "GET"},  {"path", "/api"},                        {"description", "this manifest"} },
            { {"method", "GET"},  {"path", "/api/info"},                   {"description", "this instance: pid, project title and path, slicing flag"} },
            { {"method", "POST"}, {"path", "/api/project/open"},           {"description", "form body path={file uploaded through the hub}&mode=load|import; load (.3mf) saves the current project first, import adds the model to the plate"} },
            { {"method", "GET"},  {"path", "/api/plates"},                 {"description", "project, printer preset, filaments and every plate with objects, slice state, time and filament estimates"} },
            { {"method", "GET"},  {"path", "/api/plates/{index}/thumbnail.png"}, {"description", "rendered plate preview"} },
            { {"method", "GET"},  {"path", "/api/printers"},               {"description", "known printers with live status"} },
            { {"method", "POST"}, {"path", "/api/slice?plate={index}|all"}, {"description", "start slicing one plate (selects it) or all; returns a job id; 409 while slicing"} },
            { {"method", "GET"},  {"path", "/api/jobs"},                   {"description", "recent jobs"} },
            { {"method", "GET"},  {"path", "/api/jobs/{id}"},              {"description", "job state: running | done | error | cancelled, percent, text"} },
            { {"method", "GET"},  {"path", "/api/presets"},                {"description", "printer / process choices as the sidebar shows them, filament choices and the current filament slots with colours"} },
            { {"method", "POST"}, {"path", "/api/presets/select?type=printer|process|filament&name={value}[&index={slot}]"}, {"description", "select a preset the way the sidebar does; 409 when that preset has unsaved changes on the PC"} },
            { {"method", "POST"}, {"path", "/api/presets/filament_color?index={slot}&color=%23RRGGBB"}, {"description", "set a filament slot colour"} },
            { {"method", "POST"}, {"path", "/api/presets/filament_add"},   {"description", "add a filament slot"} },
            { {"method", "GET"},  {"path", "/api/settings/process"},       {"description", "the Process tab: pages > groups > lines > options with definition, current and saved values, dirty flags, app mode"} },
            { {"method", "POST"}, {"path", "/api/settings/process"},       {"description", "form body key=value[&key=value…] (serialized option values); applies like typing into the tab, returns preset/dirty state"} },
            { {"method", "POST"}, {"path", "/api/settings/process/revert"}, {"description", "reset all settings to the last saved preset"} },
            { {"method", "POST"}, {"path", "/api/settings/process/save"},  {"description", "save modifications under the same name (system presets save to '<name> - Custom')"} }
        });
        r.body = j.dump();
        return r;
    }
    if (path == "/info" && method == "GET")
        return api_info();
    if (path == "/project/open" && method == "POST") {
        auto get = [&](const char* k) { std::string v = query_param(body, k); return v.empty() ? query_param(query, k) : v; };
        return api_project_open(get("path"), get("mode"));
    }
    if (path == "/settings/process" && method == "GET")
        return api_process_settings();
    if (path == "/settings/process" && method == "POST")
        return api_process_set(body.empty() ? query : body);
    if (path == "/settings/process/revert" && method == "POST")
        return api_process_revert();
    if (path == "/settings/process/save" && method == "POST")
        return api_process_save();
    if (path == "/presets" && method == "GET")
        return api_presets();
    if (path == "/presets/select" && method == "POST") {
        auto get = [&](const char* k) { std::string v = query_param(query, k); return v.empty() ? query_param(body, k) : v; };
        return api_select_preset(get("type"), get("name"), num(get("index"), -1));
    }
    if (path == "/presets/filament_color" && method == "POST") {
        auto get = [&](const char* k) { std::string v = query_param(query, k); return v.empty() ? query_param(body, k) : v; };
        return api_filament_color(num(get("index"), -1), get("color"));
    }
    if (path == "/presets/filament_add" && method == "POST")
        return api_filament_add();
    if (path == "/plates" && method == "GET")
        return api_plates();
    if (path.compare(0, 8, "/plates/") == 0 && method == "GET") {
        const std::string rest = path.substr(8);
        const size_t      slash = rest.find('/');
        if (slash != std::string::npos && rest.substr(slash) == "/thumbnail.png")
            return api_plate_thumbnail(num(rest.substr(0, slash), -1));
    }
    if (path == "/printers" && method == "GET")
        return api_printers();
    if (path == "/slice" && method == "POST") {
        std::string plate = query_param(query, "plate");
        if (plate.empty()) plate = query_param(body, "plate");
        return api_slice(num(plate, -1), plate == "all");
    }
    if (path == "/jobs" && method == "GET")
        return api_jobs(-1);
    if (path.compare(0, 6, "/jobs/") == 0 && method == "GET")
        return api_jobs(num(path.substr(6), -1));
    r.status = 404;
    r.body   = json_error("no such route; see /api");
    return r;
}

void RemoteAccess::accept_loop()
{
    auto* acceptor = static_cast<tcp::acceptor*>(m_acceptor);
    for (;;) {
        boost::system::error_code ec;
        auto* sock = new tcp::socket(acceptor->get_executor());
        acceptor->accept(*sock, ec);
        if (ec) {
            delete sock;
            break; // closed by stop()
        }
        std::thread([this, sock]() { serve(sock); }).detach();
    }
    delete acceptor;
}

// Loopback peers only (the hub); small bodies (form parameters); /api/... routes.
void RemoteAccess::serve(void* socket_ptr)
{
    std::unique_ptr<tcp::socket> owner(static_cast<tcp::socket*>(socket_ptr));
    tcp::socket&                 client = *owner;
    try {
        boost::system::error_code ec;
        const auto peer = client.remote_endpoint(ec).address();
        if (ec || !peer.is_loopback())
            return;
        client.set_option(tcp::no_delay(true));

        asio::streambuf req;
        asio::read_until(client, req, "\r\n\r\n");
        std::string head(asio::buffers_begin(req.data()), asio::buffers_end(req.data()));
        const size_t head_end = head.find("\r\n\r\n") + 4;
        std::string  body     = head.substr(head_end); // any body bytes already read
        head.resize(head_end);

        std::istringstream first(head.substr(0, head.find("\r\n")));
        std::string        method, target, version;
        first >> method >> target >> version;
        size_t content_length = 0;
        {
            size_t pos = head.find("\r\n") + 2;
            while (pos < head_end - 2) {
                size_t nl = head.find("\r\n", pos);
                std::string line = head.substr(pos, nl - pos);
                std::string key  = line.substr(0, std::min<size_t>(15, line.size()));
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char) std::tolower(c); });
                if (key == "content-length:")
                    content_length = (size_t) std::max(0, std::atoi(line.c_str() + 15));
                pos = nl + 2;
            }
        }
        const size_t q     = target.find('?');
        std::string  path  = q == std::string::npos ? target : target.substr(0, q);
        std::string  query = q == std::string::npos ? "" : target.substr(q + 1);

        if (path.compare(0, 4, "/api") != 0 || (path.size() > 4 && path[4] != '/')) {
            respond(client, "404 Not Found", "application/json", json_error("no such route; see /api"));
            return;
        }
        if (content_length > 64 * 1024) {
            respond(client, "413 Payload Too Large", "application/json", json_error("body too large"));
            return;
        }
        while (body.size() < content_length) {
            char   buf[4096];
            size_t n = client.read_some(asio::buffer(buf, std::min(sizeof(buf), content_length - body.size())));
            body.append(buf, n);
        }
        ApiResponse ar = handle_api(method, path.substr(4), query, body);
        const char* status = ar.status == 200 ? "200 OK" : ar.status == 400 ? "400 Bad Request" : ar.status == 404 ? "404 Not Found"
                           : ar.status == 409 ? "409 Conflict" : ar.status == 413 ? "413 Payload Too Large"
                           : ar.status == 503 ? "503 Service Unavailable" : "500 Internal Server Error";
        respond(client, status, ar.type, ar.body);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(debug) << "RemoteAccess: session ended: " << e.what();
    }
}

} // namespace GUI
} // namespace Slic3r
