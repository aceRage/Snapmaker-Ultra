# Ultra: Manifold - robust, guaranteed-manifold mesh booleans (Apache-2.0).
# Used as the primary backend of the Mesh Boolean gizmo, with MCUT as fallback.
Snapmaker_Orca_add_cmake_project(Manifold
  URL "https://github.com/elalish/manifold/releases/download/v3.5.2/manifold-3.5.2.tar.gz"
  URL_HASH SHA256=ce5f4d87877daca99a910d0af73c028f2a36b9288dd2bf3cd1cecf8faff9f7c8
  CMAKE_ARGS
    -DMANIFOLD_TEST:BOOL=OFF
    -DMANIFOLD_PAR:BOOL=OFF
    -DMANIFOLD_CROSS_SECTION:BOOL=OFF
    -DMANIFOLD_EXPORT:BOOL=OFF
    -DMANIFOLD_DEBUG:BOOL=OFF
    -DBUILD_SHARED_LIBS:BOOL=OFF
)

if (MSVC)
    add_debug_dep(dep_Manifold)
endif ()
