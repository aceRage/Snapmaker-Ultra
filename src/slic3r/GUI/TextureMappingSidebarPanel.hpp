#ifndef slic3r_TextureMappingSidebarPanel_hpp_
#define slic3r_TextureMappingSidebarPanel_hpp_

#include <wx/panel.h>

#include <memory>
#include <vector>

class wxBoxSizer;
class wxButton;
class wxCheckBox;
class wxChoice;
class wxStaticText;
class wxWindow;

namespace Slic3r {
namespace GUI {

class Sidebar;

// Compact Texture Mapping sidebar surface (not a wholesale ImageMap Plater dump).
struct TextureMappingSidebarState
{
    wxPanel      *title{nullptr};
    wxPanel      *content{nullptr};
    wxBoxSizer   *content_sizer{nullptr};
    wxButton     *add_image{nullptr};
    wxButton     *add_color{nullptr};
    wxButton     *add_gradient{nullptr};
    bool          collapsed{false};
};

void persist_texture_mapping_sidebar_state();

} // namespace GUI
} // namespace Slic3r

#endif
