// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/committer.h>
#include <dftracer/utils/store/data_file.h>
#include <dftracer/utils/store/fold.h>
#include <dftracer/utils/store/open_extent.h>
#include <dftracer/utils/store/snapshot.h>
#include <dftracer/utils/store/writer.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <variant>

using namespace dftracer::utils;

namespace dftracer::utils::store::engine {

struct DataFileWriterTestHook {
    static void reopen_read_only(DataFileWriter& writer,
                                 const dstd::String& path) {
        auto file = dstd::File::open(dstd::PathView(path), {});
        if (file) writer.fd_ = std::move(*file);
    }
};

}  // namespace dftracer::utils::store::engine

namespace {

using namespace dftracer::utils::store;

constexpr std::uint32_t EXTENT = 4096;
constexpr int RUNTIME_THREADS = 8;

// A sanitizer build runs under a ctest timeout of 120 s (ASan) or 300 s (TSan)
// and makes a commit tens of times slower, so the thread and batch counts of
// the two large cases shrink there. Each case keeps its shape and every
// assertion. Only the number of batches differs.
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

dstd::ByteView view_of(const dstd::String& bytes) {
    return {bytes.data(), bytes.size()};
}

dstd::String to_std(dstd::ByteView bytes) {
    return dstd::String(reinterpret_cast<const char*>(bytes.data()),
                        bytes.size());
}

dstd::String i64_bytes(std::int64_t value) {
    dstd::String bytes(8, '\0');
    dstd::write_u64_le(reinterpret_cast<std::uint8_t*>(bytes.data()),
                       static_cast<std::uint64_t>(value));
    return bytes;
}

std::int64_t read_i64(dstd::ByteView bytes) {
    REQUIRE(bytes.size() == 8);
    return std::bit_cast<std::int64_t>(
        dstd::read_u64_le(reinterpret_cast<const std::uint8_t*>(bytes.data())));
}

NamespaceSpec put_spec(const dstd::String& name, std::uint32_t version = 0) {
    NamespaceSpec spec;
    spec.name = dstd::String(name);
    spec.format_version = version;
    spec.fold = FoldKind::None;
    return spec;
}

NamespaceSpec fold_spec(const dstd::String& name, FoldKind kind) {
    NamespaceSpec spec;
    spec.name = dstd::String(name);
    spec.format_version = 0;
    spec.fold = kind;
    return spec;
}

NamespaceSpec durable_spec(const dstd::String& name, bool durable) {
    NamespaceSpec spec;
    spec.name = dstd::String(name);
    spec.format_version = 0;
    spec.fold = FoldKind::None;
    spec.durable = durable;
    return spec;
}

ScopeKey file_scope(const dstd::String& path) {
    return ScopeKey::file(dstd::String(path));
}

dstd::String scope_path(int index) {
    return "/data/file-" + dstd::to_string(index) + ".dft";
}

engine::WriterOptions store_options(std::uint32_t extent_size,
                                    std::uint64_t commit_bytes,
                                    std::chrono::milliseconds commit_interval) {
    engine::WriterOptions options;
    options.extent_size = extent_size;
    options.commit_bytes = commit_bytes;
    options.commit_interval = commit_interval;
    options.host = dstd::String("host");
    options.label = dstd::String("label");
    return options;
}

engine::WriterOptions manual_options(std::uint32_t extent_size) {
    return store_options(extent_size, std::uint64_t{1} << 40,
                         std::chrono::hours(1));
}

Runtime& store_runtime() {
    static Runtime runtime(RUNTIME_THREADS);
    return runtime;
}

dstd::UniquePtr<engine::Writer> make_writer(
    const dftu_utils_test::ScopedTestDir& dir,
    const engine::WriterOptions& options) {
    auto created =
        engine::Writer::create(dir.str(), *store_runtime().executor(), options);
    REQUIRE(created.has_value());
    return std::move(*created);
}

dstd::Expected<void, dstd::Error> flush_writer(engine::Writer& writer) {
    return store_runtime().submit(writer.flush()).get();
}

dstd::Expected<void, dstd::Error> close_writer(engine::Writer& writer) {
    return store_runtime().submit(writer.close()).get();
}

/// commit() with the retry the writer documents, for a caller that is not a
/// coroutine: on Busy it waits for space() on the store runtime and commits
/// the same batch again, since a Busy commit left the batch whole.
dstd::Expected<void, dstd::Error> commit_waiting(engine::Writer& writer,
                                                 Batch&& batch) {
    for (;;) {
        auto committed = writer.commit(std::move(batch));
        if (committed.has_value() ||
            !committed.error().is_code(engine::WriterErrc::Busy)) {
            return committed;
        }
        dftu_utils_test::ok(store_runtime().submit(writer.space()).get());
    }
}

dstd::Vector<std::uint8_t> read_file(const dstd::String& path) {
    const dstd::String text = dftu_utils_test::read_text(path);
    return dstd::Vector<std::uint8_t>(text.begin(), text.end());
}

dstd::String copy_out(const dstd::String& source,
                      const dftu_utils_test::ScopedTestDir& target,
                      const dstd::String& name) {
    const dstd::Vector<std::uint8_t> bytes = read_file(source);
    dftu_utils_test::FileOut output(target.file(name));
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    return target.str();
}

std::size_t record_count(const dstd::String& path) {
    auto reader = engine::DataFileReader::open(dstd::String(path));
    if (!reader) return 0;
    return reader->records_read();
}

/// Every extent ref a file holds names a copy the same file announces, so a
/// byte copy of it never holds an extent whose items are still to come.
void check_extent_refs_announced(const dstd::String& path) {
    auto reader = engine::DataFileReader::open(dstd::String(path));
    REQUIRE(reader.has_value());
    dstd::Vector<std::uint32_t> copies;
    for (const store::engine::CopyItem& copy : reader->index().copies) {
        copies.push_back(copy.local_id);
    }
    for (const store::engine::IndexedExtentRef& extent :
         reader->index().extent_refs) {
        for (const store::engine::ExtentSegment& segment :
             extent.ref.segments) {
            REQUIRE(std::find(copies.begin(), copies.end(), segment.copy_id) !=
                    copies.end());
        }
    }
}

dstd::Vector<std::uint64_t> copy_generations(const dstd::String& path) {
    auto reader = engine::DataFileReader::open(dstd::String(path));
    REQUIRE(reader.has_value());
    dstd::Vector<std::uint64_t> out;
    for (const store::engine::CopyItem& copy : reader->index().copies) {
        out.push_back(copy.generation);
    }
    return out;
}

dstd::Vector<std::uint64_t> drop_generations(const dstd::String& path) {
    auto reader = engine::DataFileReader::open(dstd::String(path));
    REQUIRE(reader.has_value());
    dstd::Vector<std::uint64_t> out;
    for (const store::engine::DropItem& drop : reader->index().drops) {
        out.push_back(drop.generation);
    }
    return out;
}

struct Row {
    dstd::String scope;
    dstd::String key;
    dstd::String value;
};

dstd::Vector<Row> collect(store::engine::ScanCursor& cursor) {
    dstd::Vector<Row> rows;
    while (!cursor.at_end()) {
        auto value = cursor.value();
        REQUIRE(value.has_value());
        REQUIRE_FALSE(cursor.scope().paths.empty());
        rows.push_back({dstd::String(cursor.scope().paths[0]),
                        to_std(cursor.key()), to_std(*value)});
        cursor.next();
    }
    return rows;
}

using RowsByScope =
    dstd::Map<dstd::Pair<dstd::String, dstd::String>, dstd::String>;

RowsByScope index_rows(const dstd::Vector<Row>& rows) {
    RowsByScope out;
    for (const Row& row : rows) out[{row.scope, row.key}] = row.value;
    return out;
}

dstd::String batch_key(int index) {
    const dstd::String digits = dstd::to_string(index);
    return "b" + dstd::String(3 - digits.size(), '0') + digits + "-k";
}

}  // namespace

TEST_CASE("many threads commit and one close keeps every key and every sum") {
    constexpr int THREADS = SANITIZED ? 8 : 32;
    constexpr int BATCHES = SANITIZED ? 60 : 500;
    constexpr int KEYS = 3;
    constexpr int FOLD_KEYS = 2;
    constexpr int SCOPES = 4;

    dftu_utils_test::ScopedTestDir dir("store_concurrent_many");
    auto writer = make_writer(dir, store_options(64 * 1024, 256 * 1024,
                                                 std::chrono::milliseconds(5)));

    auto created = writer->add_namespace(put_spec("raw-a", 0));
    REQUIRE(created.has_value());
    const engine::NamespaceId raw_a = *created;
    created = writer->add_namespace(put_spec("raw-b", 1));
    REQUIRE(created.has_value());
    const engine::NamespaceId raw_b = *created;
    created = writer->add_namespace(fold_spec("sums", FoldKind::SumI64));
    REQUIRE(created.has_value());
    const engine::NamespaceId sums = *created;

    const auto scope_of = [](int thread, int batch) {
        return scope_path((thread * 31 + batch * 7) % SCOPES);
    };
    const auto key_of = [](int thread, int batch, int key) {
        return "t" + dstd::to_string(thread) + "-b" + dstd::to_string(batch) +
               "-k" + dstd::to_string(key);
    };
    const auto value_of = [](const dstd::String& tag, int thread, int batch,
                             int key) {
        return tag + "-" + dstd::to_string(thread) + "-" +
               dstd::to_string(batch) + "-" + dstd::to_string(key);
    };
    const auto fold_key = [](int key) { return "m" + dstd::to_string(key); };

    RowsByScope expected_a;
    RowsByScope expected_b;
    dstd::Map<dstd::String, std::int64_t> expected_sum;
    for (int t = 0; t < THREADS; ++t) {
        for (int b = 0; b < BATCHES; ++b) {
            const dstd::String scope = scope_of(t, b);
            for (int k = 0; k < KEYS; ++k) {
                expected_a[{scope, key_of(t, b, k)}] = value_of("a", t, b, k);
                expected_b[{scope, key_of(t, b, k)}] = value_of("b", t, b, k);
            }
            for (int k = 0; k < FOLD_KEYS; ++k) {
                expected_sum[fold_key(k)] += t + 1;
            }
        }
    }

    std::atomic<bool> failed{false};
    dstd::Vector<dstd::Thread> workers;
    workers.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) {
        workers.emplace_back([&, t] {
            for (int b = 0; b < BATCHES; ++b) {
                const ScopeKey scope = file_scope(scope_of(t, b));
                Batch a = writer->batch(raw_a, scope, 0);
                Batch other = writer->batch(raw_b, scope, 0);
                Batch sum = writer->batch(sums, scope, 0);
                for (int k = 0; k < KEYS; ++k) {
                    const dstd::String key = key_of(t, b, k);
                    const dstd::String first = value_of("a", t, b, k);
                    const dstd::String second = value_of("b", t, b, k);
                    a.put(view_of(key), view_of(first));
                    other.put(view_of(key), view_of(second));
                }
                for (int k = 0; k < FOLD_KEYS; ++k) {
                    const dstd::String value = i64_bytes(t + 1);
                    const dstd::String key = fold_key(k);
                    sum.fold(view_of(key), view_of(value));
                }
                if (!commit_waiting(*writer, std::move(a)).has_value())
                    failed = true;
                if (!commit_waiting(*writer, std::move(other)).has_value())
                    failed = true;
                if (!commit_waiting(*writer, std::move(sum)).has_value())
                    failed = true;
            }
        });
    }
    for (dstd::Thread& worker : workers) worker.join();
    REQUIRE_FALSE(failed.load());

    auto closed = close_writer(*writer);
    REQUIRE(closed.has_value());

    auto snapshot = engine::Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());

    auto cursor = snapshot->scan_all(put_spec("raw-a", 0), {}, {});
    const RowsByScope got_a = index_rows(collect(cursor));
    CHECK(got_a.size() == expected_a.size());
    CHECK(got_a == expected_a);

    cursor = snapshot->scan_all(put_spec("raw-b", 1), {}, {});
    const RowsByScope got_b = index_rows(collect(cursor));
    CHECK(got_b.size() == expected_b.size());
    CHECK(got_b == expected_b);

    auto sums_cursor =
        snapshot->scan_all(fold_spec("sums", FoldKind::SumI64), {}, {});
    dstd::Map<dstd::String, std::int64_t> got_sum;
    const dstd::Vector<Row> sum_rows = collect(sums_cursor);
    for (const Row& row : sum_rows) {
        got_sum[row.key] = read_i64(view_of(row.value));
    }
    CHECK(got_sum.size() == expected_sum.size());
    CHECK(got_sum == expected_sum);
}

TEST_CASE("a batch appears whole or not at all in a snapshot taken mid-write") {
    constexpr int THREADS = SANITIZED ? 6 : 16;
    constexpr int BATCHES = SANITIZED ? 8 : 12;
    constexpr int KEYS = SANITIZED ? 400 : 2000;

    dftu_utils_test::ScopedTestDir dir("store_concurrent_atomic");
    dftu_utils_test::ScopedTestDir copies("store_concurrent_atomic_copies");
    auto writer = make_writer(
        dir, store_options(EXTENT, 256 * 1024, std::chrono::milliseconds(5)));
    const NamespaceSpec spec = put_spec("atomic", 0);
    auto created = writer->add_namespace(spec);
    REQUIRE(created.has_value());
    const engine::NamespaceId space = *created;
    const ScopeKey scope = file_scope("/data/atomic.dft");
    const dstd::String file_name = dstd::String(writer->name());

    std::atomic<int> next_batch{0};
    std::atomic<int> running{THREADS};
    std::atomic<bool> failed{false};
    dstd::Vector<dstd::Thread> workers;
    workers.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) {
        workers.emplace_back([&] {
            for (;;) {
                const int batch = next_batch.fetch_add(1);
                if (batch >= THREADS * BATCHES) break;
                Batch entries = writer->batch(space, scope, 0);
                for (int k = 0; k < KEYS; ++k) {
                    const dstd::String key =
                        batch_key(batch) + dstd::to_string(k);
                    const dstd::String value =
                        "v" + dstd::to_string(batch) + "-" + dstd::to_string(k);
                    entries.put(view_of(key), view_of(value));
                }
                if (!commit_waiting(*writer, std::move(entries)).has_value()) {
                    failed = true;
                }
            }
            --running;
        });
    }

    std::size_t ready_samples = 0;
    std::size_t visible_samples = 0;
    const auto sample = [&] {
        const dstd::String copy =
            copy_out(dstd::String(writer->path()), copies, file_name);
        check_extent_refs_announced(copies.file(file_name));
        auto snapshot = engine::Snapshot::open(copy);
        if (!snapshot.has_value() ||
            snapshot->state(spec, scope) != ScopeState::Ready) {
            return;
        }
        ++ready_samples;
        auto cursor = snapshot->scan_all(spec, {}, {});
        dstd::Map<int, std::size_t> per_batch;
        while (!cursor.at_end()) {
            auto value = cursor.value();
            REQUIRE(value.has_value());
            const dstd::String key = to_std(cursor.key());
            REQUIRE(key.size() > 4);
            const int batch = dftu_utils_test::to_number<int>(key.substr(1, 3));
            ++per_batch[batch];
            cursor.next();
        }
        for (const auto& entry : per_batch) {
            REQUIRE(entry.second == static_cast<std::size_t>(KEYS));
        }
        visible_samples += per_batch.empty() ? 0 : 1;
    };
    while (running.load() > 0) sample();
    for (int i = 0; i < 3; ++i) sample();

    for (dstd::Thread& worker : workers) worker.join();
    REQUIRE_FALSE(failed.load());
    CHECK(ready_samples > 0);
    CHECK(visible_samples > 0);

    auto closed = close_writer(*writer);
    REQUIRE(closed.has_value());

    auto snapshot = engine::Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->state(spec, scope) == ScopeState::Ready);
    auto cursor = snapshot->scan_all(spec, {}, {});
    dstd::Map<int, std::size_t> per_batch;
    while (!cursor.at_end()) {
        auto value = cursor.value();
        REQUIRE(value.has_value());
        const int batch =
            dftu_utils_test::to_number<int>(to_std(cursor.key()).substr(1, 3));
        ++per_batch[batch];
        cursor.next();
    }
    REQUIRE(per_batch.size() == static_cast<std::size_t>(THREADS * BATCHES));
    for (const auto& entry : per_batch) {
        CHECK(entry.second == static_cast<std::size_t>(KEYS));
    }
}

TEST_CASE("flush makes a committed batch visible in a byte copy") {
    dftu_utils_test::ScopedTestDir dir("store_concurrent_flush");
    dftu_utils_test::ScopedTestDir copies("store_concurrent_flush_copies");
    auto writer = make_writer(dir, manual_options(EXTENT));
    const NamespaceSpec spec = put_spec("flush", 0);
    auto created = writer->add_namespace(spec);
    REQUIRE(created.has_value());
    const ScopeKey scope = file_scope("/data/flush.dft");

    Batch entries = writer->batch(*created, scope, 0);
    for (int k = 0; k < 5; ++k) {
        const dstd::String key = "k" + dstd::to_string(k);
        const dstd::String value = "v" + dstd::to_string(k);
        entries.put(view_of(key), view_of(value));
    }
    REQUIRE(commit_waiting(*writer, std::move(entries)).has_value());

    auto flushed = flush_writer(*writer);
    REQUIRE(flushed.has_value());

    const dstd::String copy = copy_out(dstd::String(writer->path()), copies,
                                       dstd::String(writer->name()));
    auto snapshot = engine::Snapshot::open(copy);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->state(spec, scope) == ScopeState::Ready);
    for (int k = 0; k < 5; ++k) {
        const dstd::String key = "k" + dstd::to_string(k);
        const dstd::String expected = "v" + dstd::to_string(k);
        auto value = snapshot->get(spec, scope, view_of(key));
        REQUIRE(value.has_value());
        REQUIRE(value->has_value());
        CHECK(to_std((*value)->bytes()) == expected);
    }

    auto closed = close_writer(*writer);
    REQUIRE(closed.has_value());
}

static dstd::Vector<std::uint8_t> read_range(const dstd::String& path,
                                             std::uint64_t offset,
                                             std::size_t length) {
    const dstd::String text = dftu_utils_test::read_text(path);
    if (offset >= text.size()) return {};
    const std::size_t count =
        std::min(length, text.size() - static_cast<std::size_t>(offset));
    return dstd::Vector<std::uint8_t>(
        text.begin() + static_cast<std::ptrdiff_t>(offset),
        text.begin() + static_cast<std::ptrdiff_t>(offset + count));
}

/// True when the record the file's header names as its tail holds an extent ref
/// with a segment whose key range is exactly `key`. Reading the tail record
/// alone keeps the check off the rest of the file, which grows while it runs.
static bool marker_recorded(const dstd::String& path, const dstd::String& key) {
    const dstd::Vector<std::uint8_t> header =
        read_range(path, 0, store::engine::FILE_HEADER_BYTES);
    if (header.size() < store::engine::FILE_HEADER_BYTES) return false;
    const std::size_t tail_at = 48 + dstd::read_u16_le(header.data() + 44) +
                                dstd::read_u16_le(header.data() + 46);
    const std::uint64_t tail = dstd::read_u64_le(header.data() + tail_at);
    const dstd::Vector<std::uint8_t> prefix = read_range(path, tail, 16);
    if (prefix.size() != 16) return false;
    const std::size_t record_bytes =
        40 + dstd::read_u32_le(prefix.data() + 12) + 4;
    const dstd::Vector<std::uint8_t> bytes =
        read_range(path, tail, record_bytes);
    if (bytes.size() != record_bytes) return false;
    auto record = store::engine::decode_record(
        dstd::ByteView{bytes.data(), bytes.size()});
    if (!record.has_value() || record->offset != tail) return false;
    for (const store::engine::RecordItem& item : record->items) {
        const auto* ref = std::get_if<store::engine::ExtentRefItem>(&item);
        if (ref == nullptr) continue;
        for (const store::engine::ExtentSegment& segment : ref->segments) {
            if (segment.first_key.size() != key.size() ||
                segment.last_key.size() != key.size()) {
                continue;
            }
            if (std::memcmp(segment.first_key.data(), key.data(), key.size()) !=
                0) {
                continue;
            }
            if (std::memcmp(segment.last_key.data(), key.data(), key.size()) !=
                0) {
                continue;
            }
            return true;
        }
    }
    return false;
}

TEST_CASE("flush holds every batch committed before it under other threads") {
    // Many small segments cost the sealing append a sort, so the window between
    // the seal of an extent and the submit of its write is one a flush meets.
    // A flush that counts a sealed extent only from its submit would return
    // without recording the extent that holds the marker.
    constexpr std::uint32_t RACE_EXTENT = 64 * 1024;
    constexpr std::uint32_t BACKGROUND = 4;
    constexpr int ROUNDS = SANITIZED ? 40 : 200;
    constexpr int KEYS = 2;
    // Bounded so a slow build cannot pile up data that the final close then
    // spends minutes encoding. The rounds still race live background commits.
    constexpr std::uint64_t BACKGROUND_BATCHES = SANITIZED ? 400 : 20000;

    dftu_utils_test::ScopedTestDir dir("store_concurrent_flush_race");
    auto writer = make_writer(dir, manual_options(RACE_EXTENT));
    const NamespaceSpec spec = put_spec("raced", 0);
    auto created = writer->add_namespace(spec);
    REQUIRE(created.has_value());
    const engine::NamespaceId space = *created;
    const ScopeKey background_scope = file_scope("/data/background.dft");
    const ScopeKey marker_scope = file_scope("/data/marker.dft");
    const dstd::String value = "value";

    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    dstd::Vector<dstd::Thread> writers;
    writers.reserve(BACKGROUND);
    for (std::uint32_t t = 0; t < BACKGROUND; ++t) {
        writers.emplace_back([&, t] {
            std::uint64_t count = 0;
            while (!stop.load(std::memory_order_acquire) &&
                   count < BACKGROUND_BATCHES) {
                Batch entries = writer->batch(space, background_scope, 0);
                for (int k = 0; k < KEYS; ++k) {
                    const dstd::String key = "t" + dstd::to_string(t) + "-" +
                                             dstd::to_string(count) + "-" +
                                             dstd::to_string(k);
                    entries.put(view_of(key), view_of(value));
                }
                if (!commit_waiting(*writer, std::move(entries)).has_value()) {
                    failed.store(true, std::memory_order_release);
                }
                ++count;
            }
        });
    }

    const dstd::String path = dstd::String(writer->path());
    int missing = 0;
    int first_missing = -1;
    for (int round = 0; round < ROUNDS; ++round) {
        const dstd::String key = "marker-" + dstd::to_string(round);
        Batch marker = writer->batch(space, marker_scope, 0);
        marker.put(view_of(key), view_of(value));
        REQUIRE(commit_waiting(*writer, std::move(marker)).has_value());
        REQUIRE(flush_writer(*writer).has_value());

        if (!marker_recorded(path, key)) {
            if (first_missing < 0) first_missing = round;
            ++missing;
        }
    }

    stop.store(true, std::memory_order_release);
    for (dstd::Thread& worker : writers) worker.join();
    CHECK_FALSE(failed.load());
    INFO("first missing marker: " << first_missing);
    CHECK(missing == 0);

    REQUIRE(close_writer(*writer).has_value());
}

TEST_CASE("every extent ref carries the sequence of the record that holds it") {
    dftu_utils_test::ScopedTestDir dir("store_concurrent_ref_sequence");
    auto writer = make_writer(dir, manual_options(EXTENT));
    const NamespaceSpec spec = put_spec("sequences", 0);
    auto created = writer->add_namespace(spec);
    REQUIRE(created.has_value());
    const ScopeKey scope = file_scope("/data/sequences.dft");
    const dstd::String value(700, 'r');

    constexpr int ROUNDS = 12;
    for (int round = 0; round < ROUNDS; ++round) {
        Batch entries = writer->batch(*created, scope, 0);
        for (int k = 0; k < 3; ++k) {
            const dstd::String key =
                "r" + dstd::to_string(round) + "-" + dstd::to_string(k);
            entries.put(view_of(key), view_of(value));
        }
        REQUIRE(commit_waiting(*writer, std::move(entries)).has_value());
        REQUIRE(flush_writer(*writer).has_value());
    }
    REQUIRE(close_writer(*writer).has_value());

    const dstd::String path = dstd::String(writer->path());
    const dstd::Vector<std::uint8_t> file = read_file(path);
    std::size_t refs = 0;
    for (std::size_t offset = store::engine::FILE_HEADER_BYTES;
         offset + 40U <= file.size();
         offset += store::engine::FILE_HEADER_BYTES) {
        auto record = store::engine::decode_record(
            dstd::ByteView{file.data() + offset, file.size() - offset});
        if (!record.has_value() || record->offset != offset) continue;
        // The footer repeats the items of the chain, so it carries the sequence
        // each extent ref was first recorded under, not its own.
        if (record->kind == store::engine::RecordKind::Footer) continue;
        for (const store::engine::RecordItem& item : record->items) {
            const auto* ref = std::get_if<store::engine::ExtentRefItem>(&item);
            if (ref == nullptr) continue;
            ++refs;
            CHECK(ref->sequence == record->sequence);
        }
    }
    CHECK(refs >= ROUNDS);

    auto reader = engine::DataFileReader::open(dstd::String(path));
    REQUIRE(reader.has_value());
    CHECK(reader->index().last_sequence ==
          static_cast<std::uint64_t>(ROUNDS) + 1U);
}

TEST_CASE(
    "group commit by size writes a record once enough extent bytes wait") {
    constexpr std::uint64_t COMMIT_BYTES = 128 * 1024;
    // A 5000-byte value makes each batch a segment longer than an extent, so
    // every batch is written as its own run. The commit size counts its raw
    // segment bytes, about 5 KB, so 16 batches stay below COMMIT_BYTES and 40
    // pass it.
    constexpr int QUIET_BATCHES = 16;
    constexpr int HELD_BATCHES = 40;

    dftu_utils_test::ScopedTestDir dir("store_concurrent_size");
    dftu_utils_test::ScopedTestDir copies("store_concurrent_size_copies");
    auto writer = make_writer(
        dir, store_options(EXTENT, COMMIT_BYTES, std::chrono::hours(1)));
    const NamespaceSpec spec = put_spec("sized", 0);
    auto created = writer->add_namespace(spec);
    REQUIRE(created.has_value());
    const ScopeKey scope = file_scope("/data/sized.dft");
    const dstd::String file_name = dstd::String(writer->name());
    const dstd::String big_value(5000, 'x');

    int extent = 0;
    const auto commit_extent = [&] {
        const dstd::String key = "p" + dstd::to_string(extent);
        Batch entries = writer->batch(*created, scope, 0);
        entries.put(view_of(key), view_of(big_value));
        REQUIRE(commit_waiting(*writer, std::move(entries)).has_value());
        ++extent;
    };
    // The record is in the file as soon as its pwrite returns, so the reader
    // of the live file sees it. A byte copy adds an fd and a race to that.
    const auto records_now = [&] {
        return record_count(dstd::String(writer->path()));
    };

    for (int i = 0; i < QUIET_BATCHES; ++i) commit_extent();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(records_now() == 0);

    for (int i = QUIET_BATCHES; i < HELD_BATCHES; ++i) commit_extent();
    bool appeared = false;
    for (int attempt = 0; attempt < 200 && !appeared; ++attempt) {
        if (records_now() >= 1) {
            appeared = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(appeared);

    auto closed = close_writer(*writer);
    REQUIRE(closed.has_value());
}

TEST_CASE("group commit by time writes a record without flush") {
    dftu_utils_test::ScopedTestDir dir("store_concurrent_time");
    dftu_utils_test::ScopedTestDir copies("store_concurrent_time_copies");
    auto writer =
        make_writer(dir, store_options(EXTENT, std::uint64_t{1} << 40,
                                       std::chrono::milliseconds(50)));
    const NamespaceSpec spec = put_spec("timed", 0);
    auto created = writer->add_namespace(spec);
    REQUIRE(created.has_value());
    const ScopeKey scope = file_scope("/data/timed.dft");
    const dstd::String file_name = dstd::String(writer->name());

    Batch entries = writer->batch(*created, scope, 0);
    const dstd::String key = "only";
    const dstd::String big_value(5000, 'y');
    entries.put(view_of(key), view_of(big_value));
    REQUIRE(commit_waiting(*writer, std::move(entries)).has_value());

    const dstd::String source = dstd::String(writer->path());
    const auto started = std::chrono::steady_clock::now();
    bool appeared = false;
    while (std::chrono::steady_clock::now() - started <
           std::chrono::seconds(1)) {
        if (record_count(source) >= 1) {
            appeared = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(appeared);

    auto closed = close_writer(*writer);
    REQUIRE(closed.has_value());
}

TEST_CASE(
    "a durable namespace syncs per group commit and a volatile one at close") {
    dftu_utils_test::ScopedTestDir dir("store_concurrent_durable");
    engine::CommitterTestHook::reset();

    auto writer = make_writer(dir, manual_options(EXTENT));
    auto created = writer->add_namespace(durable_spec("durable", true));
    REQUIRE(created.has_value());
    const engine::NamespaceId durable = *created;
    created = writer->add_namespace(durable_spec("volatile", false));
    REQUIRE(created.has_value());
    const engine::NamespaceId volatile_space = *created;
    const ScopeKey scope = file_scope("/data/durable.dft");

    const auto commit = [&](engine::NamespaceId space,
                            const dstd::String& key) {
        Batch entries = writer->batch(space, scope, 0);
        entries.put(view_of(key), view_of(dstd::String("value")));
        REQUIRE(commit_waiting(*writer, std::move(entries)).has_value());
    };

    commit(durable, "d1");
    REQUIRE(flush_writer(*writer).has_value());
    CHECK(engine::CommitterTestHook::syncs() == 2);

    commit(volatile_space, "v1");
    REQUIRE(flush_writer(*writer).has_value());
    CHECK(engine::CommitterTestHook::syncs() == 2);

    commit(durable, "d2");
    REQUIRE(flush_writer(*writer).has_value());
    CHECK(engine::CommitterTestHook::syncs() == 4);

    REQUIRE(close_writer(*writer).has_value());
    CHECK(engine::CommitterTestHook::syncs() == 4);
}

TEST_CASE("one publish of several building copies writes one record") {
    dftu_utils_test::ScopedTestDir dir("store_concurrent_publish_many");
    engine::CommitterTestHook::reset();

    auto writer = make_writer(dir, manual_options(EXTENT));
    auto first = writer->add_namespace(durable_spec("first", true));
    auto second = writer->add_namespace(durable_spec("second", true));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const ScopeKey scope = file_scope("/data/publish.dft");
    const ScopeKey other = file_scope("/data/publish_other.dft");

    for (const engine::NamespaceId space : {*first, *second}) {
        Batch entries = writer->building_batch(space, scope);
        entries.put(view_of(dstd::String("k")), view_of(dstd::String("v")));
        REQUIRE(commit_waiting(*writer, std::move(entries)).has_value());
    }
    Batch entries = writer->building_batch(*first, other);
    entries.put(view_of(dstd::String("k")), view_of(dstd::String("v")));
    REQUIRE(commit_waiting(*writer, std::move(entries)).has_value());
    REQUIRE(flush_writer(*writer).has_value());
    const std::uint64_t before = engine::CommitterTestHook::syncs();

    const engine::CopyChange targets[] = {{*first, &scope, 1, nullptr},
                                          {*second, &scope, 2, nullptr},
                                          {*first, &other, 3, nullptr}};
    REQUIRE(writer->apply(targets).has_value());
    REQUIRE(flush_writer(*writer).has_value());
    CHECK(engine::CommitterTestHook::syncs() - before == 2);

    auto snapshot = engine::Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->copy_info(durable_spec("first", true), scope).state ==
          ScopeState::Ready);
    CHECK(
        snapshot->copy_info(durable_spec("second", true), scope).params_hash ==
        2);
    CHECK(snapshot->copy_info(durable_spec("first", true), other).state ==
          ScopeState::Ready);
    REQUIRE(close_writer(*writer).has_value());
}

TEST_CASE("one apply of a drop, a fail and a publish writes one record") {
    dftu_utils_test::ScopedTestDir dir("store_concurrent_apply_mixed");
    engine::CommitterTestHook::reset();

    auto writer = make_writer(dir, manual_options(EXTENT));
    auto dropped = writer->add_namespace(durable_spec("dropped", true));
    auto failed = writer->add_namespace(durable_spec("failed", true));
    auto built = writer->add_namespace(durable_spec("built", true));
    REQUIRE(dropped.has_value());
    REQUIRE(failed.has_value());
    REQUIRE(built.has_value());
    const ScopeKey scope = file_scope("/data/apply.dft");

    for (const engine::NamespaceId space : {*dropped, *failed}) {
        Batch entries = writer->batch(space, scope, 1);
        entries.put(view_of(dstd::String("k")), view_of(dstd::String("old")));
        REQUIRE(commit_waiting(*writer, std::move(entries)).has_value());
    }
    Batch entries = writer->building_batch(*built, scope);
    entries.put(view_of(dstd::String("k")), view_of(dstd::String("new")));
    REQUIRE(commit_waiting(*writer, std::move(entries)).has_value());
    REQUIRE(flush_writer(*writer).has_value());

    auto before = engine::Snapshot::open(dir.str());
    REQUIRE(before.has_value());
    const std::uint64_t syncs = engine::CommitterTestHook::syncs();

    const engine::CopyChange changes[] = {
        {*dropped, &scope, 0, nullptr, engine::ChangeKind::Dropped},
        {*failed, &scope, 4, nullptr, engine::ChangeKind::Failed},
        {*built, &scope, 6, nullptr, engine::ChangeKind::Ready}};
    REQUIRE(writer->apply(changes).has_value());
    REQUIRE(flush_writer(*writer).has_value());
    CHECK(engine::CommitterTestHook::syncs() - syncs == 2);

    auto after = engine::Snapshot::open(dir.str());
    REQUIRE(after.has_value());
    CHECK(after->copy_info(durable_spec("dropped", true), scope).state ==
          ScopeState::Absent);
    const auto failed_info =
        after->copy_info(durable_spec("failed", true), scope);
    CHECK(failed_info.state == ScopeState::Failed);
    CHECK(failed_info.params_hash == 4);
    const auto built_info =
        after->copy_info(durable_spec("built", true), scope);
    CHECK(built_info.state == ScopeState::Ready);
    CHECK(built_info.params_hash == 6);

    CHECK(before->copy_info(durable_spec("dropped", true), scope).state ==
          ScopeState::Ready);
    CHECK(before->copy_info(durable_spec("failed", true), scope).state ==
          ScopeState::Ready);
    CHECK(before->copy_info(durable_spec("built", true), scope).state !=
          ScopeState::Ready);
    REQUIRE(close_writer(*writer).has_value());
}

TEST_CASE("an apply with one bad change applies nothing") {
    dftu_utils_test::ScopedTestDir dir("store_concurrent_apply_bad");
    auto writer = make_writer(dir, manual_options(EXTENT));
    auto space = writer->add_namespace(durable_spec("space", true));
    auto other = writer->add_namespace(durable_spec("other", true));
    REQUIRE(space.has_value());
    REQUIRE(other.has_value());
    const ScopeKey scope = file_scope("/data/ready.dft");
    const ScopeKey unbuilt = file_scope("/data/unbuilt.dft");

    Batch plain = writer->batch(*space, scope, 0);
    plain.put(view_of(dstd::String("k")), view_of(dstd::String("v")));
    REQUIRE(commit_waiting(*writer, std::move(plain)).has_value());
    REQUIRE(flush_writer(*writer).has_value());

    const engine::CopyChange no_building[] = {
        {*space, &scope, 0, nullptr, engine::ChangeKind::Dropped},
        {*other, &unbuilt, 0, nullptr, engine::ChangeKind::Ready}};
    CHECK_FALSE(writer->apply(no_building).has_value());
    const engine::CopyChange twice[] = {
        {*space, &scope, 0, nullptr, engine::ChangeKind::Dropped},
        {*space, &scope, 3, nullptr, engine::ChangeKind::Failed}};
    CHECK_FALSE(writer->apply(twice).has_value());
    const engine::CopyChange no_scope[] = {
        {*space, &scope, 0, nullptr, engine::ChangeKind::Dropped},
        {*other, nullptr, 0, nullptr, engine::ChangeKind::Failed}};
    CHECK_FALSE(writer->apply(no_scope).has_value());
    const engine::CopyChange unknown[] = {
        {*space, &scope, 0, nullptr, engine::ChangeKind::Dropped},
        {engine::NamespaceId{99}, &scope, 0, nullptr,
         engine::ChangeKind::Failed}};
    CHECK_FALSE(writer->apply(unknown).has_value());
    REQUIRE(flush_writer(*writer).has_value());

    auto snapshot = engine::Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->copy_info(durable_spec("space", true), scope).state ==
          ScopeState::Ready);
    CHECK(snapshot->copy_info(durable_spec("other", true), unbuilt).state ==
          ScopeState::Absent);

    const engine::CopyChange fine[] = {
        {*space, &scope, 0, nullptr, engine::ChangeKind::Dropped}};
    REQUIRE(writer->apply(fine).has_value());
    REQUIRE(close_writer(*writer).has_value());
}

TEST_CASE("a publish with one bad target publishes nothing") {
    dftu_utils_test::ScopedTestDir dir("store_concurrent_publish_bad");
    auto writer = make_writer(dir, manual_options(EXTENT));
    auto space = writer->add_namespace(durable_spec("space", true));
    REQUIRE(space.has_value());
    const ScopeKey building = file_scope("/data/building.dft");
    const ScopeKey ready = file_scope("/data/ready.dft");

    Batch staged = writer->building_batch(*space, building);
    staged.put(view_of(dstd::String("k")), view_of(dstd::String("v")));
    REQUIRE(commit_waiting(*writer, std::move(staged)).has_value());
    Batch plain = writer->batch(*space, ready, 0);
    plain.put(view_of(dstd::String("k")), view_of(dstd::String("v")));
    REQUIRE(commit_waiting(*writer, std::move(plain)).has_value());
    REQUIRE(flush_writer(*writer).has_value());

    const engine::CopyChange targets[] = {{*space, &building, 1, nullptr},
                                          {*space, &ready, 1, nullptr}};
    CHECK_FALSE(writer->apply(targets).has_value());
    const engine::CopyChange twice[] = {{*space, &building, 1, nullptr},
                                        {*space, &building, 1, nullptr}};
    CHECK_FALSE(writer->apply(twice).has_value());
    REQUIRE(flush_writer(*writer).has_value());

    auto snapshot = engine::Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->copy_info(durable_spec("space", true), building).state !=
          ScopeState::Ready);
    REQUIRE(writer->apply({targets, 1}).has_value());
    REQUIRE(close_writer(*writer).has_value());
}

TEST_CASE("drops and failures from many threads keep the generation rules") {
    constexpr int THREADS = 8;
    constexpr int ROUNDS = 10;
    constexpr int KEYS = 3;

    dftu_utils_test::ScopedTestDir dir("store_concurrent_generations");
    auto writer = make_writer(dir, store_options(64 * 1024, 256 * 1024,
                                                 std::chrono::milliseconds(5)));
    const NamespaceSpec spec = put_spec("generations", 0);
    auto created = writer->add_namespace(spec);
    REQUIRE(created.has_value());
    const engine::NamespaceId space = *created;

    const auto scope_of = [](int thread) { return scope_path(thread + 10); };
    const auto key_of = [](int thread, int round, int key) {
        return "t" + dstd::to_string(thread) + "-r" + dstd::to_string(round) +
               "-k" + dstd::to_string(key);
    };

    std::atomic<bool> failed{false};
    dstd::Vector<dstd::Thread> workers;
    workers.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) {
        workers.emplace_back([&, t] {
            const ScopeKey scope = file_scope(scope_of(t));
            const auto commit_round = [&](int round) {
                Batch entries = writer->batch(space, scope, 0);
                for (int k = 0; k < KEYS; ++k) {
                    const dstd::String key = key_of(t, round, k);
                    entries.put(view_of(key), view_of(key));
                }
                if (!commit_waiting(*writer, std::move(entries)).has_value()) {
                    failed = true;
                }
            };
            for (int round = 0; round < ROUNDS; ++round) {
                commit_round(round);
                if (!writer->drop(space, scope).has_value()) failed = true;
                if (!writer->fail(space, scope, 0).has_value()) failed = true;
                commit_round(round);
            }
        });
    }
    for (dstd::Thread& worker : workers) worker.join();
    REQUIRE_FALSE(failed.load());

    REQUIRE(close_writer(*writer).has_value());

    const dstd::String path = dstd::String(writer->path());
    dstd::Vector<std::uint64_t> generations = copy_generations(path);
    const dstd::Vector<std::uint64_t> drops = drop_generations(path);
    generations.insert(generations.end(), drops.begin(), drops.end());
    std::sort(generations.begin(), generations.end());

    // The commit that follows a drop takes the next generation, and the one
    // after a failure the next again, so one round adds three: the first commit
    // of the next round finds its copy still live and reuses it.
    const std::uint64_t last_generation = 3 * ROUNDS + 1;
    dstd::Vector<std::uint64_t> expected;
    for (int t = 0; t < THREADS; ++t) {
        for (std::uint64_t generation = 1; generation <= last_generation;
             ++generation) {
            expected.push_back(generation);
        }
    }
    std::sort(expected.begin(), expected.end());
    // A footer repeats the items of the chain, so a generation may be read
    // twice. Every generation of every scope appears and no other one does.
    generations.erase(std::unique(generations.begin(), generations.end()),
                      generations.end());
    dstd::Vector<std::uint64_t> distinct_expected;
    for (std::uint64_t generation = 1; generation <= last_generation;
         ++generation) {
        distinct_expected.push_back(generation);
    }
    CHECK(generations == distinct_expected);

    auto snapshot = engine::Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    for (int t = 0; t < THREADS; ++t) {
        const ScopeKey scope = file_scope(scope_of(t));
        REQUIRE(snapshot->state(spec, scope) == ScopeState::Ready);
        auto cursor = snapshot->scan(spec, scope, {}, {});
        const dstd::Vector<Row> rows = collect(cursor);
        REQUIRE(rows.size() == static_cast<std::size_t>(KEYS));
        dstd::Vector<dstd::String> keys;
        for (const Row& row : rows) keys.push_back(row.key);
        std::sort(keys.begin(), keys.end());
        dstd::Vector<dstd::String> expected_keys;
        for (int k = 0; k < KEYS; ++k) {
            expected_keys.push_back(key_of(t, ROUNDS - 1, k));
        }
        std::sort(expected_keys.begin(), expected_keys.end());
        CHECK(keys == expected_keys);
    }
}

TEST_CASE("a writer destroyed without close drains its extent writes") {
    dftu_utils_test::ScopedTestDir dir("store_concurrent_teardown");
    const std::uint64_t before = engine::CommitterTestHook::live_batches();
    {
        auto writer = make_writer(dir, manual_options(EXTENT));
        const NamespaceSpec spec = put_spec("teardown", 0);
        auto created = writer->add_namespace(spec);
        REQUIRE(created.has_value());
        const ScopeKey scope = file_scope("/data/teardown.dft");
        // A value longer than an extent makes every batch a run of its own, so
        // writes are in flight while the writer is destroyed, and the last one
        // stays in an open extent no record names.
        const dstd::String big_value(5000, 't');
        for (int i = 0; i < 64; ++i) {
            Batch entries = writer->batch(*created, scope, 0);
            entries.put(view_of("k" + dstd::to_string(i)), view_of(big_value));
            REQUIRE(commit_waiting(*writer, std::move(entries)).has_value());
        }
        Batch open_extent = writer->batch(*created, scope, 0);
        open_extent.put(view_of(dstd::String("open")),
                        view_of(dstd::String("value")));
        REQUIRE(commit_waiting(*writer, std::move(open_extent)).has_value());
        CHECK(engine::CommitterTestHook::live_batches() > before);

        writer.reset();
    }
    // A completion that lands after the destructor would write into the batch
    // it carries, so every batch is gone once it returned.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(engine::CommitterTestHook::live_batches() == before);
}

TEST_CASE("a failed extent write deletes the batches of its extent") {
    dftu_utils_test::ScopedTestDir dir("store_concurrent_failed_extent");
    const std::uint64_t before = engine::CommitterTestHook::live_batches();
    auto writer = make_writer(dir, manual_options(EXTENT));
    const NamespaceSpec spec = put_spec("failed_extent", 0);
    auto created = writer->add_namespace(spec);
    REQUIRE(created.has_value());
    const ScopeKey scope = file_scope("/data/failed_extent.dft");

    Batch entries = writer->batch(*created, scope, 0);
    entries.put(view_of(dstd::String("key")), view_of(dstd::String("value")));
    REQUIRE(commit_waiting(*writer, std::move(entries)).has_value());
    CHECK(engine::CommitterTestHook::live_batches() == before + 1);

    store::engine::DataFileWriterTestHook::reopen_read_only(
        store::engine::WriterTestHook::file(*writer),
        dstd::String(writer->path()));
    auto flushed = flush_writer(*writer);
    REQUIRE_FALSE(flushed.has_value());
    CHECK(flushed.error().is(dstd::Condition::Io));
    CHECK(engine::CommitterTestHook::live_batches() == before);
}

TEST_CASE("a failed record write latches for commit, flush and close") {
    dftu_utils_test::ScopedTestDir dir("store_concurrent_latch");
    engine::CommitterTestHook::reset();
    auto writer = make_writer(dir, manual_options(EXTENT));
    const NamespaceSpec spec = put_spec("latched", 0);
    auto created = writer->add_namespace(spec);
    REQUIRE(created.has_value());
    const engine::NamespaceId space = *created;
    const ScopeKey scope = file_scope("/data/latched.dft");

    const auto commit_one = [&](const dstd::String& key) {
        Batch entries = writer->batch(space, scope, 0);
        entries.put(view_of(key), view_of(dstd::String("value")));
        return commit_waiting(*writer, std::move(entries));
    };

    REQUIRE(commit_one("first").has_value());

    engine::CommitterTestHook::fail_next_record(true);
    auto flushed = flush_writer(*writer);
    REQUIRE_FALSE(flushed.has_value());
    CHECK(flushed.error().is(dstd::Condition::Io));

    auto committed = commit_one("second");
    REQUIRE_FALSE(committed.has_value());
    CHECK(committed.error().is(dstd::Condition::Io));

    auto flushed_again = flush_writer(*writer);
    REQUIRE_FALSE(flushed_again.has_value());
    CHECK(flushed_again.error().is(dstd::Condition::Io));

    auto closed = close_writer(*writer);
    REQUIRE_FALSE(closed.has_value());
    CHECK(closed.error().is(dstd::Condition::Io));
}
