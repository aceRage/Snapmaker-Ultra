# Ultra (support groups): headless front end for the Slice Compare engine, used by
# scripts/support_group_identity.py as the Stage 2+ tolerance gate. Test-only on purpose - it is
# never installed, so the shipped application gains no surface from it.
add_executable(slice_compare_cli slice_compare_cli.cpp)
target_link_libraries(slice_compare_cli libslic3r nanosvg OpenSSL::Crypto)
if (WIN32)
    target_link_libraries(slice_compare_cli bcrypt.lib)
endif()
set_property(TARGET slice_compare_cli PROPERTY FOLDER "tests")

# The same DLL staging the other test targets do: this executable links libslic3r, which pulls
# in OpenCASCADE (TKernel.dll and friends) at run time.
if (WIN32)
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        Snapmaker_Orca_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
    elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
        Snapmaker_Orca_copy_dlls(COPY_DLLS "RelWithDebInfo" "" output_dlls_Release)
    else()
        Snapmaker_Orca_copy_dlls(COPY_DLLS "Release" "" output_dlls_Release)
    endif()
endif()

