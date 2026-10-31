#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/hash/hex64.h>
#include <dftracer/utils/index/cache/cache_store.h>
#include <dftracer/utils/index/store/store_backend.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/store/snapshot.h>
#include <dftracer/utils/store/writer.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>

namespace dftracer::utils::index::cache {

namespace kv = dftracer::utils::store;

namespace {

constexpr std::uint64_t DEFAULT_CACHE_MAX_BYTES = std::uint64_t{2} << 30;
constexpr std::string_view DATA_NAMESPACE = "cache";
constexpr std::string_view USAGE_NAMESPACE = "cache.usage";
constexpr std::uint32_t NAMESPACE_VERSION = 1;
constexpr std::string_view USAGE_KEY = "u";

kv::NamespaceSpec spec_of(std::string_view name) {
    kv::NamespaceSpec spec;
    spec.name.assign(name.data(), name.size());
    spec.format_version = NAMESPACE_VERSION;
    return spec;
}

const kv::NamespaceSpec& data_spec() {
    static const kv::NamespaceSpec spec = spec_of(DATA_NAMESPACE);
    return spec;
}

const kv::NamespaceSpec& usage_spec() {
    static const kv::NamespaceSpec spec = spec_of(USAGE_NAMESPACE);
    return spec;
}

kv::ScopeKey scope_of(std::uint64_t sig) {
    char hex[dstd::hash::HEX64_DIGITS];
    dstd::hash::format_hex64(sig, hex);
    return kv::ScopeKey::file(dstd::StringView(hex, sizeof(hex)));
}

std::optional<std::uint64_t> sig_of(const kv::ScopeKey& scope) {
    if (scope.kind != kv::ScopeKind::File || scope.paths.size() != 1)
        return std::nullopt;
    const dstd::String& path = scope.paths[0];
    return dstd::hash::parse_hex64(std::string_view(path.data(), path.size()));
}

dstd::ByteView view_of(std::string_view s) {
    return dstd::ByteView(s.data(), s.size());
}

std::string_view text_of(dstd::ByteView b) {
    return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
}

std::uint64_t u64_at(dstd::ByteView b, std::size_t at) {
    return dstd::read_u64_be(reinterpret_cast<const std::uint8_t*>(b.data()) +
                             at);
}

[[noreturn]] void throw_io(std::string_view what, const dstd::Error& error) {
    const dstd::String text = error.format();
    throw DFTUtilsException(
        ErrorCode::Io,
        std::string(what) + ": " + std::string(text.data(), text.size()));
}

void check(std::string_view what,
           const dstd::Expected<void, dstd::Error>& result) {
    if (!result) throw_io(what, result.error());
}

std::string usage_value(std::uint64_t bytes, std::uint64_t last_used) {
    std::string v;
    dstd::append_u64_be(v, bytes);
    dstd::append_u64_be(v, last_used);
    return v;
}

std::uint64_t now_micros() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

/// Kept open so each folder gets one data file per process. They close before
/// the process runtime stops, since their writers wait on it.
struct CacheRoots {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<store::StoreRoot>> roots;

    ~CacheRoots() {
        for (auto& entry : roots) {
            try {
                entry.second->close();
            } catch (const std::exception& error) {
                DFTRACER_UTILS_LOG_WARN("cache store: close %s: %s",
                                        entry.first.c_str(), error.what());
            }
        }
    }
};

CacheRoots& cache_roots() {
    static CacheRoots value;
    return value;
}

}  // namespace

std::uint64_t cache_max_bytes() {
    const auto text = dftracer::utils::Env::get<std::string_view>(
        "DFTRACER_UTILS_CACHE_MAX_BYTES");
    if (!text) return DEFAULT_CACHE_MAX_BYTES;
    return dftracer::utils::parse_bytes(*text).value_or(
        DEFAULT_CACHE_MAX_BYTES);
}

bool CacheStore::exists(const std::string& path) {
    std::error_code missing;
    return std::filesystem::exists(std::filesystem::path(path) / "store",
                                   missing);
}

std::shared_ptr<CacheStore> CacheStore::open(const std::string& path,
                                             Mode mode) {
    try {
        const bool writable = mode == Mode::ReadWrite;
        if (writable) {
            std::filesystem::create_directories(path);
        } else if (!exists(path)) {
            return nullptr;
        }
        std::shared_ptr<store::StoreRoot> root;
        {
            CacheRoots& all = cache_roots();
            std::lock_guard<std::mutex> lock(all.mutex);
            auto& held = all.roots[path];
            // A folder removed since this process wrote it gets a new root,
            // so later writes do not land in a data file that is gone.
            if (held && !exists(path)) {
                try {
                    held->close();
                } catch (const std::exception& error) {
                    DFTRACER_UTILS_LOG_WARN(
                        "cache store: close the removed %s: %s", path.c_str(),
                        error.what());
                }
                held.reset();
            }
            if (!held) held = store::StoreRoot::get_or_open(path);
            root = held;
        }
        return std::make_shared<CacheStore>(std::move(root), writable);
    } catch (const std::exception&) {
        return nullptr;
    }
}

CacheStore::CacheStore(std::shared_ptr<store::StoreRoot> root, bool writable)
    : root_(std::move(root)), writable_(writable) {}

std::optional<std::string> CacheStore::get(std::uint64_t sig,
                                           std::string_view key) const {
    const auto held = root_->snapshot();
    if (!held) return std::nullopt;
    auto found = held->get(data_spec(), scope_of(sig), view_of(key));
    if (!found) throw_io("cache read", found.error());
    if (!*found) return std::nullopt;
    return std::string(text_of((**found).bytes()));
}

void CacheStore::scan(
    std::uint64_t sig, std::string_view prefix,
    const std::function<void(std::string_view, std::string_view)>& fn) const {
    const auto held = root_->snapshot();
    if (!held) return;
    auto cursor = held->scan(data_spec(), scope_of(sig), view_of(prefix));
    for (; !cursor.at_end(); cursor.next()) {
        auto value = cursor.value();
        if (!value) throw_io("cache scan", value.error());
        fn(text_of(cursor.key()), text_of(*value));
    }
    if (cursor.error()) throw_io("cache scan", *cursor.error());
}

void CacheStore::scan_all(
    std::string_view begin, std::string_view end,
    const std::function<bool(std::uint64_t, std::string_view,
                             std::string_view)>& fn) const {
    const auto held = root_->snapshot();
    if (!held) return;
    auto cursor = held->scan_all(data_spec(), view_of(begin), view_of(end));
    for (; !cursor.at_end(); cursor.next()) {
        const auto sig = sig_of(cursor.scope());
        if (!sig) continue;
        auto value = cursor.value();
        if (!value) throw_io("cache scan", value.error());
        if (!fn(*sig, text_of(cursor.key()), text_of(*value))) return;
    }
    if (cursor.error()) throw_io("cache scan", *cursor.error());
}

std::vector<CacheStore::Usage> CacheStore::usage() const {
    std::vector<Usage> out;
    const auto held = root_->snapshot();
    if (!held) return out;
    auto cursor =
        held->scan_all(usage_spec(), dstd::ByteView{}, dstd::ByteView{});
    for (; !cursor.at_end(); cursor.next()) {
        const auto sig = sig_of(cursor.scope());
        if (!sig || text_of(cursor.key()) != USAGE_KEY) continue;
        auto value = cursor.value();
        if (!value) throw_io("cache usage scan", value.error());
        if (value->size() != USAGE_BYTES) continue;
        out.push_back({*sig, u64_at(*value, 0), u64_at(*value, 8)});
    }
    if (cursor.error()) throw_io("cache usage scan", *cursor.error());
    return out;
}

void CacheStore::put(
    std::uint64_t sig,
    const std::vector<std::pair<std::string, std::string>>& entries,
    std::uint64_t bytes) {
    if (!writable_)
        throw DFTUtilsException(ErrorCode::InvalidArgument,
                                "cache write: the store is read-only");
    {
        store::StoreRoot::WriterUse use(*root_);
        memory::Scope in_pool(root_->pool());
        kv::engine::Writer& writer = root_->writer();
        const kv::ScopeKey scope = scope_of(sig);
        kv::Batch data = writer.batch(
            root_->namespace_id(std::string(DATA_NAMESPACE), NAMESPACE_VERSION),
            scope, 0);
        for (const auto& [key, value] : entries)
            data.put_ref(view_of(key), view_of(value));
        check("cache write", root_->commit(writer, std::move(data)));
        const std::string usage = usage_value(bytes, now_micros());
        kv::Batch record =
            writer.batch(root_->namespace_id(std::string(USAGE_NAMESPACE),
                                             NAMESPACE_VERSION),
                         scope, 0);
        record.put_ref(view_of(USAGE_KEY), view_of(usage));
        check("cache usage write", root_->commit(writer, std::move(record)));
        check("cache flush", root_->run(writer.flush()));
        root_->bump_version();
    }
    evict(sig);
}

void CacheStore::touch(std::uint64_t sig) {
    if (!writable_) return;
    const auto held = root_->snapshot();
    if (!held) return;
    const kv::ScopeKey scope = scope_of(sig);
    auto found = held->get(usage_spec(), scope, view_of(USAGE_KEY));
    if (!found) throw_io("cache usage read", found.error());
    if (!*found || (**found).bytes().size() != USAGE_BYTES) return;
    const std::string usage =
        usage_value(u64_at((**found).bytes(), 0), now_micros());
    store::StoreRoot::WriterUse use(*root_);
    memory::Scope in_pool(root_->pool());
    kv::engine::Writer& writer = root_->writer();
    kv::Batch record = writer.batch(
        root_->namespace_id(std::string(USAGE_NAMESPACE), NAMESPACE_VERSION),
        scope, 0);
    record.put_ref(view_of(USAGE_KEY), view_of(usage));
    check("cache usage write", root_->commit(writer, std::move(record)));
}

void CacheStore::evict(std::uint64_t keep) {
    std::vector<Usage> entries = usage();
    std::uint64_t total = 0;
    for (const Usage& e : entries) total += e.bytes;
    const std::uint64_t budget = cache_max_bytes();
    if (total <= budget) return;
    std::sort(entries.begin(), entries.end(),
              [](const Usage& a, const Usage& b) {
                  return a.last_used < b.last_used;
              });
    store::StoreRoot::WriterUse use(*root_);
    memory::Scope in_pool(root_->pool());
    kv::engine::Writer& writer = root_->writer();
    const kv::engine::NamespaceId data_id =
        root_->namespace_id(std::string(DATA_NAMESPACE), NAMESPACE_VERSION);
    const kv::engine::NamespaceId usage_id =
        root_->namespace_id(std::string(USAGE_NAMESPACE), NAMESPACE_VERSION);
    auto remove = [&](const Usage& e) {
        const kv::ScopeKey scope = scope_of(e.sig);
        check("cache eviction", writer.drop(data_id, scope));
        check("cache eviction", writer.drop(usage_id, scope));
        total -= e.bytes;
    };
    for (const Usage& e : entries) {
        if (total <= budget) break;
        if (e.sig != keep) remove(e);
    }
    if (total > budget)
        for (const Usage& e : entries)
            if (e.sig == keep) remove(e);
    check("cache flush", root_->run(writer.flush()));
    root_->bump_version();
}

}  // namespace dftracer::utils::index::cache
