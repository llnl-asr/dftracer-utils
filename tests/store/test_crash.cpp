// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/data_file.h>
#include <dftracer/utils/store/extent_codec.h>
#include <dftracer/utils/store/fold.h>
#include <dftracer/utils/store/io_fault.h>
#include <dftracer/utils/store/record.h>
#include <dftracer/utils/store/snapshot.h>
#include <dftracer/utils/store/writer.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils;

namespace {

namespace dstd = dftracer::utils::dstd;
namespace store = dftracer::utils::store;
namespace io_fault = store::engine::io_fault;

using dftracer::utils::Runtime;
using dftracer::utils::TaskHandle;
using dftracer::utils::dstd::ByteView;

constexpr std::uint32_t EXTENT_BYTES = 4 * 1024;
constexpr std::uint64_t COMMIT_BYTES = 4 * 1024;
constexpr std::chrono::milliseconds COMMIT_INTERVAL{20};
constexpr std::uint64_t EXTENT_QUOTA_BYTES = 4ULL * EXTENT_BYTES;
constexpr int RUNTIME_THREADS = 8;
constexpr int COROUTINES = 16;
constexpr int ROUNDS = 20;
/// A durable batch in every fourth round: the rounds between two durable ones
/// are one unsynced window with several whole commits in it.
constexpr int DURABLE_EVERY = 4;
constexpr int TAIL_COMMITS = 45;
constexpr std::size_t RAW_KEYS = 1;
constexpr std::size_t RAW_VALUE_BYTES = 1000;
/// A raw value longer than the format's big-value bound: its segment holds a
/// reference and the bytes go to a range of their own, so a lost big value is a
/// loss the reader has to answer for without its extent.
constexpr std::size_t BIG_VALUE_LENGTH = store::engine::BIG_VALUE_BYTES + 1;
constexpr std::size_t FOLD_KEYS = 1;
constexpr int RETRY_ATTEMPTS = 4000;
constexpr std::chrono::seconds TASK_LIMIT{120};
/// Images per kill point of the lost and the reordered variants.
constexpr std::size_t LOST_SEEDS = 3;
constexpr std::size_t REORDER_SEEDS = 2;
/// A torn write keeps a prefix: a whole number of 512-byte units for an extent,
/// a record or a tail, and any prefix up to the end of the tail fields for a
/// header, the one write that changes bytes of an earlier write in place.
constexpr std::uint64_t TEAR_UNIT = 512;
/// The header crc field: bytes [12, 16) of a data file header.
constexpr std::uint64_t HEADER_CRC_BYTES = 16;
/// Reference R_m carries the header of its m-th commit, so an extent of the log
/// that no applied record names cannot change a dump.
constexpr std::size_t FAILURE_REPORTS = 4;

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr bool SANITIZED = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr bool SANITIZED = true;
#else
constexpr bool SANITIZED = false;
#endif
#else
constexpr bool SANITIZED = false;
#endif

ByteView view_of(const std::string& bytes) {
    return {bytes.data(), bytes.size()};
}

dstd::String dstring(const std::string& value) {
    return dstd::String(value.data(), value.size());
}

std::string to_std(ByteView bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
}

std::string to_std(const dstd::String& value) {
    return std::string(value.data(), value.size());
}

std::string i64_bytes(std::int64_t value) {
    std::string bytes(8, '\0');
    dftracer::utils::dstd::write_u64_le(
        reinterpret_cast<std::uint8_t*>(bytes.data()),
        static_cast<std::uint64_t>(value));
    return bytes;
}

std::string padded(const std::string& head, std::size_t length) {
    std::string out = head;
    out.resize(length, 'x');
    return out;
}

store::NamespaceSpec put_spec(const std::string& name, std::uint32_t version) {
    store::NamespaceSpec spec;
    spec.name = dstring(name);
    spec.format_version = version;
    spec.fold = store::FoldKind::None;
    return spec;
}

store::NamespaceSpec durable_spec(const std::string& name) {
    store::NamespaceSpec spec = put_spec(name, 0);
    spec.durable = true;
    return spec;
}

store::NamespaceSpec fold_spec(const std::string& name, store::FoldKind kind) {
    store::NamespaceSpec spec;
    spec.name = dstring(name);
    spec.format_version = 0;
    spec.fold = kind;
    return spec;
}

store::ScopeKey file_scope(const std::string& path) {
    return store::ScopeKey::file(dstring(path));
}

Runtime& store_runtime() {
    static Runtime runtime(RUNTIME_THREADS);
    return runtime;
}

void write_bytes(const std::string& path, const std::string& bytes) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_bytes(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::uint64_t seed_of(std::size_t kill, std::size_t variant,
                      std::size_t index) {
    std::uint64_t seed = dftracer::utils::dstd::hash::splitmix64(
        dftracer::utils::dstd::hash::GOLDEN_RATIO ^ kill);
    seed = dftracer::utils::dstd::hash::splitmix64(seed ^ variant);
    return dftracer::utils::dstd::hash::splitmix64(seed ^ index);
}

template <class Handle>
void await_handle(Handle& handle, std::chrono::seconds limit) {
    REQUIRE(handle.future.wait_for(limit) == std::future_status::ready);
    handle.wait();
}

dstd::Expected<void, dstd::Error> close_writer(store::engine::Writer& writer) {
    return store_runtime().submit(writer.close()).get();
}

dstd::Expected<void, dstd::Error> flush_writer(store::engine::Writer& writer) {
    return store_runtime().submit(writer.flush()).get();
}

bool is_busy(const dstd::Error& error) {
    return error.is_code(store::engine::WriterErrc::Busy);
}

struct Namespaces {
    store::engine::NamespaceId sums;
    store::engine::NamespaceId durable;
    store::engine::NamespaceId raw;
    store::engine::NamespaceId big;
};

std::string scope_path(int index) {
    return "/data/crash-" + std::to_string(index) + ".dft";
}

std::string fail_path(int index) {
    return "/data/crash-failed-" + std::to_string(index) + ".dft";
}

std::string big_path() { return "/data/crash-big.dft"; }

std::string fold_key(int index, std::size_t k) {
    return "c" + std::to_string(index) + "-f" + std::to_string(k);
}

std::string raw_key(int index, int round, std::size_t k) {
    return "c" + std::to_string(index) + "-r" + std::to_string(round) + "-k" +
           std::to_string(k);
}

std::string raw_value(int index, int round, std::size_t k) {
    return "v" + std::to_string(index) + "-" + std::to_string(round) + "-" +
           std::to_string(k);
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> commit_retrying(
    store::engine::Writer& writer, store::Batch batch, int attempts) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        auto committed = writer.commit(std::move(batch));
        if (committed.has_value() || !is_busy(committed.error())) {
            co_return committed;
        }
        co_await writer.space();
    }
    co_return dstd::Unexpected<dstd::Error>(dstd::Error(
        store::engine::WriterErrc::Busy, "the writer made no progress"));
}

coro::CoroTask<void> write_coroutine(store::engine::Writer& writer,
                                     Namespaces ids, std::atomic<int>& errors,
                                     int index, int first_round,
                                     int last_round) {
    const store::ScopeKey scope = file_scope(scope_path(index));
    for (int round = first_round; round < last_round; ++round) {
        store::Batch sums = writer.batch(ids.sums, scope, 0);
        for (std::size_t k = 0; k < FOLD_KEYS; ++k) {
            const std::string key = fold_key(index, k);
            const std::string value =
                i64_bytes(static_cast<std::int64_t>(index) + 1);
            sums.fold(view_of(key), view_of(value));
        }
        auto sums_done =
            co_await commit_retrying(writer, std::move(sums), RETRY_ATTEMPTS);
        if (!sums_done.has_value()) {
            errors.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }

        store::Batch rows = writer.batch(ids.raw, scope, 0);
        for (std::size_t k = 0; k < RAW_KEYS; ++k) {
            const std::string key = raw_key(index, round, k);
            const std::string value =
                padded(raw_value(index, round, k), RAW_VALUE_BYTES);
            rows.put(view_of(key), view_of(value));
        }
        auto rows_done =
            co_await commit_retrying(writer, std::move(rows), RETRY_ATTEMPTS);
        if (!rows_done.has_value()) {
            errors.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }

        if (index == 0 && round % DURABLE_EVERY == 0) {
            store::Batch durable = writer.batch(ids.durable, scope, 0);
            for (std::size_t k = 0; k < RAW_KEYS; ++k) {
                const std::string key = "d" + raw_key(index, round, k);
                const std::string value =
                    padded(raw_value(index, round, k), RAW_VALUE_BYTES);
                durable.put(view_of(key), view_of(value));
            }
            auto durable_done = co_await commit_retrying(
                writer, std::move(durable), RETRY_ATTEMPTS);
            if (!durable_done.has_value()) {
                errors.fetch_add(1, std::memory_order_relaxed);
                co_return;
            }
        }

        if (index == 0 && round == 1) {
            // One big value, in a round with no durable commit, so the writes
            // that carry it fall in an unsynced window of their own.
            store::Batch big = writer.batch(ids.big, file_scope(big_path()), 0);
            const std::string value = padded("big", BIG_VALUE_LENGTH);
            big.put(view_of(std::string("big-key")), view_of(value));
            auto big_done = co_await commit_retrying(writer, std::move(big),
                                                     RETRY_ATTEMPTS);
            if (!big_done.has_value()) {
                errors.fetch_add(1, std::memory_order_relaxed);
                co_return;
            }
        }
        if (round != first_round) continue;
        if (index == 0) {
            auto dropped =
                writer.drop(ids.raw, scope, view_of(raw_key(index, round, 0)),
                            view_of(raw_key(index, round, 1)));
            if (!dropped.has_value()) {
                errors.fetch_add(1, std::memory_order_relaxed);
                co_return;
            }
        } else if (index == 1) {
            auto failed = writer.fail(ids.raw, file_scope(fail_path(index)), 0);
            if (!failed.has_value()) {
                errors.fetch_add(1, std::memory_order_relaxed);
                co_return;
            }
        }
    }
    co_return;
}

/// A canonical dump: every row of every namespace the workload wrote, and the
/// state and generation of every scope the snapshot knows, both in std::map
/// order and length-prefixed, so two dumps compare as text.
struct Dump {
    std::map<std::string, std::string> rows;
    std::map<std::string, std::string> row_group;
    std::map<std::string, std::string> scopes;

    std::string text() const { return text_without({}); }

    /// The dump a reader makes when every copy of every group in `blank` reads
    /// Bad: no rows of the group, and a Bad state for it. A group the snapshot
    /// does not know stays unknown.
    std::string text_without(const std::set<std::string>& blank) const {
        std::string out;
        std::size_t kept = 0;
        for (const auto& entry : rows) {
            if (blank.count(row_group.at(entry.first)) == 0) ++kept;
        }
        put(out, "rows=" + std::to_string(kept));
        for (const auto& entry : rows) {
            if (blank.count(row_group.at(entry.first)) != 0) continue;
            put(out, entry.first);
            put(out, entry.second);
        }
        put(out, "scopes=" + std::to_string(scopes.size()));
        for (const auto& entry : scopes) {
            put(out, entry.first);
            if (blank.count(entry.first) == 0) {
                put(out, entry.second);
                continue;
            }
            std::string state = entry.second;
            const std::size_t slash = state.find('/');
            state.resize(slash == std::string::npos ? 0 : slash + 1);
            state += std::to_string(static_cast<int>(store::ScopeState::Bad));
            put(out, state);
        }
        return out;
    }

    static void put(std::string& out, const std::string& field) {
        out += std::to_string(field.size());
        out += ':';
        out.append(field);
    }
};

std::string scope_text(const store::ScopeKey& scope) {
    std::string out = std::to_string(static_cast<int>(scope.kind));
    out += '/';
    out += std::to_string(scope.paths.size());
    for (const dstd::String& path : scope.paths) {
        out += '/';
        out += std::to_string(path.size());
        out += ':';
        out.append(path.data(), path.size());
    }
    return out;
}

std::string group_text(const std::string& space, const store::ScopeKey& scope) {
    std::string key = space;
    Dump::put(key, scope_text(scope));
    return key;
}

/// The dump of one image, or why the reader refused it: a scan of a copy whose
/// extent or big value is gone may fail instead of reading Bad.
struct Dumped {
    bool ok = false;
    std::string error;
    std::size_t skipped = 0;
    Dump dump;

    std::string text() const { return ok ? dump.text() : "error:" + error; }
};

Dumped dump_snapshot(const store::engine::Snapshot& snapshot,
                     const std::vector<store::NamespaceSpec>& specs) {
    Dumped out;
    out.skipped = snapshot.skipped().size();
    for (const store::NamespaceSpec& spec : specs) {
        store::engine::ScanCursor cursor = snapshot.scan_all(spec, {}, {});
        const std::string space = to_std(spec.name);
        while (!cursor.at_end()) {
            auto value = cursor.value();
            if (!value.has_value()) {
                out.error = "scan " +
                            std::to_string(
                                static_cast<int>(value.error().condition())) +
                            " of " + space;
                return out;
            }
            std::string group = group_text(space, cursor.scope());
            std::string key = group;
            Dump::put(key, to_std(cursor.key()));
            out.dump.rows[key] = to_std(*value);
            out.dump.row_group[key] = group;
            cursor.next();
        }
        if (cursor.error()) {
            out.error =
                "scan_all " +
                std::to_string(static_cast<int>(cursor.error()->condition())) +
                " of " + space;
            return out;
        }
    }
    auto generations = snapshot.generations();
    if (!generations) {
        out.error =
            "generations " +
            std::to_string(static_cast<int>(generations.error().condition()));
        return out;
    }
    for (const store::engine::ScopeGeneration& generation : *generations) {
        store::NamespaceSpec spec;
        spec.name = generation.namespace_name;
        spec.format_version = generation.format_version;
        std::string key =
            group_text(to_std(generation.namespace_name), generation.scope);
        std::string value = std::to_string(generation.generation);
        value += '/';
        value += std::to_string(
            static_cast<int>(snapshot.state(spec, generation.scope)));
        out.dump.scopes[key] = value;
    }
    out.ok = true;
    return out;
}

bool is_sync(const io_fault::Event& event) {
    return event.type == io_fault::Event::Type::Sync;
}

std::uint64_t whole_length(const io_fault::Event& event) {
    return is_sync(event) ? 0 : event.length;
}

/// The bytes one event wrote: an extent's range of the finished file, because a
/// extent is never rewritten, or the bytes the log copied.
const std::uint8_t* event_bytes(const io_fault::Event& event,
                                const std::string& final_bytes) {
    if (event.kind == io_fault::WriteKind::Extent) {
        return reinterpret_cast<const std::uint8_t*>(final_bytes.data()) +
               event.offset;
    }
    return event.bytes.empty() ? nullptr : event.bytes.data();
}

/// One write of an image: `keep` bytes of it, at its offset. `keep` is the
/// event's length except for a torn write and for a write that did not happen.
std::string render_image(
    const std::vector<std::pair<std::size_t, std::uint64_t>>& applied,
    const dstd::Vector<io_fault::Event>& log, const std::string& final_bytes) {
    std::uint64_t end = 0;
    for (const auto& one : applied) {
        end = std::max(end, log[one.first].offset + one.second);
    }
    std::string image(static_cast<std::size_t>(end), '\0');
    for (const auto& one : applied) {
        if (one.second == 0) continue;
        const io_fault::Event& event = log[one.first];
        const std::uint8_t* data = event_bytes(event, final_bytes);
        if (data == nullptr) continue;
        std::memcpy(image.data() + event.offset, data,
                    static_cast<std::size_t>(one.second));
    }
    return image;
}

/// The index one past the last sync below `k`: every write at or after it is
/// unsynced, and every write below it was issued before that sync.
std::size_t unsynced_from(const dstd::Vector<io_fault::Event>& log,
                          std::size_t k) {
    for (std::size_t i = k; i > 0; --i) {
        if (is_sync(log[i - 1])) return i;
    }
    return 0;
}

/// The end of the only bytes a header rewrite changes: its tail fields. A
/// prefix that covers them leaves the whole new header, crc included.
std::uint64_t header_tail_end(const io_fault::Event& event) {
    const std::uint64_t size = event.bytes.size();
    if (size < 48) return size;
    const std::uint8_t* data = event.bytes.data();
    const std::uint64_t host = dftracer::utils::dstd::read_u16_le(data + 44);
    const std::uint64_t label = dftracer::utils::dstd::read_u16_le(data + 46);
    const std::uint64_t at = 48 + host + label;
    return at + 16 <= size ? at + 16 : size;
}

std::uint64_t torn_prefix(const io_fault::Event& event, std::mt19937_64& rng) {
    if (event.kind == io_fault::WriteKind::Header) {
        const std::uint64_t end = header_tail_end(event);
        return end == 0 ? 0 : rng() % end;
    }
    const std::uint64_t units = whole_length(event) / TEAR_UNIT;
    return units == 0 ? 0 : TEAR_UNIT * (rng() % units);
}

std::size_t records_below(const std::vector<std::size_t>& record_at,
                          std::size_t limit) {
    return static_cast<std::size_t>(
        std::lower_bound(record_at.begin(), record_at.end(), limit) -
        record_at.begin());
}

std::size_t last_write_below(const dstd::Vector<io_fault::Event>& log,
                             std::size_t k) {
    for (std::size_t i = k; i > 0; --i) {
        if (!is_sync(log[i - 1])) return i - 1;
    }
    return k;
}

std::string log_text(const dstd::Vector<io_fault::Event>& log) {
    static const char* const KINDS[] = {"header", "record",  "big",
                                        "footer", "trailer", "extent"};
    std::string out;
    for (std::size_t i = 0; i < log.size(); ++i) {
        const io_fault::Event& event = log[i];
        out += std::to_string(i);
        out += ' ';
        if (is_sync(event)) {
            out += "sync";
        } else {
            out += KINDS[static_cast<std::size_t>(event.kind)];
            out += " offset=" + std::to_string(event.offset);
            out += " length=" + std::to_string(event.length);
        }
        out += '\n';
    }
    return out;
}

store::ScopeKey scope_key_of(const store::engine::ScopeItem& item) {
    store::ScopeKey key;
    key.kind = item.kind;
    key.paths.reserve(item.paths.size());
    for (const dstd::String& path : item.paths) {
        key.paths.push_back(path);
    }
    return key;
}

/// The log decoded once: the copies every record announces, the extent and big
/// writes of every sequence, the group of every copy, and which copies are
/// durable. An extent ref keeps the sequence of the commit that wrote it, so a
/// checkpoint that lists it again does not change which sequence a loss hits.
struct CrashModel {
    std::vector<std::map<std::uint32_t, std::uint64_t>> copies;
    std::map<std::uint64_t, std::vector<std::pair<std::size_t, std::uint32_t>>>
        extents;
    std::map<std::uint64_t, std::vector<std::pair<std::size_t, std::uint32_t>>>
        bigs;
    std::map<std::uint64_t, std::vector<std::size_t>> big_blocks;
    std::map<std::uint32_t, std::string> group_of_copy;
    std::set<std::uint32_t> durable_copies;
};

CrashModel build_model(const dstd::Vector<io_fault::Event>& log,
                       const std::vector<std::size_t>& record_at,
                       const std::string& final_bytes) {
    CrashModel model;
    std::map<std::uint64_t, std::size_t> extent_event;
    std::map<std::uint64_t, std::size_t> big_event;
    for (std::size_t i = 0; i < log.size(); ++i) {
        if (log[i].kind == io_fault::WriteKind::Extent) {
            extent_event[log[i].offset] = i;
        } else if (log[i].kind == io_fault::WriteKind::Big) {
            big_event[log[i].offset] = i;
        }
    }

    std::map<std::uint32_t, store::engine::NamespaceItem> namespaces;
    std::map<std::uint32_t, store::engine::ScopeItem> scopes;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>
        ids_of_copy;

    model.copies.resize(record_at.size());
    for (std::size_t c = 0; c < record_at.size(); ++c) {
        const io_fault::Event& event = log[record_at[c]];
        auto record = store::engine::decode_record(
            ByteView{event.bytes.data(), event.bytes.size()});
        REQUIRE(record.has_value());
        REQUIRE(record->sequence == c + 1);
        for (const store::engine::RecordItem& item : record->items) {
            if (const auto* ns =
                    std::get_if<store::engine::NamespaceItem>(&item)) {
                namespaces[ns->local_id] = *ns;
            } else if (const auto* scope =
                           std::get_if<store::engine::ScopeItem>(&item)) {
                scopes[scope->local_id] = *scope;
            } else if (const auto* copy =
                           std::get_if<store::engine::CopyItem>(&item)) {
                model.copies[c][copy->local_id] = copy->generation;
                ids_of_copy[copy->local_id] = {copy->namespace_id,
                                               copy->scope_id};
            } else if (const auto* extent =
                           std::get_if<store::engine::ExtentRefItem>(&item)) {
                if (extent->segments.empty()) {
                    const auto block = big_event.find(extent->offset);
                    REQUIRE(block != big_event.end());
                    model.big_blocks[extent->sequence].push_back(block->second);
                    continue;
                }
                const auto at = extent_event.find(extent->offset);
                REQUIRE(at != extent_event.end());
                REQUIRE(extent->offset + extent->length <= final_bytes.size());
                const auto* stored_at =
                    reinterpret_cast<const std::uint8_t*>(final_bytes.data()) +
                    extent->offset;
                dstd::Vector<std::uint8_t> raw(stored_at,
                                               stored_at + extent->length);
                REQUIRE(store::engine::decode_extent(raw).has_value());
                const ByteView bytes{raw.data(), raw.size()};
                for (const store::engine::ExtentSegment& segment :
                     extent->segments) {
                    model.extents[extent->sequence].emplace_back(
                        at->second, segment.copy_id);
                    auto view = store::engine::SegmentView::open(bytes.subspan(
                        segment.offset_in_extent, segment.length));
                    REQUIRE(view.has_value());
                    store::engine::SegmentView::Cursor cursor =
                        view->lower_bound(ByteView{});
                    while (!cursor.at_end()) {
                        const store::engine::SegmentView::Entry found =
                            cursor.entry();
                        if (found.is_big()) {
                            const auto big =
                                big_event.find(found.big_ref().offset);
                            REQUIRE(big != big_event.end());
                            model.bigs[extent->sequence].emplace_back(
                                big->second, segment.copy_id);
                        }
                        cursor.next();
                    }
                }
            }
        }
    }

    for (const auto& entry : ids_of_copy) {
        const auto ns = namespaces.find(entry.second.first);
        const auto scope = scopes.find(entry.second.second);
        if (ns == namespaces.end() || scope == scopes.end()) continue;
        model.group_of_copy[entry.first] =
            group_text(to_std(ns->second.name), scope_key_of(scope->second));
        if (ns->second.durable != 0) model.durable_copies.insert(entry.first);
    }
    return model;
}

/// The reference dumps one image of the lost variant must reach: R_m for its m,
/// with every copy of L_m reading Bad. L_m holds the copies whose extent or big
/// value the image lost and a record among the first m names.
struct LostOracle {
    std::vector<std::string> expected;
    std::vector<std::size_t> lost;
    std::size_t durable_lost = 0;
    std::size_t durable_kept = 0;
    std::size_t summary_records_lost = 0;
};

LostOracle lost_oracle(const CrashModel& model, const std::vector<Dump>& refs,
                       const std::vector<char>& dropped,
                       const std::vector<std::size_t>& record_at,
                       std::size_t lo, std::size_t hi) {
    LostOracle oracle;
    std::set<std::uint32_t> lost_copies;
    std::map<std::string, std::pair<std::uint32_t, std::uint64_t>> current;
    for (std::size_t m = 0; m <= hi; ++m) {
        if (m > 0) {
            for (const auto& entry : model.copies[m - 1]) {
                const std::string& group = model.group_of_copy.at(entry.first);
                auto at = current.find(group);
                if (at == current.end() || at->second.second < entry.second) {
                    current[group] = {entry.first, entry.second};
                }
            }
            const auto extents = model.extents.find(m);
            if (extents != model.extents.end()) {
                // An extent of this sequence does not verify, so the sequence
                // does not: every copy with a segment on an extent of it reads
                // Bad.
                bool extent_lost = false;
                for (const auto& write : extents->second) {
                    if (dropped[write.first] == 0) continue;
                    extent_lost = true;
                    break;
                }
                const auto blocks = model.big_blocks.find(m);
                if (blocks != model.big_blocks.end()) {
                    for (const std::size_t write : blocks->second) {
                        if (dropped[write] != 0) extent_lost = true;
                    }
                }
                if (extent_lost) {
                    for (const auto& write : extents->second) {
                        lost_copies.insert(write.second);
                    }
                }
                for (const auto& write : extents->second) {
                    if (model.durable_copies.count(write.second) == 0) continue;
                    if (dropped[write.first] != 0) {
                        ++oracle.durable_lost;
                    } else {
                        ++oracle.durable_kept;
                    }
                }
            }
            const auto bigs = model.bigs.find(m);
            if (bigs != model.bigs.end()) {
                for (const auto& write : bigs->second) {
                    if (dropped[write.first] != 0) {
                        lost_copies.insert(write.second);
                    }
                    if (model.durable_copies.count(write.second) == 0) continue;
                    if (dropped[write.first] != 0) {
                        ++oracle.durable_lost;
                    } else {
                        ++oracle.durable_kept;
                    }
                }
            }
        }
        if (m < lo || m > hi) continue;
        // The newest checkpoint at or before m lists every earlier commit by a
        // summary that names its record: a lost record of one of them makes
        // its copies Bad, as a lost extent does.
        std::set<std::uint32_t> lost_here = lost_copies;
        const std::size_t base = m / store::engine::CHECKPOINT_INTERVAL *
                                 store::engine::CHECKPOINT_INTERVAL;
        bool record_lost = false;
        for (std::size_t sequence = 1; sequence < base; ++sequence) {
            if (dropped[record_at[sequence - 1]] == 0) continue;
            const auto extents = model.extents.find(sequence);
            if (extents == model.extents.end()) continue;
            for (const auto& write : extents->second) {
                lost_here.insert(write.second);
                record_lost = true;
            }
        }
        if (record_lost) ++oracle.summary_records_lost;
        std::set<std::string> blank;
        for (const std::uint32_t copy : lost_here) {
            const std::string& group = model.group_of_copy.at(copy);
            const auto at = current.find(group);
            if (at != current.end() && at->second.first == copy) {
                blank.insert(group);
            }
        }
        oracle.lost.push_back(lost_here.size());
        oracle.expected.push_back(refs[m].text_without(blank));
    }
    return oracle;
}

}  // namespace

TEST_CASE("crash: a kill point leaves only whole commits") {
    using namespace store;
    using namespace store::engine;

    dftu_utils_test::ScopedTestDir dir("store_crash");
    WriterOptions options;
    options.extent_size = EXTENT_BYTES;
    options.extent_quota_bytes = EXTENT_QUOTA_BYTES;
    options.commit_bytes = COMMIT_BYTES;
    options.commit_interval = COMMIT_INTERVAL;
    options.host = dstring("host");
    options.label = dstring("crash");

    auto created =
        Writer::create(dir.str(), *store_runtime().executor(), options);
    REQUIRE(created.has_value());
    std::unique_ptr<Writer> writer = std::move(*created);

    // Record what the writer issues from here on. Create() wrote the header
    // before this, so no image carries it and every image that carries a
    // header carries a rewrite.
    io_fault::start(WriterTestHook::file(*writer).fd());

    auto sums = writer->add_namespace(fold_spec("sums", FoldKind::SumI64));
    auto durable = writer->add_namespace(durable_spec("durable"));
    auto raw = writer->add_namespace(put_spec("raw", 0));
    auto big = writer->add_namespace(put_spec("big", 0));
    REQUIRE(sums.has_value());
    REQUIRE(durable.has_value());
    REQUIRE(raw.has_value());
    REQUIRE(big.has_value());
    const Namespaces ids{*sums, *durable, *raw, *big};

    std::atomic<int> errors{0};
    const auto run_round = [&](int round) {
        auto work = store_runtime().submit(
            coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
                for (int index = 0; index < COROUTINES; ++index) {
                    scope.spawn([&, index]() -> coro::CoroTask<void> {
                        co_await write_coroutine(*writer, ids, errors, index,
                                                 round, round + 1);
                    });
                }
                co_return;
            }),
            "crash-writers");
        await_handle(work, TASK_LIMIT);
        REQUIRE(errors.load() == 0);
        // A flush after every round: it waits for the extent writes of the
        // round and runs one group commit, so the log holds at least one record
        // per round whatever the scheduler does with the committer task.
        REQUIRE(flush_writer(*writer).has_value());
    };

    for (int round = 0; round < ROUNDS; ++round) run_round(round);

    // One small batch and a flush each: one record apiece in every build, so
    // the log always passes 64 records and holds a checkpoint record.
    for (int tail = 0; tail < TAIL_COMMITS; ++tail) {
        Batch batch = writer->batch(ids.raw, file_scope("/crash/tail.pfw"), 0);
        const std::string key = "tail-" + std::to_string(tail);
        batch.put(view_of(key), view_of(key));
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(flush_writer(*writer).has_value());
    }

    REQUIRE(close_writer(*writer).has_value());
    const dstd::Vector<io_fault::Event> log = io_fault::stop();

    const std::string file_name = to_std(writer->name());
    const std::string final_bytes = read_bytes(dir.file(file_name));
    REQUIRE(!final_bytes.empty());

    std::vector<std::size_t> record_at;
    std::vector<std::size_t> header_at;
    std::size_t syncs = 0;
    for (std::size_t i = 0; i < log.size(); ++i) {
        const io_fault::Event& event = log[i];
        if (is_sync(event)) {
            ++syncs;
            continue;
        }
        REQUIRE(event.offset + event.length <= final_bytes.size());
        if (event.kind == io_fault::WriteKind::Record) record_at.push_back(i);
        if (event.kind == io_fault::WriteKind::Header) header_at.push_back(i);
    }

    std::size_t extents = 0;
    std::size_t bigs = 0;
    for (const io_fault::Event& event : log) {
        if (event.kind == io_fault::WriteKind::Extent) ++extents;
        if (event.kind == io_fault::WriteKind::Big) ++bigs;
    }
    std::printf(
        "crash log: events=%zu records=%zu headers=%zu extents=%zu bigs=%zu "
        "syncs=%zu file=%zu\n",
        log.size(), record_at.size(), header_at.size(), extents, bigs, syncs,
        final_bytes.size());

    CHECK(log.size() > 40);
    CHECK(record_at.size() >= static_cast<std::size_t>(ROUNDS));
    CHECK(record_at.size() > 64);
    CHECK(bigs >= 1);
    // Every durable round commits to the durable namespace and every round is
    // flushed, so every durable round's group commit syncs twice. This is what
    // makes m_lo below a statement about durability: a writer that skips the
    // durable sync says nothing about the commits that must survive.
    CHECK(syncs >= static_cast<std::size_t>(2 * (ROUNDS / DURABLE_EVERY)));
    CHECK(WriterTestHook::extents(*writer).held == 0);
    REQUIRE(header_at.size() == record_at.size() + 1);

    const std::vector<NamespaceSpec> specs = {
        fold_spec("sums", FoldKind::SumI64), durable_spec("durable"),
        put_spec("raw", 0), put_spec("big", 0)};

    // One directory and one file, truncated before every image: a fresh
    // directory per image would be several thousand directory trees.
    const std::string image_dir = dir.str() + "/images";
    REQUIRE(
        dftracer::utils::dstd::fs::create_directories(image_dir).has_value());
    const std::string image_path = image_dir + "/" + file_name;
    const auto dump_of = [&](const std::string& bytes) {
        write_bytes(image_path, bytes);
        auto snapshot = store::engine::Snapshot::open(image_dir);
        REQUIRE(snapshot.has_value());
        return dump_snapshot(*snapshot, specs);
    };

    // The references: R_m is the in-order image of the first m commits, with
    // the header of the m-th commit and every extent of the log. A commit's
    // header is written right after its record, so the m-th header names the
    // m-th record.
    std::vector<std::string> texts;
    std::vector<Dump> refs;
    for (std::size_t m = 0; m <= record_at.size(); ++m) {
        std::vector<std::pair<std::size_t, std::uint64_t>> applied;
        if (m >= 1) {
            const std::size_t at = header_at[m - 1];
            applied.emplace_back(at, whole_length(log[at]));
        }
        for (std::size_t c = 0; c < m; ++c) {
            applied.emplace_back(record_at[c], whole_length(log[record_at[c]]));
        }
        for (std::size_t i = 0; i < log.size(); ++i) {
            // Every extent of the log and every big value of it: a write no
            // applied record names cannot change a dump, and both are written
            // once and never rewritten.
            if (log[i].kind == io_fault::WriteKind::Extent ||
                log[i].kind == io_fault::WriteKind::Big) {
                applied.emplace_back(i, whole_length(log[i]));
            }
        }
        Dumped dumped = dump_of(render_image(applied, log, final_bytes));
        REQUIRE(dumped.ok);
        texts.push_back(dumped.text());
        refs.push_back(std::move(dumped.dump));
    }

    const CrashModel model = build_model(log, record_at, final_bytes);

    // Durable placement from the log itself: a commit that names a durable copy
    // has a sync after every write before its record and after its header. A
    // writer that syncs in the wrong place fails here, though m_lo would pass.
    const auto sync_between = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            if (is_sync(log[i])) return true;
        }
        return false;
    };
    std::size_t durable_commits = 0;
    for (std::size_t c = 0; c < record_at.size(); ++c) {
        const auto sequence_extents = model.extents.find(c + 1);
        if (sequence_extents == model.extents.end()) continue;
        bool names_durable = false;
        for (const auto& write : sequence_extents->second) {
            if (model.durable_copies.count(write.second) != 0) {
                names_durable = true;
            }
        }
        if (!names_durable) continue;
        ++durable_commits;
        // Only this commit's own blocks: an extent of a later commit can be
        // reserved after the sync and before this record.
        std::size_t last_data = 0;
        for (const auto& write : sequence_extents->second) {
            last_data = std::max(last_data, write.first);
        }
        const auto sequence_bigs = model.big_blocks.find(c + 1);
        if (sequence_bigs != model.big_blocks.end()) {
            for (const std::size_t write : sequence_bigs->second) {
                last_data = std::max(last_data, write);
            }
        }
        CHECK_MESSAGE(sync_between(last_data, record_at[c]),
                      "no sync before durable record " << c + 1);
        const std::size_t next =
            c + 1 < record_at.size() ? record_at[c + 1] : log.size();
        CHECK_MESSAGE(sync_between(header_at[c], next),
                      "no sync after the header of durable record " << c + 1);
    }
    CHECK(durable_commits >= static_cast<std::size_t>(ROUNDS / DURABLE_EVERY));
    const auto range_texts = [&](std::size_t lo, std::size_t hi) {
        const auto first = static_cast<std::ptrdiff_t>(lo);
        const auto last = static_cast<std::ptrdiff_t>(hi) + 1;
        return std::vector<std::string>(texts.begin() + first,
                                        texts.begin() + last);
    };

    struct VariantStats {
        const char* name;
        std::size_t images = 0;
        std::size_t min_m = 0;
        std::size_t max_m = 0;
        bool have_m = false;
        bool lowered = false;
    };
    VariantStats stats[4] = {{"in-order", 0, 0, 0, false, false},
                             {"torn", 0, 0, 0, false, false},
                             {"lost", 0, 0, 0, false, false},
                             {"reordered", 0, 0, 0, false, false}};

    std::size_t failures = 0;
    std::size_t reported = 0;
    std::size_t header_inversions = 0;
    std::size_t header_scans = 0;
    std::size_t whole_file_skips = 0;
    std::size_t lost_with_copies = 0;
    std::size_t big_dropped = 0;
    std::size_t durable_lost = 0;
    std::size_t durable_kept_images = 0;
    std::size_t summary_records_lost = 0;

    // `exact` is the one m an image must reach when the tear left no usable
    // header, and SIZE_MAX when any m in the range will do. `expected` holds
    // the dump the image must equal for m from `lo` up.
    const auto verify = [&](std::size_t which, std::size_t kill, std::size_t lo,
                            const std::vector<std::string>& expected,
                            const std::string& image,
                            std::size_t exact) -> std::size_t {
        const std::size_t hi = lo + expected.size() - 1;
        Dumped dumped = dump_of(image);
        if (lo > 0 && dumped.skipped > 0) ++whole_file_skips;
        const std::string text = dumped.text();
        std::size_t seen = SIZE_MAX;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (expected[i] != text) continue;
            seen = lo + i;
        }
        const bool matched = seen != SIZE_MAX;
        VariantStats& stat = stats[which];
        ++stat.images;
        if (matched) {
            if (!stat.have_m) {
                stat.min_m = seen;
                stat.max_m = seen;
                stat.have_m = true;
            } else {
                stat.min_m = std::min(stat.min_m, seen);
                stat.max_m = std::max(stat.max_m, seen);
            }
            if (seen < hi) stat.lowered = true;
        }
        const bool ok = matched && (exact == SIZE_MAX || seen == exact);
        if (!ok) {
            ++failures;
            if (reported < FAILURE_REPORTS) {
                ++reported;
                const std::string root =
                    std::string((dftracer::utils::dstd::fs::temp_directory() /
                                 "dftu_store_crash_failure")
                                    .str());
                REQUIRE(dftracer::utils::dstd::fs::create_directories(root)
                            .has_value());
                const std::string tag =
                    std::string(stat.name) + "-k" + std::to_string(kill);
                write_bytes(root + "/" + tag + ".dfs", image);
                write_bytes(root + "/" + tag + ".log", log_text(log));
                std::printf(
                    "crash image failed: variant=%s kill=%zu m_lo=%zu m_hi=%zu "
                    "exact=%zu matched=%d seen=%zu read=%s, image and log in "
                    "%s\n",
                    stat.name, kill, lo, hi, exact, matched ? 1 : 0, seen,
                    dumped.ok ? "ok" : dumped.error.c_str(), root.c_str());
            }
            CHECK_MESSAGE(ok, "variant " << std::string(stat.name) << " kill "
                                         << kill << " m_lo " << lo << " m_hi "
                                         << hi << " exact " << exact
                                         << " matched " << matched << " seen "
                                         << seen);
        }
        return seen;
    };

    const std::size_t step = SANITIZED ? 4 : 1;
    for (std::size_t k = 0; k <= log.size(); k += step) {
        const std::size_t from = unsynced_from(log, k);
        const std::size_t m_lo = records_below(record_at, from);
        const std::size_t m_hi = records_below(record_at, k);

        // a. every write of the prefix, in order, whole.
        std::vector<std::pair<std::size_t, std::uint64_t>> applied;
        applied.reserve(k);
        for (std::size_t i = 0; i < k; ++i) {
            applied.emplace_back(i, whole_length(log[i]));
        }
        // Every write is whole, so the reader reaches the newest record, or the
        // one before it when the header of the newest is not written yet.
        const std::size_t in_order_lo =
            std::max(m_lo, m_hi == 0 ? std::size_t{0} : m_hi - 1);
        verify(0, k, in_order_lo, range_texts(in_order_lo, m_hi),
               render_image(applied, log, final_bytes), SIZE_MAX);

        // b. the last unsynced write keeps a prefix. The write is the last one
        // applied, so no applied record names it. A torn header leaves a crc
        // that does not cover its content, so the reader must fall back to the
        // scan and reach every whole record of the prefix.
        const std::size_t tear = last_write_below(log, k);
        if (tear < k && tear >= from) {
            std::mt19937_64 rng(seed_of(k, 1, 0));
            const std::uint64_t keep = torn_prefix(log[tear], rng);
            applied.clear();
            for (std::size_t i = 0; i < k; ++i) {
                applied.emplace_back(i,
                                     i == tear ? keep : whole_length(log[i]));
            }
            const bool torn_header =
                log[tear].kind == io_fault::WriteKind::Header;
            // A prefix below the crc field leaves the previous header, crc and
            // all, and below the tail fields it leaves the new crc over the old
            // tail. Only a prefix that covers the crc field makes the header
            // fail its crc, and then the reader has to scan.
            const bool scan_needed = torn_header && keep >= HEADER_CRC_BYTES;
            const std::size_t exact = scan_needed ? m_hi : SIZE_MAX;
            if (scan_needed) ++header_scans;
            verify(1, k, m_lo, range_texts(m_lo, m_hi),
                   render_image(applied, log, final_bytes), exact);
        }

        // c. an unsynced write is lost with probability 1/2, once per seed. A
        // lost extent leaves the copies of the record that names it unreadable,
        // and a lost big value leaves its segment pointing at gone bytes.
        for (std::size_t seed = 0; seed < LOST_SEEDS; ++seed) {
            std::mt19937_64 rng(seed_of(k, 2, seed));
            std::vector<char> dropped(log.size(), 0);
            bool dropped_extent = false;
            std::vector<std::pair<std::size_t, std::uint64_t>> kept;
            kept.reserve(k);
            for (std::size_t i = 0; i < k; ++i) {
                const io_fault::Event& event = log[i];
                const bool droppable = !is_sync(event) && i >= from;
                if (droppable && (rng() % 2) == 0) {
                    dropped[i] = 1;
                    if (event.kind == io_fault::WriteKind::Extent) {
                        dropped_extent = true;
                    }
                    if (event.kind == io_fault::WriteKind::Big) ++big_dropped;
                    continue;
                }
                kept.emplace_back(i, whole_length(event));
            }
            const LostOracle oracle =
                lost_oracle(model, refs, dropped, record_at, m_lo, m_hi);
            durable_lost += oracle.durable_lost;
            summary_records_lost += oracle.summary_records_lost;
            if (dropped_extent && oracle.durable_kept > 0)
                ++durable_kept_images;
            const std::size_t seen =
                verify(2, k, m_lo, oracle.expected,
                       render_image(kept, log, final_bytes), SIZE_MAX);
            if (seen != SIZE_MAX && seen - m_lo < oracle.lost.size() &&
                oracle.lost[seen - m_lo] > 0) {
                ++lost_with_copies;
            }
        }

        // d. the unsynced writes in a random order, once per seed. The second
        // seed reverses them. Only the header is written at an offset twice, so
        // only the order of the headers can change what the file says.
        for (std::size_t seed = 0; seed < REORDER_SEEDS; ++seed) {
            std::mt19937_64 rng(seed_of(k, 3, seed));
            std::vector<std::size_t> order;
            order.reserve(k);
            std::vector<std::size_t> shuffled;
            for (std::size_t i = 0; i < k; ++i) {
                if (i < from) {
                    order.push_back(i);
                } else {
                    shuffled.push_back(i);
                }
            }
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            if (seed != 0) std::reverse(shuffled.begin(), shuffled.end());
            order.insert(order.end(), shuffled.begin(), shuffled.end());
            std::vector<std::pair<std::size_t, std::uint64_t>> reordered;
            reordered.reserve(order.size());
            std::size_t highest_header = 0;
            bool inverted = false;
            for (const std::size_t i : order) {
                if (log[i].kind == io_fault::WriteKind::Header &&
                    !is_sync(log[i])) {
                    if (highest_header > i) inverted = true;
                    highest_header = std::max(highest_header, i);
                }
                reordered.emplace_back(i, whole_length(log[i]));
            }
            if (inverted) ++header_inversions;
            verify(3, k, m_lo, range_texts(m_lo, m_hi),
                   render_image(reordered, log, final_bytes), SIZE_MAX);
        }
    }

    for (const VariantStats& stat : stats) {
        std::printf("crash variant %s: images=%zu m=%zu..%zu lowered=%d\n",
                    stat.name, stat.images, stat.min_m, stat.max_m,
                    stat.lowered ? 1 : 0);
    }
    std::printf(
        "crash lost coverage: images_with_lost_copies=%zu bigs_dropped=%zu "
        "durable_kept_images=%zu durable_lost=%zu summary_records_lost=%zu\n",
        lost_with_copies, big_dropped, durable_kept_images, durable_lost,
        summary_records_lost);

    CHECK(failures == 0);
    CHECK(header_inversions > 0);
    CHECK(header_scans > 0);
    for (const VariantStats& stat : stats) {
        CHECK(stat.images > 0);
    }
    CHECK(stats[1].lowered);
    CHECK(stats[2].lowered);
    CHECK(stats[3].lowered);
    CHECK(stats[0].have_m);
    // A copy that lost its extent reads Bad in at least one image, a big value
    // is dropped in at least one, a durable copy the writer synced before its
    // record is never one of them, and no image is skipped whole once a commit
    // is durable.
    CHECK(lost_with_copies > 0);
    CHECK(big_dropped > 0);
    CHECK(durable_lost == 0);
    // A commit record a checkpoint summary names is lost in at least one
    // image, and its copies read Bad there.
    CHECK(summary_records_lost > 0);
    CHECK(durable_kept_images > 0);
    CHECK(whole_file_skips == 0);
}
