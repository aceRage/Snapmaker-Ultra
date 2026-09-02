#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

// This slicer instance's JSON API, on a loopback-only port picked by the OS.
//
// Phones and agents never talk to it directly: the hub process (RemoteHub.hpp) owns the
// token-gated LAN listener and proxies /r/<token>/i/<pid>/api/... here as /api/... .
// The instance announces itself by dropping <datadir>/hub/instances/<pid>.json (pid,
// port, start time); the hub lists those, probes GET /api/info and removes dead ones.
//
// Requests run on the GUI thread (Plater, plates, presets and devices are GUI-thread
// only); confirmation dialogs raised meanwhile answer Yes on their own (auto_confirm).
class RemoteAccess
{
public:
    static RemoteAccess& get();

    // Idempotent; call from the main thread once the Plater exists.
    void start();
    void stop(); // removes the instance file; the listener dies with the process
    int  port();

    // What the hub shows in its instance list (GUI thread, from the Plater's title code).
    void note_project(const std::string& title, const std::string& path);
    // An error the GUI wanted to show while a request ran (show_error); the request reports it.
    void        note_error(const std::string& message);
    std::string take_error();

    // True while a phone/agent request is being executed on the GUI thread. Confirmation
    // dialogs raised meanwhile (e.g. "adjust these settings automatically?") answer Yes on
    // their own instead of blocking a GUI nobody is looking at.
    static bool auto_confirm();
    struct AutoConfirmScope
    {
        AutoConfirmScope();
        ~AutoConfirmScope();
    };

    // Called by the Plater's slicing handlers (GUI thread) so that /api/jobs can report
    // progress of a slice started through the API.
    void note_slice_progress(int plate, int percent, const std::string& text);
    void note_slice_done(bool finished_all, bool ok, const std::string& error);

private:
    RemoteAccess() = default;
    void accept_loop();
    void serve(void* socket); // boost::asio tcp::socket*, owned by the call
    void write_instance_file();

    struct ApiResponse
    {
        int         status { 200 };
        std::string type { "application/json" };
        std::string body;
        std::string headers; // extra "Name: value\r\n" lines
    };
    ApiResponse handle_api(const std::string& method, const std::string& path, const std::string& query, const std::string& body);
    ApiResponse api_info();
    ApiResponse api_project_open(const std::string& path, const std::string& mode);
    ApiResponse api_plates();
    ApiResponse api_plate_thumbnail(int plate);
    ApiResponse api_plate_layout(int plate);
    ApiResponse api_plate_preview(int plate);
    ApiResponse api_plate_preview_png(int plate, const std::string& view, int layer, int w, int h, double zoom, double cx, double cy);
    ApiResponse api_plate_preview_status(int plate);
    ApiResponse api_object_transform(const std::string& form_body);
    ApiResponse api_printers();
    ApiResponse api_slice(int plate, bool all);
    ApiResponse api_jobs(int id);
    ApiResponse api_presets();
    ApiResponse api_select_preset(const std::string& type, const std::string& name, int index);
    ApiResponse api_filament_color(int index, const std::string& color);
    ApiResponse api_filament_add();
    ApiResponse api_process_settings();
    ApiResponse api_process_set(const std::string& form_body);
    ApiResponse api_process_revert();
    ApiResponse api_process_save();

    struct Job
    {
        int         id { 0 };
        int         plate { -1 }; // -1 = all plates
        std::string state;        // running | done | error | cancelled
        int         percent { 0 };
        std::string text, error;
    };

    std::mutex       m_mutex;
    bool             m_on { false };
    int              m_port { 0 };
    void*            m_acceptor { nullptr };
    std::vector<Job> m_jobs;
    int              m_next_job { 1 };
    std::string      m_title, m_path, m_last_error;
    bool             m_slicing { false };
};

} // namespace GUI
} // namespace Slic3r
