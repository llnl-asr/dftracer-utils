// Benchmark: StringIntern lookups and inserts on realistic short strings.
//
// Strings are file paths and function names, a few thousand distinct. The
// clock covers only the table calls. The result of a rep is wall time divided
// by operations.
//
// Cases (one op is named per case):
//   get_or_insert_known  get_or_insert of a string already in the table.
//   get_or_insert_new    get_or_insert of a string the table has not seen, on
//                        a table that starts empty each rep.
//   resolve              resolve of an id the table holds.
//   get_or_insert_mt8    get_or_insert from 8 threads on one table that starts
//                        empty each rep, so the first pass inserts and the
//                        rest look up. One op is one call, over all threads.
//
//   string_intern_bench                       (all cases, 5 reps, table)
//   string_intern_bench --reps R              (R reps per case)
//   string_intern_bench --case NAME           (one case)
//   string_intern_bench --format csv|table    (output shape)
//   string_intern_bench --record FILE         (write the per-case median and
//                                              spread over the reps as JSON)
//   string_intern_bench --check FILE          (fail when a case is slower than
//                                              the recorded median by more
//                                              than max(3%, its recorded
//                                              spread))
//   string_intern_bench --smoke               (about 2% of the sizes, 1 rep,
//                                              and every result must be
//                                              positive and correct)

#include <dftracer/utils/dstd/string_intern.h>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using dftracer::utils::dstd::StringIntern;

using Clock = std::chrono::steady_clock;

constexpr std::size_t THREADS = 8;
constexpr std::size_t KNOWN_STRINGS = 4096;
constexpr std::size_t NEW_STRINGS = 200000;
constexpr int DEFAULT_REPS = 5;
constexpr double SMOKE_FRACTION = 0.02;

struct Result {
    double total_ns = 0;
    bool correct = true;
};

double elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - start).count();
}

std::vector<std::string> make_strings(std::size_t count) {
    static const char* const DIRS[] = {
        "/p/lustre1/user/run", "/usr/lib/x86_64-linux-gnu", "/scratch/data/ds",
        "/home/worker/.cache", "/mnt/pfs/checkpoints"};
    static const char* const FUNCS[] = {"read",     "write",     "pread64",
                                        "openat",   "fsync",     "H5Dread",
                                        "MPI_Send", "torch.save"};
    std::vector<std::string> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::string s;
        if (i % 4 == 3) {
            s = std::string(FUNCS[i % 8]) + "::" + std::to_string(i);
        } else {
            s = std::string(DIRS[i % 5]) + "/part" + std::to_string(i / 5) +
                "/shard_" + std::to_string(i) + ".dat";
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::atomic<std::uint64_t> sink{0};

Result get_or_insert_known(std::size_t n) {
    auto strings = make_strings(KNOWN_STRINGS);
    StringIntern table;
    for (const auto& s : strings) table.get_or_insert(s);
    std::uint64_t acc = 0;
    auto start = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        acc += table.get_or_insert(strings[i & (KNOWN_STRINGS - 1)]);
    }
    Result r;
    r.total_ns = elapsed_ns(start, Clock::now());
    r.correct = table.size() == KNOWN_STRINGS;
    sink.fetch_add(acc, std::memory_order_relaxed);
    return r;
}

Result get_or_insert_new(std::size_t n) {
    auto strings = make_strings(n);
    StringIntern table;
    std::uint64_t acc = 0;
    auto start = Clock::now();
    for (std::size_t i = 0; i < n; ++i) acc += table.get_or_insert(strings[i]);
    Result r;
    r.total_ns = elapsed_ns(start, Clock::now());
    r.correct = table.size() == n;
    sink.fetch_add(acc, std::memory_order_relaxed);
    return r;
}

Result resolve(std::size_t n) {
    auto strings = make_strings(KNOWN_STRINGS);
    StringIntern table;
    std::vector<std::uint32_t> ids;
    for (const auto& s : strings) ids.push_back(table.get_or_insert(s));
    std::size_t bytes = 0;
    auto start = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        bytes += table.resolve(ids[i & (KNOWN_STRINGS - 1)]).size();
    }
    Result r;
    r.total_ns = elapsed_ns(start, Clock::now());
    std::size_t expected = 0;
    for (std::size_t i = 0; i < n; ++i) {
        expected += strings[i & (KNOWN_STRINGS - 1)].size();
    }
    r.correct = bytes == expected;
    return r;
}

Result get_or_insert_mt8(std::size_t n) {
    auto strings = make_strings(KNOWN_STRINGS);
    StringIntern table;
    const std::size_t per_thread = n / THREADS;
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> total{0};
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
            }
            std::uint64_t acc = 0;
            for (std::size_t i = 0; i < per_thread; ++i) {
                acc += table.get_or_insert(
                    strings[(i + t * 509) & (KNOWN_STRINGS - 1)]);
            }
            total.fetch_add(acc, std::memory_order_relaxed);
        });
    }
    while (ready.load() < THREADS) {
    }
    auto start = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    Result r;
    r.total_ns = elapsed_ns(start, Clock::now());
    r.correct = table.size() == KNOWN_STRINGS;
    sink.fetch_add(total.load(), std::memory_order_relaxed);
    return r;
}

struct Case {
    const char* name;
    std::size_t full_ops;
    std::function<Result(std::size_t)> run;
    std::function<std::size_t(std::size_t)> ops;
};

std::size_t exact(std::size_t n) { return n; }
std::size_t per_thread(std::size_t n) { return (n / THREADS) * THREADS; }

std::vector<Case> make_cases() {
    return {
        {"get_or_insert_known", 20000000, get_or_insert_known, exact},
        {"get_or_insert_new", NEW_STRINGS, get_or_insert_new, exact},
        {"resolve", 50000000, resolve, exact},
        {"get_or_insert_mt8", 16000000, get_or_insert_mt8, per_thread},
    };
}

struct Stats {
    double median;
    double min;
    double max;
    double spread_pct;
};

Stats summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    Stats s{};
    s.median = values[values.size() / 2];
    s.min = values.front();
    s.max = values.back();
    s.spread_pct = s.median > 0 ? (s.max - s.min) / s.median * 100.0 : 0.0;
    return s;
}

constexpr double CHECK_MIN_PCT = 3.0;

struct Recorded {
    std::string name;
    double median = 0.0;
    double spread_pct = 0.0;
};

bool write_record(const char* path, const std::vector<Recorded>& rows) {
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) return false;
    std::fprintf(f, "{\n");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        std::fprintf(f,
                     "  \"%s\": {\"median\": %.3f, \"spread_pct\": %.3f}%s\n",
                     rows[i].name.c_str(), rows[i].median, rows[i].spread_pct,
                     i + 1 < rows.size() ? "," : "");
    }
    std::fprintf(f, "}\n");
    return std::fclose(f) == 0;
}

bool read_record(const char* path, std::vector<Recorded>& rows) {
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return false;
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char name[128];
        Recorded r;
        if (std::sscanf(line,
                        " \"%127[^\"]\": {\"median\": %lf, \"spread_pct\": %lf",
                        name, &r.median, &r.spread_pct) == 3) {
            r.name = name;
            rows.push_back(r);
        }
    }
    std::fclose(f);
    return !rows.empty();
}

int check_record(const char* path, const std::vector<Recorded>& now) {
    std::vector<Recorded> base;
    if (!read_record(path, base)) {
        std::fprintf(stderr, "cannot read record: %s\n", path);
        return 1;
    }
    int slower = 0;
    for (const Recorded& n : now) {
        for (const Recorded& b : base) {
            if (b.name != n.name) continue;
            double allowed = std::max(CHECK_MIN_PCT, b.spread_pct);
            double change = (n.median / b.median - 1.0) * 100.0;
            bool bad = change > allowed;
            std::fprintf(stderr,
                         "check %-24s %8.1f vs %8.1f ns (%+.1f%%, allowed "
                         "%.1f%%) %s\n",
                         n.name.c_str(), n.median, b.median, change, allowed,
                         bad ? "SLOWER" : "ok");
            if (bad) ++slower;
        }
    }
    return slower == 0 ? 0 : 1;
}

int run(const std::vector<Case>& cases, int reps, bool smoke, bool csv,
        const char* record, const char* check) {
    std::vector<Recorded> rows;
    if (csv) {
        std::printf("name,median_ns_per_op,min,max,spread_pct\n");
    } else {
        std::printf("string_intern_bench threads=%zu reps=%d%s\n", THREADS,
                    reps, smoke ? " smoke" : "");
        std::printf(
            "| case | median ns/op | min | max | spread %% |\n"
            "|---|---|---|---|---|\n");
    }
    int failures = 0;
    for (const Case& c : cases) {
        std::size_t requested =
            smoke ? std::max<std::size_t>(
                        static_cast<std::size_t>(
                            static_cast<double>(c.full_ops) * SMOKE_FRACTION),
                        64)
                  : c.full_ops;
        std::size_t ops = c.ops(requested);
        std::vector<double> per_op;
        bool ok = true;
        for (int rep = 0; rep < reps; ++rep) {
            Result r = c.run(requested);
            ok = ok && r.correct && r.total_ns > 0.0;
            per_op.push_back(r.total_ns / static_cast<double>(ops));
        }
        Stats s = summarize(per_op);
        rows.push_back({c.name, s.median, s.spread_pct});
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "case failed: %s\n", c.name);
        }
        if (csv) {
            std::printf("%s,%.3f,%.3f,%.3f,%.1f\n", c.name, s.median, s.min,
                        s.max, s.spread_pct);
        } else {
            std::printf("| %s | %.1f | %.1f | %.1f | %.1f |\n", c.name,
                        s.median, s.min, s.max, s.spread_pct);
        }
        std::fflush(stdout);
    }
    int status = failures == 0 ? 0 : 1;
    if (record != nullptr && !write_record(record, rows)) {
        std::fprintf(stderr, "cannot write record: %s\n", record);
        status = 1;
    }
    if (check != nullptr && check_record(check, rows) != 0) status = 1;
    return status;
}

int usage() {
    std::fprintf(
        stderr,
        "usage: string_intern_bench [--smoke] [--reps R] [--case NAME] "
        "[--format csv|table] [--record FILE] [--check FILE]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef __APPLE__
    // Executor threads inherit this class, so they stay on performance cores
    // and the timings do not depend on where the scheduler put a thread.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    int reps = DEFAULT_REPS;
    bool smoke = false;
    bool csv = false;
    std::string only;
    const char* record = nullptr;
    const char* check = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        bool has_value = i + 1 < argc;
        if (arg == "--smoke") {
            smoke = true;
        } else if (arg == "--reps" && has_value) {
            reps = std::atoi(argv[++i]);
        } else if (arg == "--case" && has_value) {
            only = argv[++i];
        } else if (arg == "--record" && has_value) {
            record = argv[++i];
        } else if (arg == "--check" && has_value) {
            check = argv[++i];
        } else if (arg == "--format" && has_value) {
            std::string_view fmt = argv[++i];
            if (fmt != "csv" && fmt != "table") return usage();
            csv = fmt == "csv";
        } else {
            return usage();
        }
    }
    if (smoke) reps = 1;
    if (reps <= 0) return usage();

    std::vector<Case> cases = make_cases();
    if (!only.empty()) {
        std::vector<Case> picked;
        for (const Case& c : cases) {
            if (only == c.name) picked.push_back(c);
        }
        if (picked.empty()) {
            std::fprintf(stderr, "unknown case: %s\n", only.c_str());
            return 2;
        }
        cases = std::move(picked);
    }
    return run(cases, reps, smoke, csv, record, check);
}
