cmake_minimum_required(VERSION 3.13)

# ImageMap FULL PR5: lut_wide.png.c is a ~36.3 MiB Git LFS payload (38094965 bytes).
# A clone without `git lfs pull` leaves a ~130-byte pointer. Fail configure clearly
# before project() / compile (no silent empty LUT, no cryptic pointer-as-C errors).
set(_IMAGEMAP_LUT "${CMAKE_CURRENT_SOURCE_DIR}/lut_wide.png.c")
if(NOT EXISTS "${_IMAGEMAP_LUT}")
    message(FATAL_ERROR
        "pigment-painter LUT is missing:\n"
        "  ${_IMAGEMAP_LUT}\n"
        "Run: git lfs install && git lfs pull\n"
        "See docs/imagemap-full-pr5.md and deps_src/pigment-painter/README.md")
endif()
file(READ "${_IMAGEMAP_LUT}" _IMAGEMAP_LUT_HEAD LIMIT 80)
if(_IMAGEMAP_LUT_HEAD MATCHES "git-lfs")
    message(FATAL_ERROR
        "pigment-painter LUT is still a Git LFS pointer, not the 38094965-byte payload.\n"
        "  ${_IMAGEMAP_LUT}\n"
        "Run: git lfs install && git lfs pull\n"
        "Then re-run cmake. See docs/imagemap-full-pr5.md")
endif()
unset(_IMAGEMAP_LUT_HEAD)
unset(_IMAGEMAP_LUT)

project(pigment_painter)

find_package(PNG REQUIRED)

add_library(pigment_painter STATIC
    lut_wide.png.c
    pigment_painter_mixer.cpp
    pigment_painter_mixer.hpp
)

target_include_directories(pigment_painter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pigment_painter PUBLIC cxx_std_17)
target_link_libraries(pigment_painter PRIVATE PNG::PNG)
