// SPDX-License-Identifier: MIT
// A process-lifetime, name-keyed registry of file formats and sinks, mirroring
// the provider registry in provider_registry.cpp, and the two C entry points
// that read it: dftu_lazyframe_scan_file and dftu_format_unregister.

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/file_format_registry.h>
#include <dftracer/utils/dataframe/internal/lazyframe_handle.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/line.h>
#include <dftracer/utils/json/value.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::dataframe {
namespace {

/// How many leading bytes of a file a format sees when it is probed.
constexpr std::size_t PROBE_HEAD_BYTES = 4096;

struct Registries {
    std::mutex mutex;
    dstd::Optional<
        dstd::HashMap<dstd::String, dstd::SharedPtr<FileFormatFactory>>>
        formats;
    dstd::Optional<dstd::HashMap<dstd::String, dstd::SharedPtr<SinkFactory>>>
        sinks;
};

Registries& registries() {
    Registries& held = dstd::Singleton<Registries>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, []() noexcept {
            Registries& dropped = dstd::Singleton<Registries>::get();
            const std::lock_guard<std::mutex> lock(dropped.mutex);
            dropped.formats.reset();
            dropped.sinks.reset();
        });
        return true;
    }();
    return held;
}

dstd::HashMap<dstd::String, dstd::SharedPtr<FileFormatFactory>>& formats() {
    Registries& r = registries();
    if (!r.formats) r.formats.emplace();
    return *r.formats;
}

dstd::HashMap<dstd::String, dstd::SharedPtr<SinkFactory>>& sinks() {
    Registries& r = registries();
    if (!r.sinks) r.sinks.emplace();
    return *r.sinks;
}

/// The leading PROBE_HEAD_BYTES of `path`, shorter when the file is.
dstd::Result<dstd::Vector<std::uint8_t>> read_head(dstd::StringView path) {
    auto opened = dstd::File::open(dstd::PathView(path), {});
    if (!opened)
        return common::make_error<"file format probe: cannot open '{}': {}">(
            common::ErrorCode::Io, path, opened.error().message());
    dstd::Vector<std::uint8_t> head(PROBE_HEAD_BYTES);
    auto read =
        opened->read_at(0, dstd::Span<std::uint8_t>(head.data(), head.size()));
    if (!read)
        return common::make_error<"file format probe: cannot read '{}': {}">(
            common::ErrorCode::Io, path, read.error().message());
    head.resize(*read);
    return head;
}

int clamped_score(int score) {
    if (score < 0) return 0;
    if (score > 100) return 100;
    return score;
}

}  // namespace

dstd::Result<void> register_file_format(
    dstd::SharedPtr<FileFormatFactory> format) {
    if (!format)
        return common::make_error<
            "file format register: the format must be set">(
            common::ErrorCode::InvalidArgument);
    const dstd::StringView name = format->name();
    if (name.empty())
        return common::make_error<"file format register: the name must be set">(
            common::ErrorCode::InvalidArgument);
    std::lock_guard<std::mutex> lock(registries().mutex);
    auto& reg = formats();
    if (reg.find(name) != reg.end())
        return common::make_error<"file format '{}' is already registered">(
            common::ErrorCode::InvalidArgument, name);
    reg.emplace(dstd::String(name), std::move(format));
    return {};
}

bool unregister_file_format(dstd::StringView name) {
    dstd::SharedPtr<FileFormatFactory> removed;
    {
        std::lock_guard<std::mutex> lock(registries().mutex);
        auto& reg = formats();
        auto it = reg.find(name);
        if (it == reg.end()) return false;
        removed = std::move(it->second);
        reg.erase(it);
    }
    // Released outside the lock: the format's destructor may be plugin code.
    return true;
}

dstd::SharedPtr<FileFormatFactory> find_file_format(dstd::StringView name) {
    std::lock_guard<std::mutex> lock(registries().mutex);
    auto& reg = formats();
    auto it = reg.find(name);
    if (it == reg.end()) return nullptr;
    return it->second;
}

dstd::Result<void> register_sink(dstd::SharedPtr<SinkFactory> sink) {
    if (!sink)
        return common::make_error<"sink register: the sink must be set">(
            common::ErrorCode::InvalidArgument);
    const dstd::StringView name = sink->name();
    if (name.empty())
        return common::make_error<"sink register: the name must be set">(
            common::ErrorCode::InvalidArgument);
    std::lock_guard<std::mutex> lock(registries().mutex);
    auto& reg = sinks();
    if (reg.find(name) != reg.end())
        return common::make_error<"sink '{}' is already registered">(
            common::ErrorCode::InvalidArgument, name);
    reg.emplace(dstd::String(name), std::move(sink));
    return {};
}

bool unregister_sink(dstd::StringView name) {
    dstd::SharedPtr<SinkFactory> removed;
    {
        std::lock_guard<std::mutex> lock(registries().mutex);
        auto& reg = sinks();
        auto it = reg.find(name);
        if (it == reg.end()) return false;
        removed = std::move(it->second);
        reg.erase(it);
    }
    return true;
}

dstd::SharedPtr<SinkFactory> find_sink(dstd::StringView name) {
    std::lock_guard<std::mutex> lock(registries().mutex);
    auto& reg = sinks();
    auto it = reg.find(name);
    if (it == reg.end()) return nullptr;
    return it->second;
}

dstd::Vector<dstd::String> registered_sink_names() {
    dstd::Vector<dstd::String> names;
    std::lock_guard<std::mutex> lock(registries().mutex);
    auto& reg = sinks();
    names.reserve(reg.size());
    for (const auto& entry : reg) names.push_back(entry.first);
    std::sort(names.begin(), names.end());
    return names;
}

bool has_file_formats() {
    std::lock_guard<std::mutex> lock(registries().mutex);
    return registries().formats && !registries().formats->empty();
}

namespace {

/// How well the built-in reader matches `head`: see @ref claim_reader.
int builtin_score(dstd::Span<const std::uint8_t> head) {
    constexpr int CLAIM = 50;
    constexpr int EMPTY = 10;
    if (head.size() >= 2 && head[0] == 0x1f && head[1] == 0x8b) return CLAIM;
    const char* text = reinterpret_cast<const char*>(head.data());
    std::size_t size = head.size();
    // A head cut at the limit ends in the middle of a line.
    if (size == PROBE_HEAD_BYTES) {
        const dstd::StringView whole(text, size);
        const std::size_t last = whole.rfind('\n');
        if (last == dstd::StringView::npos) return 0;
        size = last + 1;
    }
    constexpr std::size_t PROBED_LINES = 8;
    json::Document document;
    std::size_t seen = 0;
    std::size_t begin = 0;
    while (begin < size && seen < PROBED_LINES) {
        const dstd::StringView rest(text + begin, size - begin);
        std::size_t end = rest.find('\n');
        const dstd::StringView line =
            end == dstd::StringView::npos ? rest : rest.substr(0, end);
        begin += end == dstd::StringView::npos ? rest.size() : end + 1;
        dstd::StringView trimmed;
        if (!json::trim_and_validate_with_comma(line, trimmed)) continue;
        auto parsed = document.parse(trimmed);
        if (!parsed || parsed->type() != json::Value::Type::Object) return 0;
        ++seen;
    }
    return seen > 0 ? CLAIM : EMPTY;
}

struct TraceHook {
    std::mutex mutex;
    TraceOpen open = nullptr;
};

TraceHook& trace_hook() { return dstd::Singleton<TraceHook>::get(); }

}  // namespace

dstd::Vector<dstd::String> reader_names() {
    dstd::Vector<dstd::String> names;
    names.push_back(dstd::String(BUILTIN_READER));
    {
        std::lock_guard<std::mutex> lock(registries().mutex);
        if (registries().formats)
            for (const auto& entry : *registries().formats)
                names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

dstd::Result<Reader> claim_reader(dstd::StringView path,
                                  dstd::StringView forced) {
    if (!forced.empty()) {
        if (forced == BUILTIN_READER) return Reader{dstd::String(forced), {}};
        if (auto plugin = find_file_format(forced))
            return Reader{dstd::String(forced), std::move(plugin)};
        dstd::String known;
        dstd::fmt_append_join<", ">(known, reader_names());
        return common::make_error<"unknown format '{}'; known formats: {}">(
            common::ErrorCode::InvalidArgument, forced, known);
    }
    // A snapshot, so the plugins' probe() runs without the registry lock.
    dstd::Vector<dstd::SharedPtr<FileFormatFactory>> candidates;
    {
        std::lock_guard<std::mutex> lock(registries().mutex);
        if (registries().formats) {
            candidates.reserve(registries().formats->size());
            for (const auto& entry : *registries().formats)
                candidates.push_back(entry.second);
        }
    }
    auto head = read_head(path);
    if (!head) return head.error();

    Reader best;
    dstd::String rival;
    int best_score = builtin_score(
        dstd::Span<const std::uint8_t>(head->data(), head->size()));
    if (best_score > 0) best.name = BUILTIN_READER;
    for (const dstd::SharedPtr<FileFormatFactory>& candidate : candidates) {
        const int score =
            clamped_score(candidate->probe(head->data(), head->size(), path));
        if (score > best_score) {
            best = Reader{dstd::String(candidate->name()), candidate};
            rival.clear();
            best_score = score;
        } else if (score == best_score && score > 0) {
            rival = candidate->name();
        }
    }
    if (best_score == 0)
        return common::make_error<"no format claims {}">(
            common::ErrorCode::NotFound, path);
    if (!rival.empty())
        return common::make_error<"file formats '{}' and '{}' both claim {}">(
            common::ErrorCode::InvalidArgument, best.name, rival, path);
    return best;
}

dstd::Result<Reader> claim_trace_reader(dstd::StringView path,
                                        dstd::StringView forced) {
    if (forced.empty() && !has_file_formats())
        return Reader{dstd::String(BUILTIN_READER), {}};
    return claim_reader(path, forced);
}

void set_trace_open(TraceOpen open) {
    TraceHook& hook = trace_hook();
    std::lock_guard<std::mutex> lock(hook.mutex);
    hook.open = open;
}

TraceOpen trace_open() {
    TraceHook& hook = trace_hook();
    std::lock_guard<std::mutex> lock(hook.mutex);
    return hook.open;
}

}  // namespace dftracer::utils::dataframe

int dftu_format_unregister(const char* name) {
    if (!name) return 1;
    return dftracer::utils::dataframe::unregister_file_format(name) ? 0 : 1;
}

int dftu_lazyframe_scan_file(const char* path, const char* index_dir,
                             std::size_t index_dir_len, dftu_lazyframe** out) {
    if (!path || !out || (!index_dir && index_dir_len != 0)) return 1;
    const dftracer::utils::dstd::StringView dir =
        index_dir ? dftracer::utils::dstd::StringView(index_dir, index_dir_len)
                  : dftracer::utils::dstd::StringView();
    *out = dftracer::utils::dataframe::lazyframe_handle_wrap(
        dftracer::utils::dataframe::LazyFrame::scan_file(path, dir));
    return 0;
}
