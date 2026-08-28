#ifndef slic3r_Spoolman_hpp_
#define slic3r_Spoolman_hpp_

#include <string>
#include <vector>

namespace Slic3r {

// Ultra: minimal client for a self-hosted Spoolman server (https://github.com/Donkie/Spoolman).
// Synchronous helpers; call off the UI thread or accept the short block (LAN server).
struct SpoolmanSpool
{
    int         id { 0 };
    std::string vendor;
    std::string name;          // filament name
    std::string material;      // e.g. PLA
    std::string color_hex;     // RRGGBB
    std::string location;
    double      remaining_weight { 0. }; // grams; < 0 when unknown
    double      diameter { 1.75 };
    double      density { 1.24 };
    int         ext_temp { 0 };  // settings_extruder_temp, 0 = unset
    int         bed_temp { 0 };  // settings_bed_temp, 0 = unset
    bool        archived { false };
};

class Spoolman
{
public:
    // Base URL from app config ("spoolman_url"), normalized without trailing slash;
    // empty when the integration is disabled or unconfigured.
    static std::string base_url();
    static bool        enabled();

    // GET /api/v1/spool. Returns false on any transport/parse error (msg filled).
    static bool get_spools(std::vector<SpoolmanSpool>& spools, std::string& error);

    // PUT /api/v1/spool/{id}/use with use_weight grams. Returns false on error.
    static bool use_weight(int spool_id, double grams, std::string& error);
};

} // namespace Slic3r

#endif // slic3r_Spoolman_hpp_
