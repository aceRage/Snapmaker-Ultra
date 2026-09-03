# Ultra (support groups): headless front end for the Slice Compare engine, used by
# scripts/support_group_identity.py as the Stage 2+ tolerance gate. Test-only on purpose - it is
# never installed, so the shipped application gains no surface from it.
add_executable(slice_compare_cli slice_compare_cli.cpp)
target_link_libraries(slice_compare_cli libslic3r nanosvg OpenSSL::Crypto)
if (WIN32)
    target_link_libraries(slice_compare_cli bcrypt.lib)
endif()
set_property(TARGET slice_compare_cli PROPERTY FOLDER "tests")
