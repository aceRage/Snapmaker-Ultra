# Ultra: QuadriFlow - field-aligned quad remeshing (hjwdzh/QuadriFlow, BSD-3-Clause).
#
# Backs the "Quad remesh..." action: a triangle soup in, an all-quad mesh out at a
# target face count. Phase 2 of docs/superpowers/specs/2026-09-11-remesh-controls-
# research.md; the integration write-up is 2026-09-12-quad-remesh-phase2.md.
#
# Pinned to 810b7a0967c35b0dc85b4464e3835e26a756c967 (the repo's HEAD; upstream has
# cut no releases and has been quiet for years, so a commit is the only pin available).
#
# LICENCES: QuadriFlow itself is BSD-3-Clause; the bundled Lemon is under the Boost
# Software License 1.0. Both are permissive and compatible with this tree's AGPL-3.0.
# BUILD_FREE_LICENSE=ON is passed explicitly - it adds -DEIGEN_MPL2_ONLY, which makes
# the build FAIL if anything reaches for Eigen's LGPL-licensed sparse-solver subset.
# Note the Phase 1 research spec's claim that this option "swaps in an MPL2 sparse-LU
# solver" is not what upstream's CMakeLists does; verified against the pinned tree.
# The bundled MapleCOMSPS SAT solver and the Ceres-dependent post-solver.cpp are NOT
# compiled - see the licence audit at the top of CMakeLists.txt.in.
#
# Upstream ships no library target and no install rules (it builds a CLI executable
# only), so the PATCH_COMMAND swaps in our own CMakeLists, the way deps/OpenCSG does.
#
# EIGEN3_INCLUDE_DIR points at the tree's own bundled Eigen (deps_src/eigen), the same
# copy libslic3r compiles against - QuadriFlow's types cross the ABI boundary in our
# wrapper, so the two must agree on Eigen.

Snapmaker_Orca_add_cmake_project(QuadriFlow
    URL https://github.com/hjwdzh/QuadriFlow/archive/810b7a0967c35b0dc85b4464e3835e26a756c967.zip
        URL_HASH SHA256=0e530a1374dd7edd68d8bd9777395dc0be80e4cbca96485cc9a82cfacb8942ce
    DEPENDS dep_Boost
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt.in ./CMakeLists.txt
    CMAKE_ARGS
        -DBUILD_FREE_LICENSE:BOOL=ON
        -DEIGEN3_INCLUDE_DIR:PATH=${PROJECT_SOURCE_DIR}/../deps_src/eigen
)

if (MSVC)
    add_debug_dep(dep_QuadriFlow)
endif ()
