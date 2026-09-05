# Find nghttp2, the HTTP/2 library the bundled libcurl is built against (see
# deps/NGHTTP2/NGHTTP2.cmake and the phase 0 spike, section 1.4).
#
# This module exists because of one naming detail. curl 7.75 ships its own CMake/FindNGHTTP2.cmake
# that looks only for a library called `nghttp2`, while nghttp2's own CMake installs its *static*
# target on MSVC as `nghttp2_static.lib`. Curl's module therefore finds nothing on Windows and the
# build silently falls back to no HTTP/2. Because the deps recipes pass this directory as
# CMAKE_MODULE_PATH to every sub-project, and curl only *appends* its own module directory, this
# file is found first and settles the naming for curl's configure and for the top-level build
# alike.
#
# Sets NGHTTP2_FOUND, NGHTTP2_INCLUDE_DIRS, NGHTTP2_LIBRARIES and the NGHTTP2::nghttp2 target.

find_path(NGHTTP2_INCLUDE_DIR
    NAMES nghttp2/nghttp2.h
)

find_library(NGHTTP2_LIBRARY
    # nghttp2_static first: on MSVC that is the static archive, and a static libcurl must link the
    # static half or every nghttp2 call becomes an import stub that does not exist.
    NAMES nghttp2_static nghttp2
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NGHTTP2
    REQUIRED_VARS NGHTTP2_LIBRARY NGHTTP2_INCLUDE_DIR
)

if (NGHTTP2_FOUND)
    set(NGHTTP2_INCLUDE_DIRS ${NGHTTP2_INCLUDE_DIR})
    set(NGHTTP2_LIBRARIES    ${NGHTTP2_LIBRARY})
    if (NOT TARGET NGHTTP2::nghttp2)
        add_library(NGHTTP2::nghttp2 UNKNOWN IMPORTED)
        set_target_properties(NGHTTP2::nghttp2 PROPERTIES
            IMPORTED_LOCATION             "${NGHTTP2_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${NGHTTP2_INCLUDE_DIR}"
            # nghttp2.h declares every symbol __declspec(dllimport) unless this is defined, so any
            # target that includes it - curl's http2.c, and anything we ever write against it -
            # must carry the macro or it links against import stubs that were never generated.
            INTERFACE_COMPILE_DEFINITIONS "NGHTTP2_STATICLIB"
        )
    endif ()
endif ()

mark_as_advanced(NGHTTP2_INCLUDE_DIR NGHTTP2_LIBRARY)
