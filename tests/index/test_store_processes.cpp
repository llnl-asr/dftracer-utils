#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/engine/fold.h>
#include <dftracer/utils/index/store/engine/snapshot.h>
#include <dftracer/utils/index/store/engine/writer.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <signal.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace coro = dftracer::utils::coro;
namespace dstd = dftracer::utils::dstd;
namespace engine = dftracer::utils::index::store::engine;

using dftracer::utils::ByteView;
using dftracer::utils::Runtime;
using dftracer::utils::TypedTaskHandle;
using engine::Batch;
using engine::FoldKind;
using engine::NamespaceId;
using engine::NamespaceSpec;
using engine::ScanCursor;
using engine::ScopeKey;
using engine::ScopeState;
using engine::SkippedFile;
using engine::Snapshot;
using engine::Writer;
using engine::WriterErrc;
using engine::WriterOptions;

constexpr int CHILDREN = 8;
constexpr int RUNTIME_THREADS = 4;
constexpr int RAW_SCOPES = 20;
constexpr int FOLD_SCOPES = 20;
constexpr std::uint32_t PAGE_BYTES = 16 * 1024;
/// One page, so a commit whose section would cross the page is Busy and the
/// retry loop runs.
constexpr std::uint64_t PAGE_QUOTA_BYTES = PAGE_BYTES;
constexpr int RETRY_ATTEMPTS = 4000;
constexpr std::chrono::seconds CHILD_LIMIT{90};
constexpr std::chrono::seconds REAP_LIMIT{120};
constexpr std::int64_t SHARED_BASE = 7;
constexpr std::int64_t ROUND_STEP = 100;
constexpr const char* SHARED_SCOPE = "/shared/dup.pfw";

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

constexpr int RAW_BATCHES = SANITIZED ? 40 : 200;
constexpr int RAW_BATCHES_PER_SCOPE = RAW_BATCHES / RAW_SCOPES;
constexpr int FOLD_BATCHES_PER_SCOPE = SANITIZED ? 1 : 5;
constexpr int FOLD_KEYS = 2;
constexpr int DURABLE_BATCHES = SANITIZED ? 4 : 20;
constexpr int SHARED_BATCHES = SANITIZED ? 2 : 5;

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

NamespaceSpec put_spec(const std::string& name) {
    NamespaceSpec spec;
    spec.name = dstring(name);
    spec.format_version = 0;
    spec.fold = FoldKind::None;
    return spec;
}

NamespaceSpec durable_spec(const std::string& name) {
    NamespaceSpec spec = put_spec(name);
    spec.durable = true;
    return spec;
}

NamespaceSpec fold_spec(const std::string& name) {
    NamespaceSpec spec;
    spec.name = dstring(name);
    spec.format_version = 0;
    spec.fold = FoldKind::SumI64;
    return spec;
}

ScopeKey file_scope(const std::string& path) {
    return ScopeKey::file(dstring(path));
}

std::string own_scope_path(int child, int file) {
    return "/p" + std::to_string(child) + "/f" + std::to_string(file) + ".pfw";
}

std::string durable_scope_path(int child) {
    return "/p" + std::to_string(child) + "/durable.pfw";
}

std::string raw_key(int child, int file, int n) {
    return "r" + std::to_string(child) + "-" + std::to_string(file) + "-" +
           std::to_string(n);
}

std::string raw_value(int child, int file, int n, int round) {
    return "raw-c" + std::to_string(child) + "-i" + std::to_string(file) + "-" +
           raw_key(child, file, n) + "-g" + std::to_string(round);
}

std::string fold_key(int child, int file, int k) {
    return "s" + std::to_string(child) + "-" + std::to_string(file) + "-" +
           std::to_string(k);
}

std::int64_t fold_unit(int child, int round) {
    return static_cast<std::int64_t>(child + 1) +
           static_cast<std::int64_t>(round - 1) * ROUND_STEP;
}

std::string shared_key(int batch) { return "dup-" + std::to_string(batch); }

std::int64_t shared_unit(int round) {
    return SHARED_BASE + static_cast<std::int64_t>(round - 1) * ROUND_STEP;
}

std::string durable_key(int child, int n) {
    return "d" + std::to_string(child) + "-" + std::to_string(n);
}

std::string durable_value(int child, int n, int round) {
    return "dur-c" + std::to_string(child) + "-" + std::to_string(n) + "-g" +
           std::to_string(round);
}

/// A row of a scan, named by (namespace path, scope, key).
std::string row_id(const std::string& scope, const std::string& key) {
    return scope + '\x1f' + key;
}

std::string visible(const std::string& id) {
    std::string out = id;
    for (char& c : out) {
        if (c == '\x1f') c = '/';
    }
    return out;
}

using Expected = std::map<std::string, std::string>;

/// One scan, indexed by (scope, key), so the exactly-once check counts rows and
/// compares values. `expected` names every row that must appear.
struct ScanDump {
    std::size_t rows = 0;
    std::map<std::string, std::size_t> counts;
    std::map<std::string, std::string> values;

    void add(const std::string& id, const std::string& value) {
        ++rows;
        ++counts[id];
        values[id] = value;
    }

    bool exactly_once(const Expected& expected) const {
        if (rows != expected.size()) return false;
        if (counts.size() != expected.size()) return false;
        for (const auto& entry : expected) {
            const auto count = counts.find(entry.first);
            if (count == counts.end() || count->second != 1) return false;
            const auto value = values.find(entry.first);
            if (value == values.end() || value->second != entry.second) {
                return false;
            }
        }
        return true;
    }

    /// A bounded report of what the scan held against what was expected.
    std::string report(const Expected& expected) const {
        std::string out = "rows " + std::to_string(rows) + " keys " +
                          std::to_string(counts.size()) + " expected " +
                          std::to_string(expected.size()) + "\n";
        std::size_t shown = 0;
        for (const auto& entry : counts) {
            if (entry.second != 1 && shown++ < 20) {
                out += "duplicate x" + std::to_string(entry.second) + ": " +
                       visible(entry.first) + "\n";
            }
        }
        for (const auto& entry : expected) {
            const auto count = counts.find(entry.first);
            if (count == counts.end()) {
                if (shown++ < 40) {
                    out += "missing: " + visible(entry.first) + "\n";
                }
                continue;
            }
            const auto value = values.find(entry.first);
            if (value != values.end() && value->second != entry.second &&
                shown++ < 40) {
                out += "value: " + visible(entry.first) + "\n";
            }
        }
        for (const auto& entry : counts) {
            if (expected.find(entry.first) == expected.end() && shown++ < 60) {
                out += "extra: " + visible(entry.first) + "\n";
            }
        }
        return out;
    }
};

ScanDump dump_namespace(const Snapshot& snapshot, const NamespaceSpec& spec) {
    ScanDump dump;
    ScanCursor cursor = snapshot.scan_all(spec, {}, {});
    while (!cursor.at_end()) {
        auto value = cursor.value();
        REQUIRE(value.has_value());
        dump.add(row_id(to_std(cursor.scope().paths[0]), to_std(cursor.key())),
                 to_std(*value));
        cursor.next();
    }
    return dump;
}

Expected expected_raw(int round) {
    Expected out;
    for (int child = 0; child < CHILDREN; ++child) {
        for (int file = 0; file < RAW_SCOPES; ++file) {
            const std::string scope = own_scope_path(child, file);
            for (int n = 0; n < RAW_BATCHES_PER_SCOPE; ++n) {
                out[row_id(scope, raw_key(child, file, n))] =
                    raw_value(child, file, n, round);
            }
        }
    }
    return out;
}

Expected expected_fold(int round) {
    Expected out;
    for (int child = 0; child < CHILDREN; ++child) {
        const std::string sum =
            i64_bytes(fold_unit(child, round) * FOLD_BATCHES_PER_SCOPE);
        for (int file = 0; file < FOLD_SCOPES; ++file) {
            const std::string scope = own_scope_path(child, file);
            for (int k = 0; k < FOLD_KEYS; ++k) {
                out[row_id(scope, fold_key(child, file, k))] = sum;
            }
        }
    }
    const std::string shared = i64_bytes(shared_unit(round));
    for (int batch = 0; batch < SHARED_BATCHES; ++batch) {
        out[row_id(SHARED_SCOPE, shared_key(batch))] = shared;
    }
    return out;
}

Expected expected_durable(int round) {
    Expected out;
    for (int child = 0; child < CHILDREN; ++child) {
        const std::string scope = durable_scope_path(child);
        for (int n = 0; n < DURABLE_BATCHES; ++n) {
            out[row_id(scope, durable_key(child, n))] =
                durable_value(child, n, round);
        }
    }
    return out;
}

std::size_t count_data_files(const std::string& root) {
    std::size_t count = 0;
    std::error_code error;
    std::filesystem::directory_iterator iterator(root, error);
    if (error) return 0;
    for (const auto& entry : iterator) {
        const std::string name = entry.path().filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dfs") == 0) {
            ++count;
        }
    }
    return count;
}

std::string skipped_report(const Snapshot& snapshot) {
    std::string out;
    for (const SkippedFile& file : snapshot.skipped()) {
        out += "skipped " + to_std(file.name) + ": " +
               std::string(file.error.message()) + "\n";
    }
    return out;
}

/// The shared scope is written by every child under the same generation and
/// the same keys. One copy is current, so every key folds exactly once.
void check_shared_scope(const Snapshot& snapshot, const NamespaceSpec& sum,
                        int round) {
    ScanCursor cursor = snapshot.scan(sum, file_scope(SHARED_SCOPE), {});
    std::map<std::string, std::string> rows;
    std::string duplicate;
    while (!cursor.at_end()) {
        auto value = cursor.value();
        REQUIRE(value.has_value());
        const std::string key = to_std(cursor.key());
        if (!rows.emplace(key, to_std(*value)).second) duplicate = key;
        cursor.next();
    }
    const std::string expected = i64_bytes(shared_unit(round));
    CHECK_MESSAGE(duplicate.empty(),
                  "the shared scope returned key " << duplicate << " twice");
    CHECK(rows.size() == static_cast<std::size_t>(SHARED_BATCHES));
    for (int batch = 0; batch < SHARED_BATCHES; ++batch) {
        const auto at = rows.find(shared_key(batch));
        REQUIRE(at != rows.end());
        CHECK_MESSAGE(at->second == expected, "shared key "
                                                  << shared_key(batch)
                                                  << " is not one copy's sum");
    }
}

void check_states(const Snapshot& snapshot, const NamespaceSpec& raw,
                  const NamespaceSpec& sum, const NamespaceSpec& durable) {
    for (int child = 0; child < CHILDREN; ++child) {
        for (int file = 0; file < RAW_SCOPES; ++file) {
            const ScopeKey scope = file_scope(own_scope_path(child, file));
            CHECK(snapshot.state(raw, scope) == ScopeState::Ready);
            CHECK(snapshot.state(sum, scope) == ScopeState::Ready);
        }
        const ScopeKey own = file_scope(durable_scope_path(child));
        CHECK(snapshot.state(durable, own) == ScopeState::Ready);
    }
    CHECK(snapshot.state(sum, file_scope(SHARED_SCOPE)) == ScopeState::Ready);
}

void check_root(const std::string& root, int round) {
    auto opened = Snapshot::open(root);
    REQUIRE(opened.has_value());
    const Snapshot& snapshot = *opened;

    CHECK_MESSAGE(snapshot.skipped().size() == 0, skipped_report(snapshot));
    CHECK(count_data_files(root) == static_cast<std::size_t>(CHILDREN * round));

    const NamespaceSpec raw = put_spec("proc.raw");
    const NamespaceSpec sum = fold_spec("proc.sum");
    const NamespaceSpec durable = durable_spec("proc.durable");

    const ScanDump raw_dump = dump_namespace(snapshot, raw);
    const Expected raw_expected = expected_raw(round);
    CHECK_MESSAGE(raw_dump.exactly_once(raw_expected),
                  "proc.raw round " << round << ":\n"
                                    << raw_dump.report(raw_expected));

    const ScanDump fold_dump = dump_namespace(snapshot, sum);
    const Expected fold_expected = expected_fold(round);
    CHECK_MESSAGE(fold_dump.exactly_once(fold_expected),
                  "proc.sum round " << round << ":\n"
                                    << fold_dump.report(fold_expected));

    const ScanDump durable_dump = dump_namespace(snapshot, durable);
    const Expected durable_expected = expected_durable(round);
    CHECK_MESSAGE(durable_dump.exactly_once(durable_expected),
                  "proc.durable round "
                      << round << ":\n"
                      << durable_dump.report(durable_expected));

    check_shared_scope(snapshot, sum, round);
    check_states(snapshot, raw, sum, durable);
}

struct Namespaces {
    NamespaceId raw;
    NamespaceId sum;
    NamespaceId durable;
};

/// What the child reports for a failing workload. The child never returns into
/// doctest: it writes one line and exits.
struct ChildFailure {
    std::string message;
};

[[noreturn]] void child_fail(const std::string& message) {
    const std::string line = "store_processes child: " + message + "\n";
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
    ::_exit(1);
}

/// The commit retry the writer documents: a Busy commit left the batch whole,
/// so the same batch commits again after the wait for space.
coro::CoroTask<dstd::Expected<void, dstd::Error>> commit_retrying(
    Writer& writer, Batch batch, int attempts) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        auto committed = writer.commit(std::move(batch));
        if (committed.has_value() ||
            !committed.error().is_code(WriterErrc::Busy)) {
            co_return committed;
        }
        co_await writer.space();
    }
    co_return dstd::Unexpected<dstd::Error>(
        dstd::Error(WriterErrc::Busy, "the writer made no progress"));
}

coro::CoroTask<int> run_workload(Writer& writer, const Namespaces& ids,
                                 int child, int round, ChildFailure& failure) {
    for (int batch = 0; batch < RAW_BATCHES; ++batch) {
        const int file = batch % RAW_SCOPES;
        const int n = batch / RAW_SCOPES;
        const std::string key = raw_key(child, file, n);
        const std::string value = raw_value(child, file, n, round);
        Batch entries =
            writer.batch(ids.raw, file_scope(own_scope_path(child, file)), 0);
        entries.put(view_of(key), view_of(value));
        auto committed = co_await commit_retrying(writer, std::move(entries),
                                                  RETRY_ATTEMPTS);
        if (!committed.has_value()) {
            failure.message =
                "raw " + key + ": " + std::string(committed.error().message());
            co_return 1;
        }
    }

    for (int batch = 0; batch < FOLD_SCOPES * FOLD_BATCHES_PER_SCOPE; ++batch) {
        const int file = batch % FOLD_SCOPES;
        const std::string value = i64_bytes(fold_unit(child, round));
        Batch entries =
            writer.batch(ids.sum, file_scope(own_scope_path(child, file)), 0);
        for (int k = 0; k < FOLD_KEYS; ++k) {
            const std::string key = fold_key(child, file, k);
            entries.fold(view_of(key), view_of(value));
        }
        auto committed = co_await commit_retrying(writer, std::move(entries),
                                                  RETRY_ATTEMPTS);
        if (!committed.has_value()) {
            failure.message = "sum scope " + std::to_string(file) + ": " +
                              std::string(committed.error().message());
            co_return 1;
        }
    }

    for (int batch = 0; batch < SHARED_BATCHES; ++batch) {
        const std::string key = shared_key(batch);
        const std::string value = i64_bytes(shared_unit(round));
        Batch entries = writer.batch(ids.sum, file_scope(SHARED_SCOPE), 0);
        entries.fold(view_of(key), view_of(value));
        auto committed = co_await commit_retrying(writer, std::move(entries),
                                                  RETRY_ATTEMPTS);
        if (!committed.has_value()) {
            failure.message = "shared " + key + ": " +
                              std::string(committed.error().message());
            co_return 1;
        }
    }

    for (int batch = 0; batch < DURABLE_BATCHES; ++batch) {
        const std::string key = durable_key(child, batch);
        const std::string value = durable_value(child, batch, round);
        Batch entries =
            writer.batch(ids.durable, file_scope(durable_scope_path(child)), 0);
        entries.put(view_of(key), view_of(value));
        auto committed = co_await commit_retrying(writer, std::move(entries),
                                                  RETRY_ATTEMPTS);
        if (!committed.has_value()) {
            failure.message = "durable " + key + ": " +
                              std::string(committed.error().message());
            co_return 1;
        }
        if (batch == DURABLE_BATCHES / 2 - 1) {
            auto flushed = co_await writer.flush();
            if (!flushed.has_value()) {
                failure.message =
                    "durable flush: " + std::string(flushed.error().message());
                co_return 1;
            }
        }
    }

    co_return 0;
}

void run_child(const std::string& root, int child, int round,
               int barrier_read) {
    // The runtime is built before the barrier so the create that follows the
    // barrier is the first store call this process makes.
    Runtime runtime(RUNTIME_THREADS);

    char release = 0;
    ssize_t got = 0;
    do {
        got = ::read(barrier_read, &release, 1);
    } while (got < 0 && errno == EINTR);
    if (got < 0) child_fail("barrier read failed");
    (void)::close(barrier_read);

    WriterOptions options;
    options.page_size = PAGE_BYTES;
    options.page_quota_bytes = PAGE_QUOTA_BYTES;
    options.commit_bytes = std::uint64_t{1} << 20;
    options.commit_interval = std::chrono::milliseconds{20};
    options.host = dstring("proc");
    options.label = dstring("round" + std::to_string(round));

    auto created = Writer::create(root, *runtime.executor(), options);
    if (!created.has_value()) {
        child_fail("create: " + std::string(created.error().message()));
    }
    std::unique_ptr<Writer> writer = std::move(*created);

    auto raw = writer->add_namespace(put_spec("proc.raw"));
    if (!raw.has_value()) child_fail("add proc.raw");
    auto sum = writer->add_namespace(fold_spec("proc.sum"));
    if (!sum.has_value()) child_fail("add proc.sum");
    auto durable = writer->add_namespace(durable_spec("proc.durable"));
    if (!durable.has_value()) child_fail("add proc.durable");
    const Namespaces ids{*raw, *sum, *durable};

    ChildFailure failure;
    TypedTaskHandle<int> workload =
        runtime.submit(run_workload(*writer, ids, child, round, failure));
    if (workload.future.wait_for(CHILD_LIMIT) != std::future_status::ready) {
        child_fail("workload timed out");
    }
    if (workload.get() != 0) {
        child_fail(failure.message.empty() ? "workload failed"
                                           : failure.message);
    }

    TypedTaskHandle<dstd::Expected<void, dstd::Error>> closing =
        runtime.submit(writer->close());
    if (closing.future.wait_for(CHILD_LIMIT) != std::future_status::ready) {
        child_fail("close timed out");
    }
    auto closed = closing.get();
    if (!closed.has_value()) {
        child_fail("close: " + std::string(closed.error().message()));
    }
    ::_exit(0);
}

bool child_succeeded(int status) {
    return WIFEXITED(status) != 0 && WEXITSTATUS(status) == 0;
}

std::string exit_report(pid_t pid, int status) {
    if (WIFEXITED(status)) {
        return "child pid " + std::to_string(pid) + " exited with code " +
               std::to_string(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return "child pid " + std::to_string(pid) + " was killed by signal " +
               std::to_string(WTERMSIG(status));
    }
    return "child pid " + std::to_string(pid) + " ended with status " +
           std::to_string(status);
}

void reap_children(const std::vector<pid_t>& pids) {
    struct ChildExit {
        pid_t pid;
        bool done;
        int status;
    };
    std::vector<ChildExit> exits;
    exits.reserve(pids.size());
    for (const pid_t pid : pids) exits.push_back({pid, false, 0});

    const auto start = std::chrono::steady_clock::now();
    std::size_t remaining = exits.size();
    while (remaining > 0) {
        for (ChildExit& child : exits) {
            if (child.done) continue;
            int status = 0;
            const pid_t reaped = ::waitpid(child.pid, &status, WNOHANG);
            if (reaped == child.pid) {
                child.done = true;
                child.status = status;
                --remaining;
            }
        }
        if (remaining == 0) break;
        if (std::chrono::steady_clock::now() - start > REAP_LIMIT) {
            for (ChildExit& child : exits) {
                if (!child.done) ::kill(child.pid, SIGKILL);
            }
            for (ChildExit& child : exits) {
                if (!child.done) {
                    ::waitpid(child.pid, &child.status, 0);
                    child.done = true;
                }
            }
            FAIL("the children did not finish within " << REAP_LIMIT.count()
                                                       << "s");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    for (ChildExit& child : exits) {
        const bool succeeded = child_succeeded(child.status);
        CHECK_MESSAGE(succeeded, exit_report(child.pid, child.status));
    }
}

/// Forks the children, releases them together, and fails on any nonzero exit.
void run_round(const std::string& root, int round) {
    int barrier[2];
    if (::pipe(barrier) != 0) {
        FAIL("pipe failed: " << std::strerror(errno));
    }

    std::vector<pid_t> pids;
    bool forked = true;
    for (int child = 0; child < CHILDREN && forked; ++child) {
        const pid_t pid = ::fork();
        if (pid < 0) {
            forked = false;
            break;
        }
        if (pid == 0) {
            (void)::close(barrier[1]);
            run_child(root, child, round, barrier[0]);
        }
        pids.push_back(pid);
    }

    (void)::close(barrier[0]);
    (void)::close(barrier[1]);

    if (!forked) {
        for (const pid_t pid : pids) ::kill(pid, SIGKILL);
        for (const pid_t pid : pids) ::waitpid(pid, nullptr, 0);
        FAIL("fork failed: " << std::strerror(errno));
    }

    reap_children(pids);
}

/// Copies every regular file of `from` into `to`.
void copy_files(const std::string& from, const std::string& to) {
    for (const auto& entry : std::filesystem::directory_iterator(from)) {
        if (!entry.is_regular_file()) continue;
        std::filesystem::copy_file(
            entry.path(), std::filesystem::path(to) / entry.path().filename(),
            std::filesystem::copy_options::overwrite_existing);
    }
}

}  // namespace

TEST_CASE("processes: eight writers on one root read back once") {
    dftu_utils_test::ScopedTestDir dir("store_processes_root");
    dftu_utils_test::ScopedTestDir first("store_processes_round_one");

    // Both rounds fork before the parent starts any thread: a snapshot open
    // starts the default runtime's workers, and ThreadSanitizer refuses a
    // fork after that. Data files never change once closed, so a copy taken
    // after round one is the root as round one left it.
    run_round(dir.str(), 1);
    copy_files(dir.str(), first.str());
    run_round(dir.str(), 2);

    check_root(first.str(), 1);
    check_root(dir.str(), 2);
}

TEST_CASE("processes: the exactly-once check rejects a duplicate row") {
    const std::string id = row_id("/p0/f0.pfw", "k");
    ScanDump dump;
    dump.add(id, "v");
    const Expected expected{{id, "v"}};
    CHECK(dump.exactly_once(expected));

    dump.add(id, "v");
    CHECK_FALSE(dump.exactly_once(expected));
}
