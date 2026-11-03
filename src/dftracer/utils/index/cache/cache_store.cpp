// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash/hex64.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/cache/cache_store.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/runtime/runtime.h>

#include <algorithm>
#include <chrono>
#include <mutex>

namespace dftracer::utils::index::cache {

namespace {

constexpr std::uint64_t DEFAULT_CACHE_MAX_BYTES = std::uint64_t{2} << 30;
constexpr dstd::StringView DATA_NAMESPACE = "cache";
constexpr dstd::StringView USAGE_NAMESPACE = "cache.usage";
constexpr std::uint32_t NAMESPACE_VERSION = 1;
constexpr dstd::StringView USAGE_KEY = "u";

store::NamespaceSpec spec_of(dstd::StringView name) {
    store::NamespaceSpec spec;
    spec.name.assign(name.data(), name.size());
    spec.format_version = NAMESPACE_VERSION;
    return spec;
}

const store::NamespaceSpec& data_spec() {
    static const store::NamespaceSpec spec = spec_of(DATA_NAMESPACE);
    return spec;
}

const store::NamespaceSpec& usage_spec() {
    static const store::NamespaceSpec spec = spec_of(USAGE_NAMESPACE);
    return spec;
}

store::ScopeKey scope_of(std::uint64_t sig) {
    char hex[dstd::hash::HEX64_DIGITS];
    dstd::hash::format_hex64(sig, hex);
    return store::ScopeKey::file(dstd::StringView(hex, sizeof(hex)));
}

dstd::Optional<std::uint64_t> sig_of(const store::ScopeKey& scope) {
    if (scope.kind != store::ScopeKind::File || scope.paths.size() != 1)
        return dstd::nullopt;
    const dstd::String& path = scope.paths[0];
    return dstd::hash::parse_hex64(dstd::StringView(path.data(), path.size()));
}

dstd::ByteView view_of(dstd::StringView s) {
    return dstd::ByteView(s.data(), s.size());
}

dstd::StringView text_of(dstd::ByteView b) {
    return dstd::StringView(reinterpret_cast<const char*>(b.data()), b.size());
}

std::uint64_t u64_at(dstd::ByteView b, std::size_t at) {
    return dstd::read_u64_be(reinterpret_cast<const std::uint8_t*>(b.data()) +
                             at);
}

[[noreturn]] void throw_io(dstd::StringView what, const dstd::Error& error) {
    const dstd::String text = error.format();
    throw common::DFTUtilsException(
        common::ErrorCode::Io,
        dstd::String(what) + ": " + dstd::String(text.data(), text.size()));
}

void check(dstd::StringView what,
           const dstd::Expected<void, dstd::Error>& result) {
    if (!result) throw_io(what, result.error());
}

dstd::String usage_value(std::uint64_t bytes, std::uint64_t last_used) {
    dstd::String v;
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

dstd::Expected<void, dstd::Error> wait(
    coro::CoroTask<dstd::Expected<void, dstd::Error>> task) {
    const dstd::SharedPtr<Runtime> runtime = default_runtime_shared();
    const bool reentrant = coro::Executor::current() == runtime->executor();
    coro::BlockingRegion block(reentrant ? runtime->executor() : nullptr);
    return runtime->submit(std::move(task)).get();
}

/// Kept open so each folder gets one data file per process. The handles go
/// when shutdown clears them, before the store closes the writers they hold
/// and before the process runtime stops.
struct CacheRoots {
    using Held = dstd::Map<dstd::String, store::Store>;

    std::mutex mutex;
    Held roots;

    void clear() {
        Held dropped;
        std::lock_guard<std::mutex> lock(mutex);
        dropped.swap(roots);
    }
};

CacheRoots& cache_roots() {
    CacheRoots& roots = dstd::Singleton<CacheRoots>::get();
    [[maybe_unused]] static const bool hooked = [] {
        store::Store::register_shutdown_close();
        dstd::add_shutdown_hook(dstd::ShutdownStage::CloseStores, [] {
            dstd::Singleton<CacheRoots>::get().clear();
        });
        return true;
    }();
    return roots;
}

}  // namespace

void clear_cache_roots() { cache_roots().clear(); }

std::uint64_t cache_max_bytes() {
    const auto text = dftracer::utils::common::Env::get<dstd::StringView>(
        "DFTRACER_UTILS_CACHE_MAX_BYTES");
    if (!text) return DEFAULT_CACHE_MAX_BYTES;
    return dftracer::utils::common::parse_bytes(*text).value_or(
        DEFAULT_CACHE_MAX_BYTES);
}

bool CacheStore::exists(dstd::StringView path) {
    const auto found = dstd::fs::exists(dstd::PathView(store_dir(path)));
    return found && *found;
}

dstd::SharedPtr<CacheStore> CacheStore::open(dstd::StringView path, Mode mode) {
    try {
        const bool writable = mode == Mode::ReadWrite;
        if (writable) {
            common::unwrap(dstd::fs::create_directories(dstd::PathView(path)));
        } else if (!exists(path)) {
            return nullptr;
        }
        dstd::Optional<store::Store> store;
        {
            CacheRoots& all = cache_roots();
            std::lock_guard<std::mutex> lock(all.mutex);
            auto held = all.roots.find(path);
            // A folder removed since this process wrote it gets a new writer,
            // so later writes do not land in a data file that is gone.
            if (held != all.roots.end() && !exists(path)) {
                const auto closed = wait(held->second.close());
                if (!closed) {
                    const dstd::String text = closed.error().format();
                    DFTRACER_UTILS_LOG_WARN(
                        "cache store: close the removed %.*s: %.*s",
                        static_cast<int>(path.size()), path.data(),
                        static_cast<int>(text.size()), text.data());
                }
                all.roots.erase(held);
                held = all.roots.end();
            }
            if (held == all.roots.end()) {
                const dstd::String dir = store_dir(path);
                auto opened = store::Store::open(dstd::StringView(dir));
                if (!opened) return nullptr;
                held = all.roots.emplace(path, std::move(*opened)).first;
            }
            store = held->second;
        }
        auto data = store->ns(data_spec());
        auto usage = store->ns(usage_spec());
        if (!data || !usage) return nullptr;
        return dstd::make_shared<CacheStore>(std::move(*store), *data, *usage,
                                             writable);
    } catch (const std::exception&) {
        return nullptr;
    }
}

CacheStore::CacheStore(store::Store store, store::Namespace data,
                       store::Namespace usage, bool writable)
    : store_(std::move(store)),
      data_(std::move(data)),
      usage_(std::move(usage)),
      writable_(writable) {}

store::Snapshot CacheStore::snapshot() const {
    auto taken = store_.snapshot();
    if (!taken) throw_io("cache snapshot", taken.error());
    return std::move(*taken);
}

dstd::Optional<dstd::String> CacheStore::get(std::uint64_t sig,
                                             dstd::StringView key) const {
    const store::Snapshot held = snapshot();
    auto found = held.get(data_, scope_of(sig), view_of(key));
    if (!found) throw_io("cache read", found.error());
    if (!*found) return dstd::nullopt;
    return dstd::String(text_of((**found).bytes()));
}

void CacheStore::scan(
    std::uint64_t sig, dstd::StringView prefix,
    const dstd::Function<void(dstd::StringView, dstd::StringView)>& fn) const {
    const store::Snapshot held = snapshot();
    auto cursor = held.scan(data_, scope_of(sig), view_of(prefix));
    for (; !cursor.at_end(); cursor.next()) {
        auto value = cursor.value();
        if (!value) throw_io("cache scan", value.error());
        fn(text_of(cursor.key()), text_of(*value));
    }
    if (cursor.error()) throw_io("cache scan", *cursor.error());
}

void CacheStore::scan_all(
    dstd::StringView begin, dstd::StringView end,
    const dstd::Function<bool(std::uint64_t, dstd::StringView,
                              dstd::StringView)>& fn) const {
    const store::Snapshot held = snapshot();
    auto cursor = held.scan_all(data_, view_of(begin), view_of(end));
    for (; !cursor.at_end(); cursor.next()) {
        const auto sig = sig_of(cursor.scope());
        if (!sig) continue;
        auto value = cursor.value();
        if (!value) throw_io("cache scan", value.error());
        if (!fn(*sig, text_of(cursor.key()), text_of(*value))) return;
    }
    if (cursor.error()) throw_io("cache scan", *cursor.error());
}

dstd::Vector<CacheStore::Usage> CacheStore::usage() const {
    dstd::Vector<Usage> out;
    const store::Snapshot held = snapshot();
    auto cursor = held.scan_all(usage_, dstd::ByteView{}, dstd::ByteView{});
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
    const dstd::Vector<dstd::Pair<dstd::String, dstd::String>>& entries,
    std::uint64_t bytes) {
    if (!writable_)
        throw common::DFTUtilsException(common::ErrorCode::InvalidArgument,
                                        "cache write: the store is read-only");
    const store::ScopeKey scope = scope_of(sig);
    store::Batch data = store_.batch(data_, scope);
    for (const auto& [key, value] : entries)
        data.put_ref(view_of(key), view_of(value));
    check("cache write", wait(store_.commit(std::move(data))));
    const dstd::String usage = usage_value(bytes, now_micros());
    store::Batch record = store_.batch(usage_, scope);
    record.put_ref(view_of(USAGE_KEY), view_of(usage));
    check("cache usage write", wait(store_.commit(std::move(record))));
    check("cache flush", wait(store_.flush()));
    evict(sig);
}

void CacheStore::touch(std::uint64_t sig) {
    if (!writable_) return;
    const store::Snapshot held = snapshot();
    const store::ScopeKey scope = scope_of(sig);
    auto found = held.get(usage_, scope, view_of(USAGE_KEY));
    if (!found) throw_io("cache usage read", found.error());
    if (!*found || (**found).bytes().size() != USAGE_BYTES) return;
    const dstd::String usage =
        usage_value(u64_at((**found).bytes(), 0), now_micros());
    store::Batch record = store_.batch(usage_, scope);
    record.put_ref(view_of(USAGE_KEY), view_of(usage));
    check("cache usage write", wait(store_.commit(std::move(record))));
}

void CacheStore::evict(std::uint64_t keep) {
    dstd::Vector<Usage> entries = usage();
    std::uint64_t total = 0;
    for (const Usage& e : entries) total += e.bytes;
    const std::uint64_t budget = cache_max_bytes();
    if (total <= budget) return;
    std::sort(entries.begin(), entries.end(),
              [](const Usage& a, const Usage& b) {
                  return a.last_used < b.last_used;
              });
    auto remove = [&](const Usage& e) {
        const store::ScopeKey scope = scope_of(e.sig);
        check("cache eviction", store_.drop(data_, scope));
        check("cache eviction", store_.drop(usage_, scope));
        total -= e.bytes;
    };
    for (const Usage& e : entries) {
        if (total <= budget) break;
        if (e.sig != keep) remove(e);
    }
    if (total > budget)
        for (const Usage& e : entries)
            if (e.sig == keep) remove(e);
    check("cache flush", wait(store_.flush()));
}

}  // namespace dftracer::utils::index::cache
