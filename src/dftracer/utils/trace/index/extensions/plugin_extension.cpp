// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/abi_context.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/extensions/plugin_extension.h>
#include <dftracer/utils/trace/index/tables.h>

#include <mutex>
#include <shared_mutex>

namespace dftracer::utils::trace::index::extensions {
namespace {

constexpr std::uint64_t DEFAULT_MAX_PAYLOAD = std::uint64_t{1} << 20;

struct Registry {
    std::shared_mutex mutex;
    dstd::Map<dstd::String, PluginExtensionPtr, std::less<>> by_name;
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

struct Out {
    dstd::String bytes;
    std::uint64_t cap = 0;
    bool over = false;
};

void append_out(void* self, const void* data, std::uint64_t len) {
    auto& out = *static_cast<Out*>(self);
    if (out.over || !data) return;
    if (len > out.cap - out.bytes.size()) {
        out.over = true;
        return;
    }
    out.bytes.append(static_cast<const char*>(data), len);
}

}  // namespace

std::uint64_t PluginExtension::max_payload() const {
    return vt.max_payload ? vt.max_payload : DEFAULT_MAX_PAYLOAD;
}

dstd::Result<void> register_plugin_extension(dstd::StringView name,
                                             const ::dftu_index_extension* vt,
                                             void* self) {
    if (!vt || vt->struct_size != sizeof(::dftu_index_extension)) {
        return common::make_error<
            "index extension '{}' refused: its struct_size is not "
            "sizeof(dftu_index_extension)">(common::ErrorCode::InvalidArgument,
                                            name);
    }
    if (!vt->make_builder || !vt->step || !vt->finish_granule ||
        !vt->destroy_builder || !vt->compile || !vt->may_match ||
        !vt->release) {
        return common::make_error<
            "index extension '{}' refused: a required callback is NULL">(
            common::ErrorCode::InvalidArgument, name);
    }
    auto ext = dstd::make_shared<PluginExtension>(
        PluginExtension{dstd::String(name), *vt, self});
    auto& r = registry();
    std::unique_lock lock(r.mutex);
    if (!r.by_name.emplace(ext->name, std::move(ext)).second) {
        return common::make_error<
            "index extension '{}' refused: that name is already registered">(
            common::ErrorCode::InvalidArgument, name);
    }
    return {};
}

void unregister_plugin_extension(dstd::StringView name) {
    auto& r = registry();
    std::unique_lock lock(r.mutex);
    if (auto it = r.by_name.find(name); it != r.by_name.end())
        r.by_name.erase(it);
}

dstd::Vector<PluginExtensionPtr> plugin_extensions() {
    auto& r = registry();
    std::shared_lock lock(r.mutex);
    dstd::Vector<PluginExtensionPtr> out;
    out.reserve(r.by_name.size());
    for (const auto& [name, ext] : r.by_name) out.push_back(ext);
    return out;
}

PluginExtensionPtr find_plugin_extension(dstd::StringView name) {
    auto& r = registry();
    std::shared_lock lock(r.mutex);
    auto it = r.by_name.find(name);
    return it == r.by_name.end() ? nullptr : it->second;
}

PluginBuilders::PluginBuilders(dstd::Vector<PluginExtensionPtr> extensions) {
    built_.reserve(extensions.size());
    for (auto& ext : extensions) {
        coro::AbiContext ctx;
        void* b = nullptr;
        const dftu_status status =
            ext->vt.make_builder(ctx.get(), ext->self, &b);
        if (status < 0 || !b) {
            DFTRACER_UTILS_LOG_WARN(
                "Index extension '{}' built nothing: make_builder failed: {}",
                ext->name, ctx.message());
            b = nullptr;
        }
        built_.push_back({std::move(ext), b, false, false, false, {}, {}});
    }
}

PluginBuilders::~PluginBuilders() {
    for (auto& b : built_)
        if (b.builder) b.ext->vt.destroy_builder(b.builder);
}

void PluginBuilders::report(Built& built, const char* callback,
                            dftu_status status, dstd::StringView message) {
    if (built.reported) return;
    built.reported = true;
    DFTRACER_UTILS_LOG_WARN(
        "Index extension '{}' {} failed with status {}: {}; nothing is "
        "stored for that chunk or file",
        built.ext->name, callback, status,
        message.empty() ? dstd::StringView("no message") : message);
}

void PluginBuilders::end_granule() {
    if (!granule_) return;
    for (auto& b : built_) {
        if (!b.builder) continue;
        Out out{{}, b.ext->max_payload(), false};
        const ::dftu_index_out sink{sizeof(::dftu_index_out), &out, append_out};
        coro::AbiContext ctx;
        const dftu_status status =
            b.ext->vt.finish_granule(ctx.get(), b.builder, *granule_, &sink);
        if (status < 0) report(b, "finish_granule", status, ctx.message());
        if (status >= 0 && !b.chunk_failed && !out.over && !out.bytes.empty())
            b.granules.emplace_back(*granule_, std::move(out.bytes));
        b.chunk_failed = false;
    }
    granule_.reset();
}

void PluginBuilders::step(std::uint64_t granule,
                          dstd::Span<const dstd::StringView> lines) {
    if (granule_ && *granule_ != granule) end_granule();
    granule_ = granule;
    lines_.clear();
    lines_.reserve(lines.size());
    for (dstd::StringView l : lines)
        lines_.push_back({l.data(), l.size(), nullptr, nullptr});
    for (auto& b : built_) {
        if (!b.builder || b.chunk_failed) continue;
        coro::AbiContext ctx;
        const dftu_status status =
            b.ext->vt.step(ctx.get(), b.builder, lines_.data(), lines_.size());
        if (status < 0) {
            b.chunk_failed = true;
            b.step_failed = true;
            report(b, "step", status, ctx.message());
        }
    }
}

void PluginBuilders::finish() {
    end_granule();
    for (auto& b : built_) {
        if (!b.builder || !b.ext->vt.finish_file || b.step_failed) continue;
        Out out{{}, b.ext->max_payload(), false};
        const ::dftu_index_out sink{sizeof(::dftu_index_out), &out, append_out};
        coro::AbiContext ctx;
        const dftu_status status =
            b.ext->vt.finish_file(ctx.get(), b.builder, &sink);
        if (status < 0) report(b, "finish_file", status, ctx.message());
        if (status >= 0 && !out.over && !out.bytes.empty())
            b.file = std::move(out.bytes);
    }
}

void PluginBuilders::write(FileBuild& build) {
    for (auto& b : built_) {
        const dstd::String& name = b.ext->name;
        if (!b.builder) {
            trace::index::fail_plugin(build, name, b.ext->vt.version,
                                      b.ext->vt.params_hash);
            continue;
        }
        GranuleViews granules;
        granules.reserve(b.granules.size());
        for (const auto& [granule, bytes] : b.granules)
            granules.emplace_back(granule,
                                  dstd::StringView(bytes.data(), bytes.size()));
        dstd::Optional<dstd::StringView> file;
        if (b.file) file = dstd::StringView(b.file->data(), b.file->size());
        trace::index::put_plugin_views(build, name, b.ext->vt.version,
                                       b.ext->vt.params_hash, granules, file);
    }
}

}  // namespace dftracer::utils::trace::index::extensions
