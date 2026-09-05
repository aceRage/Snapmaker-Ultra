#ifndef _common_func_hppp_
#define _common_func_hppp_
#include <iostream>


// The product name lives here and nowhere else. version.inc reads these two lines back
// out of this header for CMake, and libslic3r.h derives its own names from them, so the
// three definitions that used to drift apart are now one.
#define SLIC3R_APP_NAME "UltraOne"
#define SLIC3R_APP_KEY "UltraOne"
// The data directory we used to live in, before the rename. Read from here by the
// first-start migration (DataDirMigration.cpp) and by nothing else, so a later release
// can drop the migration by deleting one constant and one call.
#define SLIC3R_LEGACY_APP_KEY "Snapmaker_Orca"
#define SLIC3R_VERSION "01.10.01.50"
#define Snapmaker_VERSION "2.3.6"
#define MIN_FIRM_VER "1.5.0"
#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "0000000" // 0000000 means uninitialized
#endif
#define SLIC3R_BUILD_ID "2.3.6"
// #define SLIC3R_RC_VERSION "01.10.01.50"
#define BBL_RELEASE_TO_PUBLIC 1
#define BBL_INTERNAL_TESTING 0
#define ORCA_CHECK_GCODE_PLACEHOLDERS 0

namespace common 
{
	std::string get_pc_name();

	std::string get_flutter_version();

	std::string get_profile_version();

	std::string getMachineId();

	std::string getLocalArea();

	std::string getLanguage();

    } // namespace common

#endif