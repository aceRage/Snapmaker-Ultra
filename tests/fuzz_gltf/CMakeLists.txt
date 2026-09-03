add_executable(fuzz_gltf fuzz_gltf.cpp nanosvg_impl.cpp)
# The same link set as tests/libslic3r: libslic3r pulls MD5 from OpenSSL and BCryptGenRandom
# from bcrypt, and nanosvg is header-only so the executable defines the implementation itself.
target_link_libraries(fuzz_gltf libslic3r nanosvg OpenSSL::Crypto)
if (WIN32)
    target_link_libraries(fuzz_gltf bcrypt.lib)
endif()
set_property(TARGET fuzz_gltf PROPERTY FOLDER "tests")

if (WIN32)
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        Snapmaker_Orca_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
    elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
        Snapmaker_Orca_copy_dlls(COPY_DLLS "RelWithDebInfo" "" output_dlls_Release)
    else()
        Snapmaker_Orca_copy_dlls(COPY_DLLS "Release" "" output_dlls_Release)
    endif()
endif()
