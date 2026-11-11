// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/domains/plugins/host_ops.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_compressor_utility.h>
#include <dftracer/utils/fileio/file_decompressor_utility.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/runtime.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#endif

#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::domains::plugins {
namespace {

using dataframe::Series;

dstd::String to_string(const char* p, std::int32_t n) {
    return (p && n > 0) ? dstd::String(p, static_cast<std::size_t>(n))
                        : dstd::String();
}

// The utilities are coroutines; a registry op is synchronous. Drive one to
// completion on the default runtime, reporting an error result as failure at
// the C ABI seam a plugin calls across.
template <class Fn>
bool run_sync(const char* name, Fn body) {
    return default_runtime()
        .block_on(coro::named(name, coro::with_scope(std::move(body))))
        .has_value();
}

dftu_series* paths_column(const dstd::Vector<dstd::Path>& paths) {
    dstd::Vector<dstd::StringView> views;
    views.reserve(paths.size());
    for (const dstd::Path& path : paths) views.emplace_back(path.str());
    return Series::strings(views).release();
}

dftu_series* scan_dir(const char* root, std::int32_t root_size) {
    const dstd::String dir = to_string(root, root_size);
    dstd::Vector<dstd::Path> paths;
    if (!run_sync("dftu.fs.scan_dir",
                  [&](coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
                      auto listing_task =
                          runtime::fs::list_dir(dstd::PathView(dir));
                      auto listing = co_await std::move(listing_task);
                      if (!listing) co_return listing.error();
                      for (auto& entry : *listing)
                          paths.push_back(std::move(entry.path));
                      co_return dstd::Result<void>{};
                  })) {
        return nullptr;
    }
    return paths_column(paths);
}

dftu_series* scan_dir_pattern(const char* root, std::int32_t root_size,
                              const char* pattern, std::int32_t pattern_size) {
    const dstd::String dir = to_string(root, root_size);
    runtime::fs::FindOptions options;
    options.recursive = false;
    options.with_stat = false;
    dstd::String name = to_string(pattern, pattern_size);
    if (!name.empty()) {
        if (name.front() == '.') name.insert(0, 1, '*');
        options.patterns.push_back(std::move(name));
    }
    dstd::Vector<dstd::Path> paths;
    if (!run_sync(
            "dftu.fs.scan_dir_pattern",
            [&](coro::Scope& scope) -> coro::CoroTask<dstd::Result<void>> {
                auto found_task =
                    runtime::fs::find_all(scope, dstd::PathView(dir), options);
                auto found = co_await std::move(found_task);
                if (!found) co_return found.error();
                for (auto& file : *found) paths.push_back(std::move(file.path));
                co_return dstd::Result<void>{};
            })) {
        return nullptr;
    }
    return paths_column(paths);
}

std::int32_t compress(const char* in, std::int32_t in_size, const char* out,
                      std::int32_t out_size) {
    fileio::FileCompressionUtilityInput input =
        fileio::FileCompressionUtilityInput::from_file(to_string(in, in_size))
            .with_output(to_string(out, out_size));
    bool ok = false;
    if (!run_sync("dftu.file.compress",
                  [&](coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
                      fileio::FileCompressorUtility compressor;
                      ok = (co_await compressor(input)).has_value();
                      co_return dstd::Result<void>{};
                  })) {
        return 0;
    }
    return ok ? 1 : 0;
}

std::int32_t decompress(const char* in, std::int32_t in_size, const char* out,
                        std::int32_t out_size) {
    fileio::FileDecompressionUtilityInput input =
        fileio::FileDecompressionUtilityInput::from_file(to_string(in, in_size))
            .with_output(to_string(out, out_size));
    bool ok = false;
    if (!run_sync("dftu.file.decompress",
                  [&](coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
                      fileio::FileDecompressorUtility decompressor;
                      ok = (co_await decompressor(input)).has_value();
                      co_return dstd::Result<void>{};
                  })) {
        return 0;
    }
    return ok ? 1 : 0;
}

dftu_series* line_filter(const dftu_series* lines, const char* needle,
                         std::int32_t needle_size) {
    dftu_series* mask = dftu_series_str_contains(lines, needle, needle_size);
    if (!mask) return nullptr;
    dftu_series* selected = dftu_series_filter(lines, mask);
    dftu_series_free(mask);
    if (!selected) return nullptr;
    // filter yields a SELECTION view over `lines`; hand back a FLAT column so
    // the caller holds the kept lines outright, as the async original did.
    dftu_series* kept = dftu_series_materialize(selected);
    dftu_series_free(selected);
    return kept;
}

// Each op materializes what its async original streamed: a scan collects the
// whole listing, line_filter the whole kept column. That is bounded by the
// directory / input column, unlike a whole-file read, which is why there is no
// dftu.file.read here.
const dftu_op_desc HOST_OPS[] = {
    {"dftu.fs.scan_dir", DFTU_OP_SIG(SERIES, STR, NONE, NONE),
     reinterpret_cast<dftu_op_generic_fn>(&scan_dir)},
    {"dftu.fs.scan_dir_pattern", DFTU_OP_SIG(SERIES, STR, STR, NONE),
     reinterpret_cast<dftu_op_generic_fn>(&scan_dir_pattern)},
    {"dftu.file.compress", DFTU_OP_SIG(BOOL, STR, STR, NONE),
     reinterpret_cast<dftu_op_generic_fn>(&compress)},
    {"dftu.file.decompress", DFTU_OP_SIG(BOOL, STR, STR, NONE),
     reinterpret_cast<dftu_op_generic_fn>(&decompress)},
    {"dftu.text.line_filter", DFTU_OP_SIG(SERIES, SERIES, STR, NONE),
     reinterpret_cast<dftu_op_generic_fn>(&line_filter)},
};

}  // namespace

void register_host_ops() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (const dftu_op_desc& op : HOST_OPS) dftu_op_register(&op);
    });
}

namespace {
[[maybe_unused]] const bool REGISTERED = (register_host_ops(), true);
}  // namespace

}  // namespace dftracer::utils::domains::plugins

extern "C" void dftu_register_host_ops(void) {
    dftracer::utils::domains::plugins::register_host_ops();
}
