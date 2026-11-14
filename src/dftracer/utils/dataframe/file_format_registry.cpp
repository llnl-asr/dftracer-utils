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
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

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

dstd::Result<dstd::SharedPtr<const Source>> open_file_with_format(
    dstd::StringView path) {
    auto head = read_head(path);
    if (!head) return head.error();

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

    dstd::SharedPtr<FileFormatFactory> best;
    dstd::SharedPtr<FileFormatFactory> rival;
    int best_score = 0;
    for (const dstd::SharedPtr<FileFormatFactory>& candidate : candidates) {
        const int score =
            clamped_score(candidate->probe(head->data(), head->size(), path));
        if (score > best_score) {
            best = candidate;
            rival = nullptr;
            best_score = score;
        } else if (score == best_score && score > 0) {
            rival = candidate;
        }
    }

    if (!best)
        return common::make_error<"no format claims {}">(
            common::ErrorCode::NotFound, path);
    if (rival)
        return common::make_error<"file formats '{}' and '{}' both claim {}">(
            common::ErrorCode::InvalidArgument, best->name(), rival->name(),
            path);
    return best->open(path);
}

}  // namespace dftracer::utils::dataframe

int dftu_format_unregister(const char* name) {
    if (!name) return 1;
    return dftracer::utils::dataframe::unregister_file_format(name) ? 0 : 1;
}

int dftu_lazyframe_scan_file(const char* path, dftu_lazyframe** out) {
    if (!path || !out) return 1;
    *out = dftracer::utils::dataframe::lazyframe_handle_wrap(
        dftracer::utils::dataframe::LazyFrame::scan_file(path));
    return 0;
}
