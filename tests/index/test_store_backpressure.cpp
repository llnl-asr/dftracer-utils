#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/store/engine/data_file.h>
#include <dftracer/utils/index/store/engine/fold.h>
#include <dftracer/utils/index/store/engine/io_fault.h>
#include <dftracer/utils/index/store/engine/open_page.h>
#include <dftracer/utils/index/store/engine/snapshot.h>
#include <dftracer/utils/index/store/engine/writer.h>
#include <dftracer/utils/index/store/engine/writer_pages.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store::engine {

/// Swaps the writer's descriptor for a read-only one on the same file, so
/// every pwrite fails and the page writes are the ones that fail.
struct DataFileWriterTestHook {
    static void reopen_read_only(DataFileWriter& writer,
                                 const std::string& path) {
        if (writer.fd_ >= 0) ::close(writer.fd_);
        writer.fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
};

}  // namespace dftracer::utils::index::store::engine

namespace {

namespace coro = dftracer::utils::coro;
namespace memory = dftracer::utils::memory;
namespace dstd = dftracer::utils::dstd;
namespace engine = dftracer::utils::index::store::engine;

using dftracer::utils::ByteView;
using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::TaskHandle;
using dftracer::utils::dstd::Condition;
using engine::Batch;
using engine::DataFileReader;
using engine::FoldKind;
using engine::NamespaceId;
using engine::NamespaceSpec;
using engine::ScopeKey;
using engine::ScopeState;
using engine::Snapshot;
using engine::Writer;
using engine::WriterErrc;
using engine::WriterOptions;
using engine::WriterPagesCounters;
using engine::WriterTestHook;

constexpr std::uint32_t PAGE_BYTES = 64 * 1024;
constexpr int RUNTIME_THREADS = 8;
/// A section of one raw batch: two fit a page, so a page can stay open with
/// sections of two commits in it and nothing writing them.
constexpr std::size_t RAW_KEYS = 3;
constexpr std::size_t RAW_VALUE_BYTES = 8000;
/// A section of one fold batch of 8-byte values, about the size of a raw one.
constexpr std::size_t FOLD_KEYS = 1600;
constexpr int SCOPES = 4;
/// A commit may report Busy this many times before the test calls it stuck.
constexpr int RETRY_ATTEMPTS = 4000;
constexpr int BATCH_ATTEMPTS = 64;
constexpr std::chrono::seconds TASK_LIMIT{60};
constexpr std::chrono::seconds WAIT_LIMIT{30};

// A sanitizer build runs under a ctest timeout of 120 s (ASan) or 300 s (TSan)
// and makes a commit tens of times slower, so the thread and batch counts of
// the concurrent cases shrink there. Each case keeps its shape and every
// assertion; only the number of commits differs.
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
    dftracer::utils::write_u64_le(reinterpret_cast<std::uint8_t*>(bytes.data()),
                                  static_cast<std::uint64_t>(value));
    return bytes;
}

std::int64_t read_i64(ByteView bytes) {
    REQUIRE(bytes.size() == 8);
    return std::bit_cast<std::int64_t>(dftracer::utils::read_u64_le(
        reinterpret_cast<const std::uint8_t*>(bytes.data())));
}

/// A child pool of the calling thread's pool, with @p limit bytes or none.
memory::PoolHandle child_pool(const std::string& name, std::size_t limit) {
    return memory::current_pool().child(name, memory::PoolKind::Scratch,
                                        memory::Limits{limit});
}

/// `head` padded with 'x' to exactly `length` bytes, so every value of a raw
/// batch makes its section the size the quotas are counted in.
std::string padded(const std::string& head, std::size_t length) {
    std::string out = head;
    out.resize(length, 'x');
    return out;
}

NamespaceSpec put_spec(const std::string& name, std::uint32_t version = 0) {
    NamespaceSpec spec;
    spec.name = dstring(name);
    spec.format_version = version;
    spec.fold = FoldKind::None;
    return spec;
}

NamespaceSpec fold_spec(const std::string& name, FoldKind kind) {
    NamespaceSpec spec;
    spec.name = dstring(name);
    spec.format_version = 0;
    spec.fold = kind;
    return spec;
}

ScopeKey file_scope(const std::string& path) {
    return ScopeKey::file(dstring(path));
}

std::string scope_path(int index) {
    return "/data/backpressure-" + std::to_string(index) + ".dft";
}

/// Options that commit only when asked: neither the size nor the interval ever
/// fires, so a commit is the only thing that seals a page.
WriterOptions manual_options(std::uint64_t quota) {
    WriterOptions options;
    options.page_size = PAGE_BYTES;
    options.page_quota_bytes = quota;
    options.commit_bytes = std::uint64_t{1} << 40;
    options.commit_interval = std::chrono::hours(1);
    options.host = dstring("host");
    options.label = dstring("label");
    return options;
}

Runtime& store_runtime() {
    static Runtime runtime(RUNTIME_THREADS);
    return runtime;
}

std::unique_ptr<Writer> make_writer(const dftu_utils_test::ScopedTestDir& dir,
                                    const WriterOptions& options) {
    auto created =
        Writer::create(dir.str(), *store_runtime().executor(), options);
    REQUIRE(created.has_value());
    return std::move(*created);
}

dstd::Expected<void, dstd::Error> close_writer(Writer& writer) {
    return store_runtime().submit(writer.close()).get();
}

bool is_busy(const dstd::Error& error) {
    return error.is_code(WriterErrc::Busy);
}

/// Waits for a submitted task and fails the test instead of hanging it.
template <class Handle>
void await_handle(Handle& handle, std::chrono::seconds limit) {
    REQUIRE(handle.future.wait_for(limit) == std::future_status::ready);
    handle.wait();
}

/// Awaits space() between the commits of the test thread itself.
void await_space(Writer& writer) {
    TaskHandle handle = store_runtime().submit(writer.space(), "writer-space");
    await_handle(handle, WAIT_LIMIT);
}

/// The commit of one batch with the retry the writer documents: the bytes of
/// earlier commits are written, then the same batch commits again (a Busy
/// commit left it whole). The attempts are bounded so a writer that never
/// makes progress fails the test instead of hanging it.
coro::CoroTask<dstd::Expected<void, dstd::Error>> commit_retrying(
    Writer& writer, Batch batch, int attempts) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        auto committed = writer.commit(std::move(batch));
        if (committed.has_value() || !is_busy(committed.error())) {
            co_return committed;
        }
        co_await writer.space();
    }
    co_return dstd::Unexpected<dstd::Error>(
        dstd::Error(WriterErrc::Busy, "the writer made no progress"));
}

/// One raw batch of `keys` entries whose values are `value_bytes` long.
template <class KeyFn, class ValueFn>
Batch raw_batch(Writer& writer, NamespaceId space, const ScopeKey& scope,
                std::size_t keys, std::size_t value_bytes, KeyFn key_of,
                ValueFn value_of) {
    Batch batch = writer.batch(space, scope, 0);
    for (std::size_t k = 0; k < keys; ++k) {
        const std::string value = padded(value_of(k), value_bytes);
        batch.put(view_of(key_of(k)), view_of(value));
    }
    return batch;
}

/// One fold batch of `keys` entries whose values are 8 bytes.
Batch fold_batch(Writer& writer, NamespaceId space, const ScopeKey& scope,
                 std::int64_t value, std::size_t keys) {
    Batch batch = writer.batch(space, scope, 0);
    for (std::size_t k = 0; k < keys; ++k) {
        const std::string key = "m" +
                                std::string(4 - std::to_string(k).size(), '0') +
                                std::to_string(k);
        const std::string bytes = i64_bytes(value);
        batch.fold(view_of(key), view_of(bytes));
    }
    return batch;
}

std::string raw_key(int thread, int round, std::size_t k) {
    return "t" + std::to_string(thread) + "-b" + std::to_string(round) + "-k" +
           std::to_string(k);
}

std::string raw_value(const std::string& tag, int thread, int round,
                      std::size_t k) {
    return tag + "-" + std::to_string(thread) + "-" + std::to_string(round) +
           "-" + std::to_string(k);
}

std::string fold_key(std::size_t k) {
    return "m" + std::string(4 - std::to_string(k).size(), '0') +
           std::to_string(k);
}

struct Row {
    std::string scope;
    std::string key;
    std::string value;
};

std::vector<Row> collect(engine::ScanCursor& cursor) {
    std::vector<Row> rows;
    while (!cursor.at_end()) {
        auto value = cursor.value();
        REQUIRE(value.has_value());
        REQUIRE_FALSE(cursor.scope().paths.empty());
        rows.push_back({to_std(cursor.scope().paths[0]), to_std(cursor.key()),
                        to_std(*value)});
        cursor.next();
    }
    return rows;
}

using RowsByScope = std::map<std::pair<std::string, std::string>, std::string>;

RowsByScope index_rows(const std::vector<Row>& rows) {
    RowsByScope out;
    for (const Row& row : rows) out[{row.scope, row.key}] = row.value;
    return out;
}

struct CopyRow {
    std::string scope;
    std::uint32_t id = 0;
    std::uint64_t generation = 0;
};

/// The copies the file announces, in the order its records carry them.
std::vector<CopyRow> read_copies(const std::string& path) {
    auto reader = DataFileReader::open(dstring(path));
    REQUIRE(reader.has_value());
    std::map<std::uint32_t, std::string> scope_paths;
    for (const engine::ScopeItem& scope : reader->index().scopes) {
        REQUIRE_FALSE(scope.paths.empty());
        scope_paths[scope.local_id] = to_std(scope.paths[0].logical_path);
    }
    std::vector<CopyRow> out;
    for (const engine::CopyItem& copy : reader->index().copies) {
        out.push_back(
            {scope_paths[copy.scope_id], copy.local_id, copy.generation});
    }
    return out;
}

std::uint32_t copy_id_of(const std::vector<CopyRow>& copies,
                         const std::string& scope) {
    for (const CopyRow& copy : copies) {
        if (copy.scope == scope) return copy.id;
    }
    return 0;
}

}  // namespace

TEST_CASE("back pressure: 64 coroutines commit over a two page quota") {
    constexpr int COROS = SANITIZED ? 6 : 64;
    constexpr int ROUNDS = SANITIZED ? 4 : 20;
    constexpr std::uint64_t QUOTA = 2 * PAGE_BYTES;

    dftu_utils_test::ScopedTestDir dir("store_backpressure_many");
    auto writer = make_writer(dir, manual_options(QUOTA));

    auto created = writer->add_namespace(put_spec("raw-a", 0));
    REQUIRE(created.has_value());
    const NamespaceId raw_a = *created;
    created = writer->add_namespace(put_spec("raw-b", 1));
    REQUIRE(created.has_value());
    const NamespaceId raw_b = *created;
    created = writer->add_namespace(fold_spec("sums", FoldKind::SumI64));
    REQUIRE(created.has_value());
    const NamespaceId sums = *created;

    const auto scope_of = [](int thread, int round) {
        return scope_path((thread * 31 + round * 7) % SCOPES);
    };

    RowsByScope expected_a;
    RowsByScope expected_b;
    // A scan of a fold namespace folds the scopes together, so the sums are
    // keyed by key alone.
    std::map<std::string, std::int64_t> expected_sum;
    for (int t = 0; t < COROS; ++t) {
        for (int r = 0; r < ROUNDS; ++r) {
            const std::string scope = scope_of(t, r);
            for (std::size_t k = 0; k < RAW_KEYS; ++k) {
                expected_a[{scope, raw_key(t, r, k)}] =
                    padded(raw_value("a", t, r, k), RAW_VALUE_BYTES);
                expected_b[{scope, raw_key(t, r, k)}] =
                    padded(raw_value("b", t, r, k), RAW_VALUE_BYTES);
            }
            for (std::size_t k = 0; k < FOLD_KEYS; ++k) {
                expected_sum[fold_key(k)] += t + 1;
            }
        }
    }

    std::atomic<int> errors{0};
    TaskHandle handle = store_runtime().scope(
        "backpressure", [&](CoroScope& scope) -> coro::CoroTask<void> {
            for (int t = 0; t < COROS; ++t) {
                scope.spawn([&, t](CoroScope&) -> coro::CoroTask<void> {
                    for (int r = 0; r < ROUNDS; ++r) {
                        const ScopeKey scope_key = file_scope(scope_of(t, r));
                        const auto key_of = [t, r](std::size_t k) {
                            return raw_key(t, r, k);
                        };
                        auto a = co_await commit_retrying(
                            *writer,
                            raw_batch(*writer, raw_a, scope_key, RAW_KEYS,
                                      RAW_VALUE_BYTES, key_of,
                                      [t, r](std::size_t k) {
                                          return raw_value("a", t, r, k);
                                      }),
                            RETRY_ATTEMPTS);
                        if (!a.has_value()) errors.fetch_add(1);
                        auto b = co_await commit_retrying(
                            *writer,
                            raw_batch(*writer, raw_b, scope_key, RAW_KEYS,
                                      RAW_VALUE_BYTES, key_of,
                                      [t, r](std::size_t k) {
                                          return raw_value("b", t, r, k);
                                      }),
                            RETRY_ATTEMPTS);
                        if (!b.has_value()) errors.fetch_add(1);
                        auto sum = co_await commit_retrying(
                            *writer,
                            fold_batch(*writer, sums, scope_key, t + 1,
                                       FOLD_KEYS),
                            RETRY_ATTEMPTS);
                        if (!sum.has_value()) errors.fetch_add(1);
                    }
                });
            }
            co_await scope.join();
        });
    await_handle(handle, TASK_LIMIT);
    CHECK(errors.load() == 0);

    REQUIRE(close_writer(*writer).has_value());

    const WriterPagesCounters counters = WriterTestHook::pages(*writer);
    CHECK(counters.quota == QUOTA);
    // The quota bound the bytes in flight, so a commit had to wait for a write.
    CHECK(counters.busy > 0);
    // A section here is smaller than a page, so no take ran past the quota.
    CHECK(counters.over_quota == 0);
    CHECK(counters.peak_held <= counters.quota + PAGE_BYTES);
    CHECK(counters.held == 0);
    CHECK(counters.reserved == 0);

    auto snapshot = Snapshot::open(dir.str());
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
    std::map<std::string, std::int64_t> got_sum;
    const std::vector<Row> sum_rows = collect(sums_cursor);
    for (const Row& row : sum_rows)
        got_sum[row.key] = read_i64(view_of(row.value));
    CHECK(got_sum.size() == expected_sum.size());
    CHECK(got_sum == expected_sum);
}

TEST_CASE("back pressure: a one page quota holds one page at a time") {
    constexpr int BATCHES = SANITIZED ? 8 : 40;

    dftu_utils_test::ScopedTestDir dir("store_backpressure_quota");
    auto writer = make_writer(dir, manual_options(PAGE_BYTES));

    auto created = writer->add_namespace(put_spec("quota", 0));
    REQUIRE(created.has_value());
    const NamespaceId space = *created;
    const ScopeKey scope = file_scope(scope_path(0));

    const auto key_of = [](int batch, std::size_t k) {
        return "q" + std::to_string(batch) + "-k" + std::to_string(k);
    };
    const auto value_of = [](int batch, std::size_t k) {
        return "v" + std::to_string(batch) + "-" + std::to_string(k);
    };

    RowsByScope expected;
    for (int b = 0; b < BATCHES; ++b) {
        for (std::size_t k = 0; k < RAW_KEYS; ++k) {
            expected[{to_std(scope.paths[0]), key_of(b, k)}] =
                padded(value_of(b, k), RAW_VALUE_BYTES);
        }
    }

    std::atomic<int> errors{0};
    TaskHandle handle = store_runtime().scope(
        "quota", [&](CoroScope& scope_runner) -> coro::CoroTask<void> {
            scope_runner.spawn([&](CoroScope&) -> coro::CoroTask<void> {
                for (int b = 0; b < BATCHES; ++b) {
                    auto done = co_await commit_retrying(
                        *writer,
                        raw_batch(
                            *writer, space, scope, RAW_KEYS, RAW_VALUE_BYTES,
                            [&, b](std::size_t k) { return key_of(b, k); },
                            [&, b](std::size_t k) { return value_of(b, k); }),
                        RETRY_ATTEMPTS);
                    if (!done.has_value()) errors.fetch_add(1);
                }
            });
            co_await scope_runner.join();
        });
    await_handle(handle, TASK_LIMIT);
    CHECK(errors.load() == 0);

    const WriterPagesCounters before = WriterTestHook::pages(*writer);
    CHECK(before.quota == PAGE_BYTES);
    // One batch is held and the next does not fit it, so a commit waited.
    CHECK(before.busy > 0);
    CHECK(before.over_quota == 0);
    CHECK(before.peak_held <= before.quota);

    REQUIRE(close_writer(*writer).has_value());

    const WriterPagesCounters after = WriterTestHook::pages(*writer);
    CHECK(after.held == 0);
    CHECK(after.reserved == 0);

    auto snapshot = Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    auto cursor = snapshot->scan_all(put_spec("quota", 0), {}, {});
    const RowsByScope got = index_rows(collect(cursor));
    CHECK(got.size() == expected.size());
    CHECK(got == expected);

    // A zero quota is a mistake, not a request for no bound.
    dftu_utils_test::ScopedTestDir other("store_backpressure_zero_quota");
    auto refused = Writer::create(other.str(), *store_runtime().executor(),
                                  manual_options(0));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().is(Condition::InvalidArgument));
}

TEST_CASE("back pressure: a Busy commit assigns no copy id or generation") {
    dftu_utils_test::ScopedTestDir dir("store_backpressure_busy");
    auto writer = make_writer(dir, manual_options(PAGE_BYTES));

    auto created = writer->add_namespace(put_spec("busy", 0));
    REQUIRE(created.has_value());
    const NamespaceId space = *created;
    // The refused commit names the second scope, so its copy id only shows
    // whether the commit that was Busy consumed one.
    const ScopeKey refused_scope = file_scope(scope_path(0));
    const ScopeKey first_scope = file_scope(scope_path(1));
    const ScopeKey third_scope = file_scope(scope_path(2));

    // Two of these do not fit the one page quota, and one leaves its page open
    // with room for the small third batch.
    constexpr std::size_t BUSY_KEYS = 6;
    constexpr std::size_t BUSY_VALUE_BYTES = 8000;
    const auto one = [&](const ScopeKey& scope, const std::string& tag) {
        return raw_batch(
            *writer, space, scope, BUSY_KEYS, BUSY_VALUE_BYTES,
            [&tag](std::size_t k) { return tag + "-k" + std::to_string(k); },
            [&tag](std::size_t k) { return tag + "-" + std::to_string(k); });
    };

    REQUIRE(writer->commit(one(first_scope, "first")).has_value());

    // One batch, about three quarters of the quota, is held; the next does not
    // fit, and nothing has sealed the open page yet.
    Batch refused = one(refused_scope, "refused");
    auto blocked = writer->commit(std::move(refused));
    REQUIRE_FALSE(blocked.has_value());
    REQUIRE(is_busy(blocked.error()));

    // A third commit that fits the page the first one left open, so the page
    // holds the copies of both and the refused scope is still unknown.
    REQUIRE(writer
                ->commit(raw_batch(
                    *writer, space, third_scope, 1, 64,
                    [](std::size_t) { return std::string("third"); },
                    [](std::size_t) { return std::string("third"); }))
                .has_value());

    await_space(*writer);

    // The refused batch is whole, so the retry commits what the Busy commit
    // left untouched.
    auto retried = writer->commit(std::move(refused));
    REQUIRE(retried.has_value());

    REQUIRE(close_writer(*writer).has_value());

    const std::vector<CopyRow> copies = read_copies(to_std(writer->path()));
    REQUIRE(copies.size() == 3);
    // The ids follow the commits that succeeded: the commit that was Busy
    // consumed none, so the copy of its retry is the last.
    CHECK(copy_id_of(copies, scope_path(1)) == 1);
    CHECK(copy_id_of(copies, scope_path(2)) == 2);
    CHECK(copy_id_of(copies, scope_path(0)) == 3);
    for (const CopyRow& copy : copies) CHECK(copy.generation == 1);

    auto snapshot = Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->state(put_spec("busy", 0), refused_scope) ==
            ScopeState::Ready);
    for (const ScopeKey& scope : {first_scope, refused_scope}) {
        const std::string tag = scope == first_scope ? "first" : "refused";
        for (std::size_t k = 0; k < BUSY_KEYS; ++k) {
            const std::string key = tag + "-k" + std::to_string(k);
            const std::string expected =
                padded(tag + "-" + std::to_string(k), BUSY_VALUE_BYTES);
            auto value =
                snapshot->get(put_spec("busy", 0), scope, view_of(key));
            REQUIRE(value.has_value());
            REQUIRE(value->has_value());
            CHECK(to_std((*value)->bytes()) == expected);
        }
    }
    auto third =
        snapshot->get(put_spec("busy", 0), third_scope, view_of("third"));
    REQUIRE(third.has_value());
    REQUIRE(third->has_value());
    CHECK(to_std((*third)->bytes()) == padded("third", 64));
}

TEST_CASE("back pressure: a batch larger than the quota is taken over it") {
    constexpr std::size_t BIG_KEYS = 8;
    constexpr std::size_t BIG_VALUE_BYTES = 9000;

    dftu_utils_test::ScopedTestDir dir("store_backpressure_over");
    auto writer = make_writer(dir, manual_options(PAGE_BYTES));

    auto created = writer->add_namespace(put_spec("over", 0));
    REQUIRE(created.has_value());
    const NamespaceId space = *created;
    const ScopeKey scope = file_scope(scope_path(0));

    const auto key_of = [](std::size_t k) {
        return "big-k" + std::to_string(k);
    };
    const auto value_of = [](std::size_t k) {
        return "big-" + std::to_string(k);
    };

    // Nothing is held, so no write can free room: the batch is taken past the
    // quota rather than refused for a wait nobody can end.
    auto taken = writer->commit(raw_batch(*writer, space, scope, BIG_KEYS,
                                          BIG_VALUE_BYTES, key_of, value_of));
    REQUIRE(taken.has_value());

    const WriterPagesCounters counters = WriterTestHook::pages(*writer);
    CHECK(counters.quota == PAGE_BYTES);
    CHECK(counters.over_quota == 1);
    CHECK(counters.peak_held > counters.quota);
    CHECK(counters.busy == 0);

    std::atomic<int> errors{0};
    TaskHandle handle = store_runtime().scope(
        "over", [&](CoroScope& scope_runner) -> coro::CoroTask<void> {
            scope_runner.spawn([&](CoroScope&) -> coro::CoroTask<void> {
                auto small = co_await commit_retrying(
                    *writer,
                    raw_batch(
                        *writer, space, scope, 1, 64,
                        [](std::size_t) { return std::string("after"); },
                        [](std::size_t) { return std::string("after"); }),
                    BATCH_ATTEMPTS);
                if (!small.has_value()) errors.fetch_add(1);
            });
            co_await scope_runner.join();
        });
    await_handle(handle, TASK_LIMIT);
    CHECK(errors.load() == 0);

    REQUIRE(close_writer(*writer).has_value());

    auto snapshot = Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    for (std::size_t k = 0; k < BIG_KEYS; ++k) {
        auto value =
            snapshot->get(put_spec("over", 0), scope, view_of(key_of(k)));
        REQUIRE(value.has_value());
        REQUIRE(value->has_value());
        CHECK(to_std((*value)->bytes()) ==
              padded(value_of(k), BIG_VALUE_BYTES));
    }
    auto after = snapshot->get(put_spec("over", 0), scope,
                               view_of(std::string("after")));
    REQUIRE(after.has_value());
    REQUIRE(after->has_value());
    CHECK(to_std((*after)->bytes()) == padded("after", 64));
}

TEST_CASE("back pressure: the held page bytes are reserved in the pool") {
    constexpr int COROS = SANITIZED ? 4 : 16;
    constexpr int BATCHES = SANITIZED ? 2 : 8;
    constexpr std::uint64_t QUOTA = 8 * PAGE_BYTES;

    dftu_utils_test::ScopedTestDir dir("store_backpressure_budget");
    memory::PoolHandle budget_pool = child_pool("backpressure budget", 0);
    REQUIRE(static_cast<bool>(budget_pool));

    std::unique_ptr<Writer> writer;
    {
        // The writer's own pool is a child of this one, so its charges are
        // counted here.
        memory::Scope pool_scope(*budget_pool.get());
        auto created = Writer::create(dir.str(), *store_runtime().executor(),
                                      manual_options(QUOTA));
        REQUIRE(created.has_value());
        writer = std::move(*created);
    }

    auto created = writer->add_namespace(put_spec("budget", 0));
    REQUIRE(created.has_value());
    const NamespaceId space = *created;
    const auto scope_of = [](int thread) {
        return scope_path(thread % SCOPES);
    };
    const auto key_of = [](int thread, int batch, std::size_t k) {
        return "t" + std::to_string(thread) + "-b" + std::to_string(batch) +
               "-k" + std::to_string(k);
    };
    const auto value_of = [](int thread, int batch, std::size_t k) {
        return "v" + std::to_string(thread) + "-" + std::to_string(batch) +
               "-" + std::to_string(k);
    };

    RowsByScope expected;
    for (int t = 0; t < COROS; ++t) {
        for (int b = 0; b < BATCHES; ++b) {
            for (std::size_t k = 0; k < RAW_KEYS; ++k) {
                expected[{scope_of(t), key_of(t, b, k)}] =
                    padded(value_of(t, b, k), RAW_VALUE_BYTES);
            }
        }
    }

    std::atomic<int> errors{0};
    std::atomic<int> charged_while_held{0};
    std::atomic<int> uncharged_while_held{0};
    TaskHandle handle = store_runtime().scope(
        "budget", [&](CoroScope& scope_runner) -> coro::CoroTask<void> {
            for (int t = 0; t < COROS; ++t) {
                scope_runner.spawn([&, t](CoroScope&) -> coro::CoroTask<void> {
                    for (int b = 0; b < BATCHES; ++b) {
                        auto done = co_await commit_retrying(
                            *writer,
                            raw_batch(
                                *writer, space, file_scope(scope_of(t)),
                                RAW_KEYS, RAW_VALUE_BYTES,
                                [&, t, b](std::size_t k) {
                                    return key_of(t, b, k);
                                },
                                [&, t, b](std::size_t k) {
                                    return value_of(t, b, k);
                                }),
                            RETRY_ATTEMPTS);
                        if (!done.has_value()) errors.fetch_add(1);
                        // A commit leaves its section in an open page, so the
                        // bytes it charged are held at this point.
                        const WriterPagesCounters counters =
                            WriterTestHook::pages(*writer);
                        if (counters.held == 0) continue;
                        if (counters.reserved >= counters.held) {
                            charged_while_held.fetch_add(1);
                        } else {
                            uncharged_while_held.fetch_add(1);
                        }
                    }
                });
            }
            co_await scope_runner.join();
        });
    await_handle(handle, TASK_LIMIT);
    CHECK(errors.load() == 0);
    // Every byte held is a byte the writer's pool holds for it, and nothing
    // ran past the quota, so the pool admitted every charge.
    CHECK(charged_while_held.load() > 0);
    CHECK(uncharged_while_held.load() == 0);

    const WriterPagesCounters held = WriterTestHook::pages(*writer);
    CHECK(held.over_quota == 0);
    CHECK(held.peak_held <= held.quota);

    REQUIRE(close_writer(*writer).has_value());

    const WriterPagesCounters after = WriterTestHook::pages(*writer);
    CHECK(after.held == 0);
    CHECK(after.reserved == 0);

    auto snapshot = Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    auto cursor = snapshot->scan_all(put_spec("budget", 0), {}, {});
    const RowsByScope got = index_rows(collect(cursor));
    CHECK(got.size() == expected.size());
    CHECK(got == expected);
}

TEST_CASE("back pressure: a pool that refuses every charge still completes") {
    constexpr int BATCHES = 4;
    constexpr std::uint64_t QUOTA = 8 * PAGE_BYTES;
    // Every charge of a child pool is counted against the pool above it in
    // whole mebibytes (PoolAccess::settle), the smallest of them as one: a
    // three page limit is below that, so it refuses every charge.
    constexpr std::size_t LIMIT_BYTES = 3 * PAGE_BYTES;

    dftu_utils_test::ScopedTestDir dir("store_backpressure_refused");
    memory::PoolHandle budget_pool = child_pool("refused budget", LIMIT_BYTES);
    REQUIRE(static_cast<bool>(budget_pool));

    std::unique_ptr<Writer> writer;
    {
        memory::Scope pool_scope(*budget_pool.get());
        auto created = Writer::create(dir.str(), *store_runtime().executor(),
                                      manual_options(QUOTA));
        REQUIRE(created.has_value());
        writer = std::move(*created);
    }

    auto created = writer->add_namespace(put_spec("refused", 0));
    REQUIRE(created.has_value());
    const NamespaceId space = *created;
    const ScopeKey scope = file_scope(scope_path(0));
    const auto key_of = [](int batch, std::size_t k) {
        return "r" + std::to_string(batch) + "-k" + std::to_string(k);
    };
    const auto value_of = [](int batch, std::size_t k) {
        return "v" + std::to_string(batch) + "-" + std::to_string(k);
    };

    RowsByScope expected;
    for (int b = 0; b < BATCHES; ++b) {
        for (std::size_t k = 0; k < RAW_KEYS; ++k) {
            expected[{to_std(scope.paths[0]), key_of(b, k)}] =
                padded(value_of(b, k), RAW_VALUE_BYTES);
        }
    }

    std::atomic<int> errors{0};
    TaskHandle handle = store_runtime().scope(
        "refused", [&](CoroScope& scope_runner) -> coro::CoroTask<void> {
            scope_runner.spawn([&](CoroScope&) -> coro::CoroTask<void> {
                for (int b = 0; b < BATCHES; ++b) {
                    auto done = co_await commit_retrying(
                        *writer,
                        raw_batch(
                            *writer, space, scope, RAW_KEYS, RAW_VALUE_BYTES,
                            [&, b](std::size_t k) { return key_of(b, k); },
                            [&, b](std::size_t k) { return value_of(b, k); }),
                        RETRY_ATTEMPTS);
                    if (!done.has_value()) errors.fetch_add(1);
                }
            });
            co_await scope_runner.join();
        });
    await_handle(handle, TASK_LIMIT);
    CHECK(errors.load() == 0);

    // The pool refused every charge, so every batch either waited for the one
    // before it or was taken past the quota, and each of them completed.
    const WriterPagesCounters counters = WriterTestHook::pages(*writer);
    CHECK(counters.over_quota == BATCHES);
    CHECK(counters.busy > 0);
    CHECK(counters.reserved == 0);

    REQUIRE(close_writer(*writer).has_value());
    CHECK(WriterTestHook::pages(*writer).held == 0);

    auto snapshot = Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    auto cursor = snapshot->scan_all(put_spec("refused", 0), {}, {});
    const RowsByScope got = index_rows(collect(cursor));
    CHECK(got.size() == expected.size());
    CHECK(got == expected);
}

TEST_CASE("back pressure: a latched page error ends space() and commit") {
    dftu_utils_test::ScopedTestDir dir("store_backpressure_latched");
    auto writer = make_writer(dir, manual_options(8 * PAGE_BYTES));

    auto created = writer->add_namespace(put_spec("latched", 0));
    REQUIRE(created.has_value());
    const NamespaceId space = *created;
    const ScopeKey scope = file_scope(scope_path(0));

    const auto one = [&](const std::string& tag) {
        return raw_batch(
            *writer, space, scope, RAW_KEYS, RAW_VALUE_BYTES,
            [&tag](std::size_t k) { return tag + "-k" + std::to_string(k); },
            [&tag](std::size_t k) { return tag + "-" + std::to_string(k); });
    };

    REQUIRE(writer->commit(one("held")).has_value());
    const WriterPagesCounters before = WriterTestHook::pages(*writer);
    CHECK(before.held > 0);

    // Every page write fails from here on, so the batch above is discarded:
    // its bytes go back and the count latches.
    engine::DataFileWriterTestHook::reopen_read_only(
        WriterTestHook::file(*writer), to_std(writer->path()));

    auto flushed = store_runtime().submit(writer->flush());
    await_handle(flushed, WAIT_LIMIT);
    const auto flush_result = flushed.get();
    REQUIRE_FALSE(flush_result.has_value());
    CHECK(flush_result.error().is(Condition::Io));

    const WriterPagesCounters failed = WriterTestHook::pages(*writer);
    CHECK(failed.held == 0);
    CHECK(failed.reservation == 0);

    auto refused = writer->commit(one("second"));
    REQUIRE_FALSE(refused.has_value());
    CHECK_FALSE(is_busy(refused.error()));
    CHECK(refused.error().is(Condition::Io));

    // The latch ends every later wait at once.
    await_space(*writer);
    await_space(*writer);

    auto closed = store_runtime().submit(writer->close());
    await_handle(closed, WAIT_LIMIT);
    CHECK_FALSE(closed.get().has_value());
}

TEST_CASE("back pressure: a page write that fails ends a wait already parked") {
    dftu_utils_test::ScopedTestDir dir("store_backpressure_parked");
    auto writer = make_writer(dir, manual_options(8 * PAGE_BYTES));

    auto created = writer->add_namespace(put_spec("parked", 0));
    REQUIRE(created.has_value());
    const NamespaceId space = *created;
    const ScopeKey scope = file_scope(scope_path(0));

    REQUIRE(writer
                ->commit(raw_batch(
                    *writer, space, scope, RAW_KEYS, RAW_VALUE_BYTES,
                    [](std::size_t k) { return "k" + std::to_string(k); },
                    [](std::size_t k) { return "v" + std::to_string(k); }))
                .has_value());
    CHECK(WriterTestHook::pages(*writer).held > 0);

    // space() seals the open page and parks before its write fails; nothing
    // else touches the writer, so only the failed write can end the wait.
    engine::DataFileWriterTestHook::reopen_read_only(
        WriterTestHook::file(*writer), to_std(writer->path()));
    await_space(*writer);

    const WriterPagesCounters after = WriterTestHook::pages(*writer);
    CHECK(after.held == 0);
    CHECK(after.reservation == 0);

    auto closed = store_runtime().submit(writer->close());
    await_handle(closed, WAIT_LIMIT);
    CHECK_FALSE(closed.get().has_value());
}

TEST_CASE("back pressure: a pool limit below the quota refuses some charges") {
    // 24 KiB sections in many namespaces stay in their open pages (two fit a
    // page and nothing seals them), so the held bytes grow with nothing in
    // flight until the pool limit, not the quota, refuses a charge.
    constexpr std::size_t LIMIT_BYTES = std::size_t{1} << 20;
    constexpr int NAMESPACES = 40;
    constexpr int SECTIONS_PER_NAMESPACE = 2;

    dftu_utils_test::ScopedTestDir dir("store_backpressure_limit");
    memory::PoolHandle budget_pool = child_pool("limit budget", LIMIT_BYTES);
    REQUIRE(static_cast<bool>(budget_pool));

    std::unique_ptr<Writer> writer;
    {
        memory::Scope pool_scope(*budget_pool.get());
        auto created = Writer::create(dir.str(), *store_runtime().executor(),
                                      manual_options(16 * LIMIT_BYTES));
        REQUIRE(created.has_value());
        writer = std::move(*created);
    }

    std::vector<NamespaceId> spaces;
    for (int n = 0; n < NAMESPACES; ++n) {
        auto created =
            writer->add_namespace(put_spec("limit-" + std::to_string(n), 0));
        REQUIRE(created.has_value());
        spaces.push_back(*created);
    }
    const ScopeKey scope = file_scope(scope_path(0));
    const auto key_of = [](int n, int s, std::size_t k) {
        return "n" + std::to_string(n) + "-s" + std::to_string(s) + "-k" +
               std::to_string(k);
    };
    const auto make = [&](int n, int s) {
        return raw_batch(
            *writer, spaces[static_cast<std::size_t>(n)], scope, RAW_KEYS,
            RAW_VALUE_BYTES,
            [&, n, s](std::size_t k) { return key_of(n, s, k); },
            [&, n, s](std::size_t k) { return key_of(n, s, k) + "-v"; });
    };

    int taken = 0;
    bool refused = false;
    for (int s = 0; s < SECTIONS_PER_NAMESPACE && !refused; ++s) {
        for (int n = 0; n < NAMESPACES && !refused; ++n) {
            auto committed = writer->commit(make(n, s));
            if (committed.has_value()) {
                ++taken;
                continue;
            }
            REQUIRE(is_busy(committed.error()));
            refused = true;
        }
    }
    const WriterPagesCounters at_refusal = WriterTestHook::pages(*writer);
    CHECK(refused);
    CHECK(taken > 0);
    CHECK(at_refusal.busy == 1);
    CHECK(at_refusal.over_quota == 0);
    CHECK(at_refusal.held <= LIMIT_BYTES);
    CHECK(at_refusal.reservation == at_refusal.held);

    // Every batch still commits once space() lets the held bytes go.
    std::atomic<int> errors{0};
    TaskHandle handle = store_runtime().scope(
        "limit", [&](CoroScope& runner) -> coro::CoroTask<void> {
            runner.spawn([&](CoroScope&) -> coro::CoroTask<void> {
                int index = 0;
                for (int s = 0; s < SECTIONS_PER_NAMESPACE; ++s) {
                    for (int n = 0; n < NAMESPACES; ++n, ++index) {
                        if (index < taken) continue;
                        auto done = co_await commit_retrying(
                            *writer, make(n, s), RETRY_ATTEMPTS);
                        if (!done.has_value()) errors.fetch_add(1);
                    }
                }
            });
            co_await runner.join();
        });
    await_handle(handle, TASK_LIMIT);
    CHECK(errors.load() == 0);

    REQUIRE(close_writer(*writer).has_value());
    const WriterPagesCounters after = WriterTestHook::pages(*writer);
    CHECK(after.held == 0);
    CHECK(after.over_released == 0);

    auto snapshot = Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    for (int n = 0; n < NAMESPACES; ++n) {
        auto cursor = snapshot->scan_all(
            put_spec("limit-" + std::to_string(n), 0), {}, {});
        CHECK(collect(cursor).size() == SECTIONS_PER_NAMESPACE * RAW_KEYS);
    }
}

TEST_CASE("back pressure: a Busy commit writes none of its big values") {
    dftu_utils_test::ScopedTestDir dir("store_backpressure_big");
    auto writer = make_writer(dir, manual_options(PAGE_BYTES / 2));

    auto created = writer->add_namespace(put_spec("big", 0));
    REQUIRE(created.has_value());
    const NamespaceId space = *created;
    const ScopeKey scope = file_scope(scope_path(0));

    // A 40 KiB section is past the half-page quota and stays in its open page:
    // it is taken past the quota and holds bytes nothing writes yet.
    Batch fill = writer->batch(space, scope, 0);
    const std::string fill_value(40 * 1024, 'f');
    fill.put(view_of(std::string("fill")), view_of(fill_value));
    REQUIRE(writer->commit(std::move(fill)).has_value());
    CHECK(WriterTestHook::pages(*writer).over_quota == 1);

    const std::string path = to_std(writer->path());
    const auto size_before = std::filesystem::file_size(path);
    Batch big = writer->batch(space, scope, 0);
    const std::string big_value(std::size_t{2} << 20, 'b');
    big.put(view_of(std::string("big")), view_of(big_value));
    auto busy = writer->commit(std::move(big));
    REQUIRE_FALSE(busy.has_value());
    CHECK(is_busy(busy.error()));
    // The big value goes to the file as the section is sealed; a Busy commit
    // sealed nothing, so the file did not grow by it.
    CHECK(std::filesystem::file_size(path) < size_before + big_value.size());

    auto retried = store_runtime().submit(
        commit_retrying(*writer, std::move(big), RETRY_ATTEMPTS));
    await_handle(retried, WAIT_LIMIT);
    CHECK(retried.get().has_value());
    REQUIRE(close_writer(*writer).has_value());

    auto snapshot = Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    auto cursor = snapshot->scan_all(put_spec("big", 0), {}, {});
    const std::vector<Row> rows = collect(cursor);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].key == "big");
    CHECK(rows[0].value == big_value);
}

TEST_CASE(
    "back pressure: an append between a waiter's seal and its park is sealed") {
    dftu_utils_test::ScopedTestDir dir("store_backpressure_announce");
    auto writer = make_writer(dir, manual_options(4 * PAGE_BYTES));

    auto created = writer->add_namespace(put_spec("announce", 0));
    REQUIRE(created.has_value());
    const NamespaceId space = *created;
    const ScopeKey scope = file_scope(scope_path(0));

    static Writer* hooked = nullptr;
    static NamespaceId hooked_space;
    static const ScopeKey* hooked_scope = nullptr;
    static bool appended = false;
    hooked = writer.get();
    hooked_space = space;
    hooked_scope = &scope;
    appended = false;

    // Nothing is held when space() seals, so its seal writes nothing. The hook
    // then commits a batch that stays in an open page; only the commit's own
    // seal, done because a wait is announced, writes it and wakes the waiter.
    WriterTestHook::set_space_sealed([](Writer& w) {
        if (&w != hooked || appended) return;
        Batch batch = w.batch(hooked_space, *hooked_scope, 0);
        const std::string value(RAW_VALUE_BYTES, 'a');
        batch.put(view_of(std::string("late")), view_of(value));
        appended = w.commit(std::move(batch)).has_value();
    });
    await_space(*writer);
    WriterTestHook::set_space_sealed(nullptr);
    CHECK(appended);
    CHECK(WriterTestHook::pages(*writer).held == 0);

    REQUIRE(close_writer(*writer).has_value());
    auto snapshot = Snapshot::open(dir.str());
    REQUIRE(snapshot.has_value());
    auto cursor = snapshot->scan_all(put_spec("announce", 0), {}, {});
    CHECK(collect(cursor).size() == 1);
}

TEST_CASE("back pressure keeps pages full under steady load") {
    // Many coroutines on one writer keep a waiter announced most of the time.
    // Sealing on every Busy or every announced wait writes pages that are
    // mostly empty; sealing only when no write is in flight keeps them full.
    constexpr int COROS = SANITIZED ? 4 : 16;
    constexpr int ROUNDS = SANITIZED ? 10 : 40;
    dftu_utils_test::ScopedTestDir dir("store_backpressure_full_pages");
    auto writer = make_writer(dir, manual_options(4 * PAGE_BYTES));
    auto created = writer->add_namespace(put_spec("full", 0));
    REQUIRE(created.has_value());
    const NamespaceId space = *created;

    namespace io_fault = engine::io_fault;
    io_fault::start_counting();
    std::atomic<int> errors{0};
    TaskHandle handle = store_runtime().scope(
        "full pages", [&](CoroScope& scope) -> coro::CoroTask<void> {
            for (int t = 0; t < COROS; ++t) {
                scope.spawn([&, t](CoroScope&) -> coro::CoroTask<void> {
                    for (int r = 0; r < ROUNDS; ++r) {
                        const ScopeKey scope_key = file_scope(scope_path(t));
                        Batch batch = raw_batch(
                            *writer, space, scope_key, RAW_KEYS,
                            RAW_VALUE_BYTES,
                            [t, r](std::size_t k) { return raw_key(t, r, k); },
                            [t, r](std::size_t k) {
                                return raw_value("f", t, r, k);
                            });
                        auto done = co_await commit_retrying(
                            *writer, std::move(batch), RETRY_ATTEMPTS);
                        if (!done.has_value()) errors.fetch_add(1);
                    }
                });
            }
            co_await scope.join();
        });
    await_handle(handle, TASK_LIMIT);
    REQUIRE(close_writer(*writer).has_value());
    const io_fault::Counts counts = io_fault::stop_counting();
    CHECK(errors.load() == 0);

    const auto page = static_cast<std::size_t>(io_fault::WriteKind::Page);
    const std::uint64_t pages = counts.writes[page];
    const std::uint64_t bytes = counts.bytes[page];
    const std::uint64_t busy = WriterTestHook::pages(*writer).busy;
    INFO("pages " << pages << " bytes " << bytes << " busy " << busy);
    REQUIRE(pages > 0);
    // The quota refused commits, so the rule was under test.
    if (!SANITIZED) CHECK(busy > 0);
    // Pages hold at least half a page on average.
    CHECK(bytes / pages >= PAGE_BYTES / 2);
}
