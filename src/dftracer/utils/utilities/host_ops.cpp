#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/utilities/fileio/file_compressor_utility.h>
#include <dftracer/utils/utilities/fileio/file_decompressor_utility.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/host_ops.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/frame_ops.h>
#endif

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities {
namespace {

using dataframe::Series;

std::string to_string(const char* p, std::int32_t n) {
    return (p && n > 0) ? std::string(p, static_cast<std::size_t>(n))
                        : std::string();
}

// The utilities are coroutines; a registry op is synchronous. Drive one to
// completion on the default runtime, reporting a throw as failure rather than
// unwinding through the C ABI seam a plugin calls across.
template <class Fn>
bool run_sync(const char* name, Fn body) {
    try {
        default_runtime().run_blocking(name, std::move(body));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

dftu_series* paths_column(const std::vector<filesystem::FileEntry>& entries,
                          bool files_only) {
    std::vector<std::string> paths;
    paths.reserve(entries.size());
    for (const filesystem::FileEntry& e : entries) {
        if (files_only && !e.is_regular_file) continue;
        paths.push_back(e.path.string());
    }
    return Series::strings(paths).release();
}

dftu_series* scan_dir(const char* root, std::int32_t root_len) {
    filesystem::DirectoryScannerUtilityInput input{
        fs::path(to_string(root, root_len))};
    std::vector<filesystem::FileEntry> entries;
    if (!run_sync("dftu.fs.scan_dir",
                  [&](CoroScope& scope) -> coro::CoroTask<void> {
                      filesystem::DirectoryScannerUtility scanner;
                      entries = co_await scanner(scope, input);
                  })) {
        return nullptr;
    }
    return paths_column(entries, false);
}

dftu_series* scan_dir_pattern(const char* root, std::int32_t root_len,
                              const char* pattern, std::int32_t pattern_len) {
    filesystem::PatternDirectoryScannerUtilityInput input(
        to_string(root, root_len), {to_string(pattern, pattern_len)});
    std::vector<filesystem::FileEntry> entries;
    if (!run_sync("dftu.fs.scan_dir_pattern",
                  [&](CoroScope& scope) -> coro::CoroTask<void> {
                      filesystem::PatternDirectoryScannerUtility scanner;
                      entries = co_await scanner(scope, input);
                  })) {
        return nullptr;
    }
    return paths_column(entries, true);
}

std::int32_t compress(const char* in, std::int32_t in_len, const char* out,
                      std::int32_t out_len) {
    fileio::FileCompressionUtilityInput input =
        fileio::FileCompressionUtilityInput::from_file(to_string(in, in_len))
            .with_output(to_string(out, out_len));
    bool ok = false;
    if (!run_sync("dftu.file.compress",
                  [&](CoroScope&) -> coro::CoroTask<void> {
                      fileio::FileCompressorUtility compressor;
                      ok = (co_await compressor(input)).has_value();
                  })) {
        return 0;
    }
    return ok ? 1 : 0;
}

std::int32_t decompress(const char* in, std::int32_t in_len, const char* out,
                        std::int32_t out_len) {
    fileio::FileDecompressionUtilityInput input =
        fileio::FileDecompressionUtilityInput::from_file(to_string(in, in_len))
            .with_output(to_string(out, out_len));
    bool ok = false;
    if (!run_sync("dftu.file.decompress",
                  [&](CoroScope&) -> coro::CoroTask<void> {
                      fileio::FileDecompressorUtility decompressor;
                      ok = (co_await decompressor(input)).has_value();
                  })) {
        return 0;
    }
    return ok ? 1 : 0;
}

dftu_series* line_filter(const dftu_series* lines, const char* needle,
                         std::int32_t needle_len) {
    dftu_series* mask = dftu_series_str_contains(lines, needle, needle_len);
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
     reinterpret_cast<const void*>(&scan_dir)},
    {"dftu.fs.scan_dir_pattern", DFTU_OP_SIG(SERIES, STR, STR, NONE),
     reinterpret_cast<const void*>(&scan_dir_pattern)},
    {"dftu.file.compress", DFTU_OP_SIG(BOOL, STR, STR, NONE),
     reinterpret_cast<const void*>(&compress)},
    {"dftu.file.decompress", DFTU_OP_SIG(BOOL, STR, STR, NONE),
     reinterpret_cast<const void*>(&decompress)},
    {"dftu.text.line_filter", DFTU_OP_SIG(SERIES, SERIES, STR, NONE),
     reinterpret_cast<const void*>(&line_filter)},
};

}  // namespace

void register_host_ops() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (const dftu_op_desc& op : HOST_OPS) dftu_op_register(&op);
#ifdef DFTRACER_UTILS_ENABLE_ARROW
        common::arrow::register_frame_ops();
#endif
    });
}

namespace {
[[maybe_unused]] const bool REGISTERED = (register_host_ops(), true);
}  // namespace

}  // namespace dftracer::utils::utilities

extern "C" void dftu_register_host_ops(void) {
    dftracer::utils::utilities::register_host_ops();
}
