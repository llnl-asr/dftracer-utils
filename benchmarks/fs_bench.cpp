// SPDX-License-Identifier: MIT
//
// Time to list and stat one tree, and a hash of the files found, by method.
//
//   fs_bench ROOT [--method scanner|find_all] [--threads N] [--io-threads N]
//            [--reps N]
//   fs_bench --smoke   (builds a small tree; exits nonzero when the methods
//                       find different files)
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

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

Row scan_with_scanner(dstd::StringView root, std::size_t threads,
                      std::size_t io_threads) {
    using utilities::filesystem::PatternDirectoryScannerUtility;
    using utilities::filesystem::PatternDirectoryScannerUtilityInput;
    dstd::Vector<dstd::String> patterns;
    patterns.push_back(dstd::String(".pfw.gz"));
    PatternDirectoryScannerUtilityInput input(root, std::move(patterns), true,
                                              true);
    auto entries =
        utilities::filesystem::scan_on_pool<PatternDirectoryScannerUtility>(
            threads, io_threads, std::move(input));
    Row row;
    for (const auto& entry : entries) {
        row.paths.push_back(dstd::String(entry.path.str()));
        row.bytes += entry.size;
    }
    return row;
}

Row scan_with_find_all(dstd::StringView root, std::size_t threads,
                       std::size_t io_threads) {
    RuntimeSettings settings;
    settings.threads = threads;
    settings.io_threads = io_threads;
    Row row;
    configure_default_runtime(settings).block_on(coro::named(
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
    return row;
}

std::uint64_t hash_paths(dstd::Vector<dstd::String>& paths) {
    std::sort(paths.begin(), paths.end());
    std::uint64_t hash = 14695981039346656037ULL;
    for (const dstd::String& path : paths) {
        for (const char c : path) {
            hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
        }
        hash = (hash ^ 0xff) * 1099511628211ULL;
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
        if (!dstd::fs::create_directories(dir)) return 2;
        for (int f = 0; f < 40; ++f) {
            const dstd::Path file = dir / dstd::String(dstd::to_string(f) +
                                                       dstd::String(".pfw.gz"));
            if (!dstd::fs::write_file(file, "x")) return 2;
        }
    }
    Row scanned = scan_with_scanner(root.str(), 4, 4);
    Row found = scan_with_find_all(root.str(), 4, 4);
    const bool same = scanned.paths.size() == 400 &&
                      hash_paths(scanned.paths) == hash_paths(found.paths) &&
                      scanned.bytes == found.bytes;
    (void)dstd::fs::remove_all(root);
    if (!same) std::fprintf(stderr, "the methods found different files\n");
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
    const char* method = "find_all";
    std::size_t threads = 0;
    std::size_t io_threads = 0;
    int reps = 5;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--method") == 0) method = argv[i + 1];
        if (std::strcmp(argv[i], "--threads") == 0) {
            threads = static_cast<std::size_t>(std::atoi(argv[i + 1]));
        }
        if (std::strcmp(argv[i], "--io-threads") == 0) {
            io_threads = static_cast<std::size_t>(std::atoi(argv[i + 1]));
        }
        if (std::strcmp(argv[i], "--reps") == 0) reps = std::atoi(argv[i + 1]);
    }
    const bool use_scanner = std::strcmp(method, "scanner") == 0;
    for (int rep = 0; rep < reps; ++rep) {
        const auto start = std::chrono::steady_clock::now();
        Row row = use_scanner ? scan_with_scanner(root, threads, io_threads)
                              : scan_with_find_all(root, threads, io_threads);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
        const std::size_t count = row.paths.size();
        std::printf("%s rep=%d files=%zu bytes=%llu hash=%016llx ms=%.1f\n",
                    method, rep, count,
                    static_cast<unsigned long long>(row.bytes),
                    static_cast<unsigned long long>(hash_paths(row.paths)), ms);
        std::fflush(stdout);
    }
    return 0;
}
