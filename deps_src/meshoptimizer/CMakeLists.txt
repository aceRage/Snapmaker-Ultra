# Snapmaker Orca specific CMake.
#
# Only the DECODER half of meshoptimizer is vendored: enough to read
# EXT_meshopt_compression in Format/GLTF.cpp, and nothing else. Keeping it here in deps_src/
# rather than deps/ is the point - a new library in deps/ would force every developer and CI
# runner to rebuild the whole dependency tree.

cmake_minimum_required(VERSION 3.13)
project(meshoptimizer)

add_library(meshoptimizer STATIC
    meshoptimizer.h
    vertexcodec.cpp
    indexcodec.cpp
    vertexfilter.cpp
)

# Export the parent directory so the include reads <meshoptimizer/meshoptimizer.h>.
target_include_directories(meshoptimizer SYSTEM
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)
