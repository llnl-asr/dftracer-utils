// SPDX-License-Identifier: MIT
//
// Time to list and stat one tree with find_all, and a hash of the files found.
//
//   fs_bench ROOT [--threads N] [--io-threads N] [--reps N]
//   fs_bench --smoke   (builds a small tree; exits nonzero when find_all and
//                       a serial walk find different files or sizes)
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/runtime.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dftracer::utils;

namespace {

struct Row {
    dstd::Vector<dstd::String> paths;
    std::uint64_t bytes = 0;
};

Row scan_serial(const dstd::Path& root) {
    Row row;
    const auto walked =
        dstd::fs::walk(root, [&](dstd::PathView path, dstd::fs::FileKind kind) {
            if (kind == dstd::fs::FileKind::File &&
                path.str().ends_with(".pfw.gz")) {
                const auto size = dstd::fs::file_size(path);
                row.paths.push_back(dstd::String(path.str()));
                if (size) row.bytes += *size;
            }
            return dstd::fs::WalkAction::Continue;
        });
    if (!walked) std::exit(2);
    return row;
}

Row scan_with_find_all(dstd::StringView root, std::size_t threads,
                       std::size_t io_threads) {
    RuntimeSettings settings;
    settings.threads = threads;
    settings.io_threads = io_threads;
    Row row;
    auto ran = configure_default_runtime(settings).block_on(coro::named(
        "find_all",
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            runtime::fs::FindOptions options;
            options.patterns.push_back(dstd::String("*.pfw.gz"));
            options.skip_dirs.push_back(dstd::String(".dftindex*"));
            options.skip_dirs.push_back(dstd::String("split"));
            options.skip_dirs.push_back(dstd::String("schemas"));
            options.skip_unreadable = true;
            auto found =
                co_await runtime::fs::find_all(scope, root, std::move(options));
            if (!found) {
                std::fprintf(stderr, "find_all failed\n");
                std::exit(2);
            }
            for (const auto& file : *found) {
                row.paths.push_back(dstd::String(file.path.str()));
                row.bytes += file.size;
            }
        })));
    if (!ran) {
        std::fprintf(stderr, "find_all: %s\n", ran.error().format().c_str());
        std::exit(2);
    }
    return row;
}

std::uint64_t hash_paths(dstd::Vector<dstd::String>& paths) {
    std::sort(paths.begin(), paths.end());
    std::uint64_t hash = dstd::hash::FNV1A_OFFSET_BASIS;
    for (const dstd::String& path : paths) {
        for (const char c : path) {
            hash = (hash ^ static_cast<unsigned char>(c)) *
                   dstd::hash::FNV1A_PRIME;
        }
        hash = (hash ^ 0xff) * dstd::hash::FNV1A_PRIME;
    }
    return hash;
}

int run_smoke() {
    char pattern[] = "/tmp/fs_bench_XXXXXX";
    const char* made = ::mkdtemp(pattern);
    if (made == nullptr) return 2;
    const dstd::Path root(made);
    for (int d = 0; d < 10; ++d) {
        const dstd::Path dir = root / dstd::to_string(d);
        if (!dstd::fs::create_directories(dir).has_value()) return 2;
        for (int f = 0; f < 40; ++f) {
            const dstd::Path file =
                dir / dstd::String(dstd::fmt<"{}.pfw.gz">(f));
            if (!dstd::fs::write_file(file, "x")) return 2;
        }
    }
    Row serial = scan_serial(root);
    Row found = scan_with_find_all(root.str(), 4, 4);
    const bool same = serial.paths.size() == 400 &&
                      hash_paths(serial.paths) == hash_paths(found.paths) &&
                      serial.bytes == found.bytes;
    dstd::fs::remove_all(root).discard();
    if (!same) std::fprintf(stderr, "find_all and the serial walk differ\n");
    return same ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--smoke") == 0) return run_smoke();
    if (argc < 2) {
        std::fprintf(stderr, "usage: fs_bench ROOT [options]\n");
        return 2;
    }
    const char* root = argv[1];
    std::size_t threads = 0;
    std::size_t io_threads = 0;
    int reps = 5;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--threads") == 0) {
            threads = static_cast<std::size_t>(std::atoi(argv[i + 1]));
        }
        if (std::strcmp(argv[i], "--io-threads") == 0) {
            io_threads = static_cast<std::size_t>(std::atoi(argv[i + 1]));
        }
        if (std::strcmp(argv[i], "--reps") == 0) reps = std::atoi(argv[i + 1]);
    }
    for (int rep = 0; rep < reps; ++rep) {
        const auto start = std::chrono::steady_clock::now();
        Row row = scan_with_find_all(root, threads, io_threads);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
        const std::size_t count = row.paths.size();
        std::printf(
            "find_all rep=%d files=%zu bytes=%llu hash=%016llx ms=%.1f\n", rep,
            count, static_cast<unsigned long long>(row.bytes),
            static_cast<unsigned long long>(hash_paths(row.paths)), ms);
        std::fflush(stdout);
    }
    return 0;
}
