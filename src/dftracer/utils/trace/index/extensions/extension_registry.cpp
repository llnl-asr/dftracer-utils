// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/extensions/extension_registry.h>
#include <dftracer/utils/trace/index/extensions/plugin_columns.h>
#include <dftracer/utils/trace/index/tables.h>

#include <mutex>
#include <shared_mutex>

namespace dftracer::utils::trace::index::extensions {
namespace {

constexpr std::uint64_t DEFAULT_MAX_PAYLOAD = std::uint64_t{1} << 20;

struct Registry {
    std::shared_mutex mutex;
    dstd::Map<dstd::String, ExtensionPtr, std::less<>> by_name;
};

Registry& registry() {
    Registry& r = dstd::Singleton<Registry>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, []() noexcept {
            Registry& held = dstd::Singleton<Registry>::get();
            decltype(held.by_name) dropped;
            std::unique_lock lock(held.mutex);
            dropped.swap(held.by_name);
        });
        return true;
    }();
    return r;
}

}  // namespace

std::uint64_t Extension::payload_cap() const {
    const std::uint64_t cap = max_payload();
    return cap ? cap : DEFAULT_MAX_PAYLOAD;
}

dstd::Result<void> register_extension(ExtensionPtr extension) {
    const dstd::String name(extension->name());
    auto& r = registry();
    std::unique_lock lock(r.mutex);
    if (!r.by_name.emplace(name, std::move(extension)).second) {
        return common::make_error<
            "index extension '{}' refused: that name is already registered">(
            common::ErrorCode::InvalidArgument, name);
    }
    return {};
}

void unregister_extension(dstd::StringView name) {
    auto& r = registry();
    std::unique_lock lock(r.mutex);
    if (auto it = r.by_name.find(name); it != r.by_name.end())
        r.by_name.erase(it);
}

dstd::Vector<ExtensionPtr> registered_extensions() {
    auto& r = registry();
    std::shared_lock lock(r.mutex);
    dstd::Vector<ExtensionPtr> out;
    out.reserve(r.by_name.size());
    for (const auto& [name, ext] : r.by_name) out.push_back(ext);
    return out;
}

ExtensionPtr find_extension(dstd::StringView name) {
    auto& r = registry();
    std::shared_lock lock(r.mutex);
    auto it = r.by_name.find(name);
    return it == r.by_name.end() ? nullptr : it->second;
}

ExtensionBuilders::ExtensionBuilders(dstd::Vector<ExtensionPtr> extensions) {
    built_.reserve(extensions.size());
    for (auto& ext : extensions) {
        dstd::UniquePtr<ExtensionBuilder> b;
        auto made = ext->make_builder();
        if (made && *made) {
            b = std::move(*made);
        } else {
            DFTRACER_UTILS_LOG_WARN(
                "Index extension '{}' built nothing: make_builder failed: {}",
                ext->name(),
                made ? dstd::StringView() : made.error().message());
        }
        dstd::UniquePtr<PluginColumns> columns;
        if (b && !ext->columns().empty())
            columns = dstd::make_unique<PluginColumns>(ext->columns());
        built_.push_back({std::move(ext),
                          std::move(b),
                          false,
                          false,
                          false,
                          {},
                          {},
                          std::move(columns)});
    }
}

ExtensionBuilders::~ExtensionBuilders() = default;

void ExtensionBuilders::report(Built& built, dstd::StringView callback,
                               const dstd::Error& error) {
    if (built.reported) return;
    built.reported = true;
    DFTRACER_UTILS_LOG_WARN(
        "Index extension '{}' {} failed with {}; nothing is "
        "stored for that chunk or file",
        built.ext->name(), callback, error.message());
}

void ExtensionBuilders::end_granule() {
    if (!granule_) return;
    for (auto& b : built_) {
        if (!b.builder) continue;
        if (b.columns) {
            auto taken = b.columns->take();
            if (!b.chunk_failed && !taken.never.empty()) {
                fail_chunk(
                    b, "step_column",
                    common::make_error<
                        "the path '{}' is never_index in the record schema, "
                        "so the index has no value of it">(
                        common::ErrorCode::InvalidArgument, taken.never.front())
                        .error());
            }
            if (!b.chunk_failed && !taken.mixed.empty()) {
                fail_chunk(
                    b, "step_column",
                    common::make_error<
                        "the values of path '{}' have more than one "
                        "type in the chunk">(common::ErrorCode::InvalidArgument,
                                             taken.mixed.front())
                        .error());
            }
            for (auto& column : taken.columns) {
                if (b.chunk_failed) break;
                const auto status =
                    b.builder->step_column(column.path, column.series);
                if (!status) fail_chunk(b, "step_column", status.error());
            }
        }
        ExtensionOut out{{}, b.ext->payload_cap(), false};
        const auto status = b.builder->finish_granule(*granule_, out);
        if (!status) report(b, "finish_granule", status.error());
        if (status && !b.chunk_failed && !out.over && !out.bytes.empty())
            b.granules.emplace_back(*granule_, std::move(out.bytes));
        b.chunk_failed = false;
    }
    granule_.reset();
}

void ExtensionBuilders::fail_chunk(Built& built, dstd::StringView callback,
                                   const dstd::Error& error) {
    built.chunk_failed = true;
    built.step_failed = true;
    report(built, callback, error);
}

void ExtensionBuilders::set_never_index(dstd::Span<const dstd::String> never) {
    for (auto& b : built_)
        if (b.columns) b.columns->set_never_index(never);
}

bool ExtensionBuilders::takes_lines() const {
    for (const auto& b : built_)
        if (b.builder && b.ext->takes_lines()) return true;
    return false;
}

void ExtensionBuilders::step(
    std::uint64_t granule, dstd::Span<const dstd::StringView> lines,
    dstd::Span<const trace::views::detail::FoldEvent> events,
    const dftracer::utils::dstd::StringIntern& intern) {
    if (granule_ && *granule_ != granule) end_granule();
    granule_ = granule;
    for (auto& b : built_) {
        if (!b.builder || b.chunk_failed) continue;
        if (b.ext->takes_lines()) {
            const auto status = b.builder->step(lines);
            if (!status) fail_chunk(b, "step", status.error());
        }
        if (b.columns && !b.chunk_failed) b.columns->add(events, intern);
    }
}

void ExtensionBuilders::step_frame(std::uint64_t granule,
                                   const dataframe::DataFrame& frame) {
    if (granule_ && *granule_ != granule) end_granule();
    granule_ = granule;
    for (auto& b : built_) {
        if (!b.builder || b.chunk_failed) continue;
        // An extension that reads lines has seen none of this chunk, so what
        // it stores would claim the chunk is empty.
        if (b.ext->columns().empty()) {
            fail_chunk(b, "step_column",
                       common::make_error<
                           "the extension takes record lines and the reader "
                           "has none">(common::ErrorCode::InvalidArgument)
                           .error());
            continue;
        }
        for (std::size_t c = 0; c < frame.num_columns() && !b.chunk_failed;
             ++c) {
            const dstd::String path(frame.names[c].view());
            bool wanted = false;
            for (const dstd::String& want : b.ext->columns())
                wanted |= want == "*" || want == path;
            if (!wanted) continue;
            const auto& column = frame.columns[c];
            const auto type = column.type();
            if (type != dataframe::TypeId::Int64 &&
                type != dataframe::TypeId::Uint64 &&
                type != dataframe::TypeId::Float64 &&
                type != dataframe::TypeId::String) {
                fail_chunk(
                    b, "step_column",
                    common::make_error<
                        "the column '{}' has a type the extension "
                        "cannot take">(common::ErrorCode::InvalidArgument, path)
                        .error());
                break;
            }
            const auto status = b.builder->step_column(path, column);
            if (!status) fail_chunk(b, "step_column", status.error());
        }
    }
}

void ExtensionBuilders::finish() {
    end_granule();
    for (auto& b : built_) {
        if (!b.builder || !b.builder->has_finish_file() || b.step_failed)
            continue;
        ExtensionOut out{{}, b.ext->payload_cap(), false};
        const auto status = b.builder->finish_file(out);
        if (!status) report(b, "finish_file", status.error());
        if (status && !out.over && !out.bytes.empty())
            b.file = std::move(out.bytes);
    }
}

void ExtensionBuilders::write(FileBuild& build) {
    for (auto& b : built_) {
        const dstd::String name(b.ext->name());
        if (!b.builder) {
            trace::index::fail_plugin(build, name, b.ext->version(),
                                      b.ext->params_hash());
            continue;
        }
        GranuleViews granules;
        granules.reserve(b.granules.size());
        for (const auto& [granule, bytes] : b.granules)
            granules.emplace_back(granule,
                                  dstd::StringView(bytes.data(), bytes.size()));
        dstd::Optional<dstd::StringView> file;
        if (b.file) file = dstd::StringView(b.file->data(), b.file->size());
        trace::index::put_plugin_views(build, name, b.ext->version(),
                                       b.ext->params_hash(), granules, file);
    }
}

}  // namespace dftracer::utils::trace::index::extensions
