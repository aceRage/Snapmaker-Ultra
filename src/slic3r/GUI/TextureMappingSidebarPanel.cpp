#include "TextureMappingSidebarPanel.hpp"

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "TextureMappingPlaterHooks.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StaticLine.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TextureMapping.hpp"

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {

namespace {

std::vector<std::string> current_filament_colours()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return {};
    const ConfigOptionStrings *opt = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    return opt != nullptr ? opt->values : std::vector<std::string>();
}

size_t current_physical_count()
{
    return size_t(std::max(wxGetApp().filaments_cnt(), 0));
}

const char *pattern_label(int pattern)
{
    switch (pattern) {
    case int(TextureMappingZone::ImageTexture): return "Image";
    case int(TextureMappingZone::Gradient2D):   return "Color region";
    case int(TextureMappingZone::LinearGradient): return "Linear gradient";
    default: return "Zone";
    }
}

} // namespace

void persist_texture_mapping_sidebar_state()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;
    TextureMappingPlaterHooks::store_texture_mapping_definitions(*bundle);
    TextureMappingPlaterHooks::sync_current_model_texture_mapping_definitions(
        bundle->texture_mapping_zones.serialize_entries());
    TextureMappingPlaterHooks::invalidate_texture_mapping_display_color_cache();
}

void Sidebar::init_texture_mapping_panel(wxWindow *parent, wxBoxSizer *scrolled_sizer)
{
    if (m_tm_state)
        return;
    m_tm_state = std::make_unique<TextureMappingSidebarState>();

    auto *title = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    title->SetBackgroundColour(wxColour(0xF1, 0xF1, 0xF1));
    m_tm_state->title = title;

    auto *title_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *label = new wxStaticText(title, wxID_ANY, _L("Texture Mapping"));
    title_sizer->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    m_tm_state->add_image = new Button(title, _L("Add Image"));
    m_tm_state->add_color = new Button(title, _L("Add Color Region"));
    m_tm_state->add_gradient = new Button(title, _L("Add Gradient"));
    title_sizer->Add(m_tm_state->add_image, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    title_sizer->Add(m_tm_state->add_color, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    title_sizer->Add(m_tm_state->add_gradient, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    title->SetSizer(title_sizer);

    auto add_zone = [this](int pattern) {
        PresetBundle *bundle = wxGetApp().preset_bundle;
        if (bundle == nullptr)
            return;
        const auto colours = current_filament_colours();
        bundle->texture_mapping_zones.add_zone(current_physical_count(), colours, pattern);
        persist_texture_mapping_sidebar_state();
        update_texture_mapping_panel(false);
        if (wxGetApp().plater() != nullptr)
            wxGetApp().plater()->schedule_background_process();
    };
    m_tm_state->add_image->Bind(wxEVT_BUTTON, [add_zone](wxCommandEvent &) { add_zone(int(TextureMappingZone::ImageTexture)); });
    m_tm_state->add_color->Bind(wxEVT_BUTTON, [add_zone](wxCommandEvent &) { add_zone(int(TextureMappingZone::Gradient2D)); });
    m_tm_state->add_gradient->Bind(wxEVT_BUTTON, [add_zone](wxCommandEvent &) { add_zone(int(TextureMappingZone::LinearGradient)); });

    auto *spliter1 = new ::StaticLine(parent);
    spliter1->SetLineColour("#A6A9AA");
    scrolled_sizer->Add(spliter1, 0, wxEXPAND);
    scrolled_sizer->Add(title, 0, wxEXPAND | wxALL, 0);
    auto *spliter2 = new ::StaticLine(parent);
    spliter2->SetLineColour("#CECECE");
    scrolled_sizer->Add(spliter2, 0, wxEXPAND);

    m_tm_state->content = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_tm_state->content_sizer = new wxBoxSizer(wxVERTICAL);
    m_tm_state->content->SetSizer(m_tm_state->content_sizer);
    scrolled_sizer->Add(m_tm_state->content, 0, wxEXPAND, 0);

    title->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        if (m_tm_state->content->GetMaxHeight() == 0)
            m_tm_state->content->SetMaxSize({-1, -1});
        else
            m_tm_state->content->SetMaxSize({-1, 0});
        m_tm_state->collapsed = m_tm_state->content->GetMaxHeight() == 0;
        m_tm_state->content->GetParent()->Layout();
    });

    update_texture_mapping_panel(true);
}

void Sidebar::update_texture_mapping_panel(bool sync_manager)
{
    if (!m_tm_state || !m_tm_state->content_sizer)
        return;

    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;

    if (sync_manager) {
        const std::string serialized = TextureMappingPlaterHooks::texture_mapping_config_string(
            bundle->project_config, &bundle->prints.get_edited_preset().config, "texture_mapping_definitions");
        if (!serialized.empty())
            TextureMappingPlaterHooks::load_texture_mapping_definitions(*bundle, serialized);
    }

    TextureMappingManager &mgr = bundle->texture_mapping_zones;
    const auto colours = current_filament_colours();
    const size_t num_physical = current_physical_count();
    mgr.refresh(colours);

    m_tm_state->content_sizer->Clear(true);

    int visible_count = 0;
    for (size_t idx = 0; idx < mgr.zones().size(); ++idx) {
        TextureMappingZone &zone = mgr.zones()[idx];
        if (zone.deleted)
            continue;
        ++visible_count;

        auto *row = new wxPanel(m_tm_state->content);
        auto *row_sizer = new wxBoxSizer(wxVERTICAL);
        auto *head = new wxBoxSizer(wxHORIZONTAL);

        auto *enable = new wxCheckBox(row, wxID_ANY, "");
        enable->SetValue(zone.enabled);
        enable->Bind(wxEVT_CHECKBOX, [idx](wxCommandEvent &evt) {
            PresetBundle *b = wxGetApp().preset_bundle;
            if (b == nullptr || idx >= b->texture_mapping_zones.zones().size())
                return;
            b->texture_mapping_zones.zones()[idx].enabled = evt.IsChecked();
            persist_texture_mapping_sidebar_state();
            if (wxGetApp().plater() != nullptr)
                wxGetApp().plater()->schedule_background_process();
        });

        wxString title = wxString::Format("%s  ID %u", pattern_label(zone.surface_pattern), zone.zone_id);
        auto *name = new wxStaticText(row, wxID_ANY, title);
        head->Add(enable, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        head->Add(name, 1, wxALIGN_CENTER_VERTICAL);

        auto *remove = new wxButton(row, wxID_ANY, _L("Remove"));
        remove->Bind(wxEVT_BUTTON, [this, idx](wxCommandEvent &) {
            PresetBundle *b = wxGetApp().preset_bundle;
            if (b == nullptr || idx >= b->texture_mapping_zones.zones().size())
                return;
            b->texture_mapping_zones.zones()[idx].deleted = true;
            persist_texture_mapping_sidebar_state();
            update_texture_mapping_panel(false);
            if (wxGetApp().plater() != nullptr)
                wxGetApp().plater()->schedule_background_process();
        });
        head->Add(remove, 0, wxALIGN_CENTER_VERTICAL);
        row_sizer->Add(head, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

        auto *comp = new wxBoxSizer(wxHORIZONTAL);
        comp->Add(new wxStaticText(row, wxID_ANY, _L("Filaments")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

        auto add_component_choice = [&](unsigned int &component, const wxString &label) {
            auto *choice = new wxChoice(row, wxID_ANY);
            for (size_t filament = 1; filament <= num_physical; ++filament)
                choice->Append(wxString::Format("%s %zu", label, filament));
            int sel = int(component) - 1;
            if (sel < 0 || size_t(sel) >= num_physical)
                sel = 0;
            if (choice->GetCount() > 0)
                choice->SetSelection(sel);
            choice->Bind(wxEVT_CHOICE, [&component](wxCommandEvent &evt) {
                component = unsigned(evt.GetSelection() + 1);
                persist_texture_mapping_sidebar_state();
                if (wxGetApp().plater() != nullptr)
                    wxGetApp().plater()->schedule_background_process();
            });
            comp->Add(choice, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        };
        add_component_choice(zone.component_a, _L("A"));
        add_component_choice(zone.component_b, _L("B"));
        row_sizer->Add(comp, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        row->SetSizer(row_sizer);
        m_tm_state->content_sizer->Add(row, 0, wxEXPAND | wxTOP, FromDIP(4));
    }

    if (visible_count == 0) {
        auto *empty = new wxStaticText(m_tm_state->content, wxID_ANY,
                                       _L("No texture-mapping zones. Add an image, color region, or gradient."));
        empty->Wrap(FromDIP(240));
        m_tm_state->content_sizer->Add(empty, 0, wxEXPAND | wxALL, FromDIP(8));
    }

    m_tm_state->content->Layout();
    if (auto *parent = m_tm_state->content->GetParent())
        parent->Layout();
}

} // namespace GUI
} // namespace Slic3r
