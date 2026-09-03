// Fuzz driver for the glTF/GLB reader.
//
// The reader sits behind the phone upload endpoint (RemoteHub::spool_upload), which writes a file
// whose bytes an attacker chose. This target exists so that surface can be exercised with mutated
// input: it hands each path on the command line to load_gltf and reports anything that is not a
// clean true/false - an uncaught exception, or a crash the OS reports through the exit code.
//
// Not built by default (tests/CMakeLists.txt adds it EXCLUDE_FROM_ALL). Build and run with:
//
//   cmake --build build --config Release --target fuzz_gltf
//   python tests/fuzz_gltf/mutate.py --minutes 10
//
// It is deliberately a plain main() rather than an LLVMFuzzerTestOneInput: load_gltf takes a path,
// so a libFuzzer harness would have to write a temp file per case anyway, and this shape also
// drives AFL and the plain corpus runner in mutate.py.

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "libslic3r/Format/GLTF.hpp"
#include "libslic3r/Model.hpp"

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: fuzz_gltf <file.glb|file.gltf> [more files...]\n");
        return 2;
    }

    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        Slic3r::Model    model;
        Slic3r::GltfInfo info;
        std::string      message;
        bool             ok = false;
        try {
            ok = Slic3r::load_gltf(argv[i], &model, info, message);
        } catch (const std::exception &e) {
            // load_gltf's contract is "false plus a message", never an exception.
            std::fprintf(stderr, "THREW %s: %s\n", argv[i], e.what());
            ++failures;
            continue;
        } catch (...) {
            std::fprintf(stderr, "THREW %s: unknown exception\n", argv[i]);
            ++failures;
            continue;
        }
        if (ok) {
            if (model.objects.size() != 1 || model.objects.front()->volumes.empty() ||
                info.parts.size() != model.objects.front()->volumes.size()) {
                std::fprintf(stderr, "CONTRACT %s: true but the model does not match info.parts\n", argv[i]);
                ++failures;
                continue;
            }
            std::printf("ok    %s -> %u part(s)\n", argv[i], (unsigned) info.parts.size());
        } else if (message.empty()) {
            // An empty message makes Model::read_from_file throw the useless generic error.
            std::fprintf(stderr, "CONTRACT %s: false with an empty message\n", argv[i]);
            ++failures;
        } else {
            std::printf("no    %s -> %s\n", argv[i], message.c_str());
        }
    }
    return failures == 0 ? 0 : 1;
}
