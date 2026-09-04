# Download ImageMap pigment-painter LUT (lut_wide.png.c, ~36 MiB).
# Invoked by deps_src/pigment-painter/CMakeLists.txt at configure time when the
# generated C array is not already present. Do NOT commit lut_wide.png.c.
#
# Source: OrcaSlicer-ImageMap @ 92548381056dbf72836b0a1bdc455f238218dbfb
#   https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap
#
# CI: cmake configure needs outbound HTTPS to raw.githubusercontent.com (or
# github.com). Pre-seed LUT_OUTPUT to skip the download, e.g.
#   cmake -DULTRA_PIGMENT_LUT_FILE=/path/to/lut_wide.png.c ...

if(NOT DEFINED LUT_URL OR LUT_URL STREQUAL "")
    message(FATAL_ERROR "download_pigment_lut.cmake requires LUT_URL")
endif()
if(NOT DEFINED LUT_OUTPUT OR LUT_OUTPUT STREQUAL "")
    message(FATAL_ERROR "download_pigment_lut.cmake requires LUT_OUTPUT")
endif()

if(EXISTS "${LUT_OUTPUT}")
    file(SIZE "${LUT_OUTPUT}" _lut_size)
    if(_lut_size GREATER 30000000)
        return()
    endif()
    message(STATUS "Existing pigment LUT at ${LUT_OUTPUT} is too small (${_lut_size} bytes); re-downloading")
    file(REMOVE "${LUT_OUTPUT}")
endif()

message(STATUS "Downloading pigment-painter LUT (~36 MiB) from ImageMap...")
message(STATUS "  URL: ${LUT_URL}")
message(STATUS "  dest: ${LUT_OUTPUT}")

file(DOWNLOAD
    "${LUT_URL}"
    "${LUT_OUTPUT}"
    TIMEOUT 300
    SHOW_PROGRESS
    STATUS _dl_status
    LOG _dl_log
)

list(GET _dl_status 0 _dl_code)
list(GET _dl_status 1 _dl_msg)
if(NOT _dl_code EQUAL 0)
    file(REMOVE "${LUT_OUTPUT}")
    message(FATAL_ERROR
        "Failed to download pigment-painter LUT (${_dl_msg}).\n"
        "CI/local builds must be able to fetch:\n  ${LUT_URL}\n"
        "Place lut_wide.png.c at the destination and re-run cmake, or pass -DULTRA_PIGMENT_LUT_FILE=...")
endif()

file(SIZE "${LUT_OUTPUT}" _lut_size)
if(_lut_size LESS 30000000)
    file(REMOVE "${LUT_OUTPUT}")
    message(FATAL_ERROR
        "Downloaded pigment LUT is unexpectedly small (${_lut_size} bytes). Expected ~36 MiB.\n"
        "Download log:\n${_dl_log}")
endif()

message(STATUS "Pigment-painter LUT downloaded (${_lut_size} bytes)")
