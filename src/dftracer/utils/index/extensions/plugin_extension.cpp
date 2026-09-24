#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>

#include <map>
#include <mutex>
#include <shared_mutex>

namespace dftracer::utils::index::extensions {
namespace {

constexpr std::uint64_t DEFAULT_MAX_PAYLOAD = std::uint64_t{1} << 20;

struct Registry {
    std::shared_mutex mutex;
    std::map<std::string, PluginExtensionPtr, std::less<>> by_name;
};

Registry& registry() {
    static Registry r;
    return r;
}

struct Out {
    std::string bytes;
    std::uint64_t cap = 0;
    bool over = false;
};

void append_out(void* ctx, const void* data, std::uint64_t len) {
    auto& out = *static_cast<Out*>(ctx);
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

bool register_plugin_extension(std::string_view name,
                               const ::dftu_index_extension* vt, void* self) {
    if (!vt || !vt->make_builder || !vt->step || !vt->finish_granule ||
        !vt->destroy_builder || !vt->compile || !vt->may_match ||
        !vt->release) {
        DFTRACER_UTILS_LOG_ERROR(
            "Index extension '%.*s' refused: a required callback is NULL",
            static_cast<int>(name.size()), name.data());
        return false;
    }
    auto ext = std::make_shared<PluginExtension>(
        PluginExtension{std::string(name), *vt, self});
    auto& r = registry();
    std::unique_lock lock(r.mutex);
    if (!r.by_name.emplace(ext->name, std::move(ext)).second) {
        DFTRACER_UTILS_LOG_ERROR(
            "Index extension '%.*s' refused: that name is already registered",
            static_cast<int>(name.size()), name.data());
        return false;
    }
    return true;
}

void unregister_plugin_extension(std::string_view name) {
    auto& r = registry();
    std::unique_lock lock(r.mutex);
    if (auto it = r.by_name.find(name); it != r.by_name.end())
        r.by_name.erase(it);
}

std::vector<PluginExtensionPtr> plugin_extensions() {
    auto& r = registry();
    std::shared_lock lock(r.mutex);
    std::vector<PluginExtensionPtr> out;
    out.reserve(r.by_name.size());
    for (const auto& [name, ext] : r.by_name) out.push_back(ext);
    return out;
}

PluginExtensionPtr find_plugin_extension(std::string_view name) {
    auto& r = registry();
    std::shared_lock lock(r.mutex);
    auto it = r.by_name.find(name);
    return it == r.by_name.end() ? nullptr : it->second;
}

PluginBuilders::PluginBuilders(std::vector<PluginExtensionPtr> extensions) {
    built_.reserve(extensions.size());
    for (auto& ext : extensions) {
        void* b = ext->vt.make_builder(ext->self);
        if (!b)
            DFTRACER_UTILS_LOG_WARN(
                "Index extension '%s' built nothing: make_builder failed",
                ext->name.c_str());
        built_.push_back({std::move(ext), b, false, {}, {}});
    }
}

PluginBuilders::~PluginBuilders() {
    for (auto& b : built_)
        if (b.builder) b.ext->vt.destroy_builder(b.builder);
}

void PluginBuilders::end_granule() {
    if (!granule_) return;
    for (auto& b : built_) {
        if (!b.builder) continue;
        Out out{{}, b.ext->max_payload(), false};
        const ::dftu_index_out sink{&out, append_out};
        const int rc = b.ext->vt.finish_granule(b.builder, *granule_, &sink);
        if (rc == 0 && !b.chunk_failed && !out.over && !out.bytes.empty())
            b.granules.emplace_back(*granule_, std::move(out.bytes));
        b.chunk_failed = false;
    }
    granule_.reset();
}

void PluginBuilders::step(std::uint64_t granule,
                          std::span<const std::string_view> lines) {
    if (granule_ && *granule_ != granule) end_granule();
    granule_ = granule;
    lines_.clear();
    lines_.reserve(lines.size());
    for (std::string_view l : lines)
        lines_.push_back({l.data(), l.size(), nullptr, nullptr});
    for (auto& b : built_)
        if (b.builder && !b.chunk_failed &&
            b.ext->vt.step(b.builder, lines_.data(), lines_.size()) != 0)
            b.chunk_failed = true;
}

void PluginBuilders::finish() {
    end_granule();
    for (auto& b : built_) {
        if (!b.builder || !b.ext->vt.finish_file) continue;
        Out out{{}, b.ext->max_payload(), false};
        const ::dftu_index_out sink{&out, append_out};
        if (b.ext->vt.finish_file(b.builder, &sink) == 0 && !out.over &&
            !out.bytes.empty())
            b.file = std::move(out.bytes);
    }
}

void PluginBuilders::write(store::IndexWrite& w, int file_id) {
    namespace records = store::records;
    for (auto& b : built_) {
        const std::string& name = b.ext->name;
        records::clear_plugin(w, file_id, name);
        if (!b.builder) {
            records::put_plugin_manifest(w, file_id, name, b.ext->vt.version,
                                         b.ext->vt.params_hash,
                                         store::layout::ExtStatus::FAILED);
            continue;
        }
        records::put_path(w, store::IndexExtension::PLUGIN, file_id, name);
        for (const auto& [granule, bytes] : b.granules)
            records::put_path_granule(w, store::IndexExtension::PLUGIN, file_id,
                                      name, granule, bytes);
        if (b.file)
            records::put_path_file(w, store::IndexExtension::PLUGIN, file_id,
                                   name, *b.file);
        records::put_plugin_manifest(w, file_id, name, b.ext->vt.version,
                                     b.ext->vt.params_hash);
    }
}

}  // namespace dftracer::utils::index::extensions
