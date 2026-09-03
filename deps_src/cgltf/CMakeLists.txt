cmake_minimum_required(VERSION 3.13)

project(cgltf)

add_library(cgltf INTERFACE)

# Export the parent directory so the include reads <cgltf/cgltf.h> and cannot collide
# with anything else on the include path (same shape as deps_src/nlohmann).
target_include_directories(cgltf SYSTEM
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_sources(cgltf INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/cgltf.h
)

# cgltf.h is C99; the single implementation TU is compiled as C++ inside libslic3r
# (src/libslic3r/Format/GLTF.cpp defines CGLTF_IMPLEMENTATION).
target_compile_features(cgltf INTERFACE cxx_std_14)
