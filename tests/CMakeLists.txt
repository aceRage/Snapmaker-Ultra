# TODO Add individual tests as executables in separate directories
# add_subirectory(<testcase>)

set(TEST_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
file(TO_NATIVE_PATH "${TEST_DATA_DIR}" TEST_DATA_DIR)

add_library(Catch2 INTERFACE)
list (APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules/Catch2)
target_include_directories(Catch2 INTERFACE ${CMAKE_CURRENT_LIST_DIR})
add_library(Catch2::Catch2 ALIAS Catch2)
if (APPLE)
	# OSX builds targeting OSX 10.9 do not support new std::uncought_exception()
	# see https://github.com/catchorg/Catch2/issues/1218
	target_compile_definitions(Catch2 INTERFACE -DCATCH_CONFIG_NO_CPP17_UNCAUGHT_EXCEPTIONS)
endif()
include(Catch)

set(CATCH_EXTRA_ARGS "" CACHE STRING "Extra arguments for catch2 test suites.")

add_library(test_common INTERFACE)
target_compile_definitions(test_common INTERFACE TEST_DATA_DIR=R"\(${TEST_DATA_DIR}\)" CATCH_CONFIG_FAST_COMPILE)
target_link_libraries(test_common INTERFACE Catch2::Catch2)

if (APPLE)
    target_link_libraries(test_common INTERFACE "-liconv -framework IOKit" "-framework CoreFoundation" -lc++)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

add_subdirectory(libnest2d)
add_subdirectory(libslic3r)
add_subdirectory(slic3rutils)
add_subdirectory(fff_print)
add_subdirectory(slice_compare_cli)
# add_subdirectory(sla_print)
add_subdirectory(cpp17 EXCLUDE_FROM_ALL)    # does not have to be built all the time
add_subdirectory(fuzz_gltf EXCLUDE_FROM_ALL) # glTF reader fuzz driver, built on demand
# add_subdirectory(example)

# Profile sanity check: every bed model/texture, cover image and "inherits" parent a
# vendor profile names has to resolve to a file that exists. These all fail silently at
# runtime (the load sites gate on exists() or swallow the exception), so without this the
# only symptom is a degraded bed or an empty printer-picker card. GitHub Actions runs the
# same script via check_profiles.yml; this makes `ctest -R profile` catch it locally too.
# Needs no compilation, so it is registered here rather than in a C++ test target.
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    add_test(NAME profile_assets
             COMMAND ${Python3_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/scripts/orca_extra_profile_check.py
                     --check-assets)
    set_tests_properties(profile_assets PROPERTIES LABELS "profile")

    # Every filament a machine names in default_materials / default_filament_profile has to
    # resolve to a preset that exists, in the vendor's own library or in OrcaFilamentLibrary.
    # A name that does not silently disappears from the material picker at runtime. Kept as a
    # test of its own so the two failure modes stay distinguishable; --no-check-assets because
    # --check-assets defaults to on and profile_assets already covers it.
    add_test(NAME profile_materials
             COMMAND ${Python3_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/scripts/orca_extra_profile_check.py
                     --check-materials --no-check-assets)
    set_tests_properties(profile_materials PROPERTIES LABELS "profile")

    # No two vendors may ship the same user-selectable preset display name, and no two
    # presets may claim the same "renamed_from" old name. Both are errors for upstream's
    # profile validator (the binary check_profiles.yml downloads), and both are invisible
    # here at runtime: merge_presets special-cases the Snapmaker bundle so a clashing
    # Snapmaker preset survives next to BBL's, but which one find_preset then returns
    # depends on load order, i.e. on the filesystem. This is the regression guard for the
    # Snapmaker generics rename (Generic X -> Generic X @U1 0.4 nozzle).
    add_test(NAME profile_names
             COMMAND ${Python3_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/scripts/check_preset_name_clashes.py
                     -p ${PROJECT_SOURCE_DIR}/resources/profiles)
    set_tests_properties(profile_names PROPERTIES LABELS "profile")
else()
    message(STATUS "Python3 interpreter not found - skipping the profile tests")
endif()
