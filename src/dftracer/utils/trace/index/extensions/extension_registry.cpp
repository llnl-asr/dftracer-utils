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
        built_.push_back(
            {std::move(ext), std::move(b), false, false, false, {}, {}});
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
        ExtensionOut out{{}, b.ext->payload_cap(), false};
        const auto status = b.builder->finish_granule(*granule_, out);
        if (!status) report(b, "finish_granule", status.error());
        if (status && !b.chunk_failed && !out.over && !out.bytes.empty())
            b.granules.emplace_back(*granule_, std::move(out.bytes));
        b.chunk_failed = false;
    }
    granule_.reset();
}

void ExtensionBuilders::step(std::uint64_t granule,
                             dstd::Span<const dstd::StringView> lines) {
    if (granule_ && *granule_ != granule) end_granule();
    granule_ = granule;
    for (auto& b : built_) {
        if (!b.builder || b.chunk_failed) continue;
        const auto status = b.builder->step(lines);
        if (!status) {
            b.chunk_failed = true;
            b.step_failed = true;
            report(b, "step", status.error());
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
