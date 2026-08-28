#ifndef slic3r_SpoolmanDialog_hpp_
#define slic3r_SpoolmanDialog_hpp_

#include <wx/dialog.h>
#include <vector>

#include "slic3r/Utils/Spoolman.hpp"

class wxDataViewListCtrl;
class wxStaticText;

namespace Slic3r {
namespace GUI {

// Ultra: "Spool Manager" - inventory view of a self-hosted Spoolman server with
// per-slot spool bindings and filament-usage deduction (design per approved mockup).
class SpoolmanDialog : public wxDialog
{
public:
    SpoolmanDialog(wxWindow* parent);

private:
    void refresh_spools();
    void on_use_in_slot(int row);
    void deduct_last_slice();
    void update_status(const wxString& text, bool error = false);

    wxDataViewListCtrl*        m_list { nullptr };
    wxStaticText*              m_status { nullptr };
    std::vector<SpoolmanSpool> m_spools;

public:
    // slot (0-based) -> spool id bindings, persisted in app config section "spoolman_bindings"
    static int  bound_spool_for_slot(int slot);
    static void bind_slot(int slot, int spool_id);

    // Deduct the current plate's sliced filament usage from bound spools.
    // Returns a human-readable summary (empty when nothing was deducted).
    static std::string deduct_current_plate_usage();

    // Fire-and-forget deduction after a job was sent to the printer; respects the
    // "spoolman_deduct" preference and reports via the plater notification area.
    static void deduct_after_send_async();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_SpoolmanDialog_hpp_
