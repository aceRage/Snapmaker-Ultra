cmake_minimum_required(VERSION 3.13)
project(pigment_painter)

find_package(PNG REQUIRED)

# lut_wide.png.c is ~36 MiB and is NOT in git. Fetch from ImageMap at configure
# time (see README.md and scripts/download_pigment_lut.cmake).
set(IMAGEMAP_LUT_COMMIT "92548381056dbf72836b0a1bdc455f238218dbfb")
set(IMAGEMAP_LUT_URL "https://raw.githubusercontent.com/sentientstardust-dev/OrcaSlicer-ImageMap/${IMAGEMAP_LUT_COMMIT}/deps_src/pigment-painter/lut_wide.png.c")

if(DEFINED ULTRA_PIGMENT_LUT_FILE AND EXISTS "${ULTRA_PIGMENT_LUT_FILE}")
    set(PIGMENT_LUT_C "${ULTRA_PIGMENT_LUT_FILE}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lut_wide.png.c")
    set(PIGMENT_LUT_C "${CMAKE_CURRENT_SOURCE_DIR}/lut_wide.png.c")
else()
    set(PIGMENT_LUT_C "${CMAKE_CURRENT_BINARY_DIR}/lut_wide.png.c")
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            "-DLUT_URL=${IMAGEMAP_LUT_URL}"
            "-DLUT_OUTPUT=${PIGMENT_LUT_C}"
            -P "${CMAKE_SOURCE_DIR}/scripts/download_pigment_lut.cmake"
        RESULT_VARIABLE _lut_download_result
    )
    if(NOT _lut_download_result EQUAL 0)
        message(FATAL_ERROR "pigment-painter LUT download failed (see scripts/download_pigment_lut.cmake)")
    endif()
endif()

add_library(pigment_painter STATIC
    ${PIGMENT_LUT_C}
    pigment_painter_mixer.cpp
    pigment_painter_mixer.hpp
)

target_include_directories(pigment_painter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pigment_painter PUBLIC cxx_std_17)
target_link_libraries(pigment_painter PRIVATE PNG::PNG)
