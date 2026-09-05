# nghttp2, built for one reason: APNs speaks HTTP/2 and nothing else.
#
# Apple's push service negotiates ALPN "h2" and offers nothing when only http/1.1 is proposed, so
# a libcurl without HTTP/2 cannot talk to it at all - `CURLOPT_HTTP_VERSION` does not even fall
# back, it returns CURLE_UNSUPPORTED_PROTOCOL. curl 7.75's CMake makes HTTP/2 synonymous with
# nghttp2 (there is no other provider), hence this dependency. See
# docs/superpowers/specs/2026-09-04-ultra1-phase0-spike.md section 1.
#
# ENABLE_LIB_ONLY is the flag that matters. nghttp2's *applications* are C++ and drag in libev,
# libc-ares, OpenSSL and more; its *library* is plain C with no dependencies at all, and the
# library is the whole of what curl links. Everything else here is off for the same reason.
#
# 1.64.0 rather than the newest release: it is comfortably above curl's hard floor of 1.12
# (lib/http2.c refuses anything older) and does not raise the CMake version this project needs.

Snapmaker_Orca_add_cmake_project(NGHTTP2
  URL                 https://github.com/nghttp2/nghttp2/archive/refs/tags/v1.64.0.zip
  URL_HASH            SHA256=91e9473771928a0eb03f62795d22e18d001a7c7671c6d4e6fe2feb6941d123e8
  CMAKE_ARGS
    -DENABLE_LIB_ONLY:BOOL=ON
    # BUILD_STATIC_LIBS, not the ENABLE_STATIC_LIB of older releases: since 1.6x nghttp2 picks its
    # library targets from the standard CMake switches, and with both off it builds no library at
    # all and then fails on its own nghttp2::nghttp2 alias. BUILD_SHARED_LIBS is already OFF for
    # every dependency this project builds.
    -DBUILD_STATIC_LIBS:BOOL=ON
    -DENABLE_DOC:BOOL=OFF
    -DENABLE_APP:BOOL=OFF
    -DENABLE_EXAMPLES:BOOL=OFF
    -DENABLE_HPACK_TOOLS:BOOL=OFF
    -DBUILD_TESTING:BOOL=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

if (MSVC)
    add_debug_dep(dep_NGHTTP2)
endif ()
