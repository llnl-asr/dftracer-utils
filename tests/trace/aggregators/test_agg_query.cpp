// The aggregation query over real indexes: every group-by, window and filter
// gives the rows a per-event aggregator gives for the same events, and
// the chunks that cannot be answered from the stored partials are re-read.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_core.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/comparator/comparison_aggregation.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <mmix_lcg.h>
#include <testing_utilities.h>
#include <trace/aggregators/aggregation_oracle.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace dftracer::utils;
namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace layout = dftracer::utils::index::store::layout;
using dataframe::FieldStat;
using dataframe::FieldStatDomain;
using trace::AggQuery;
using trace::AggQueryResult;
using trace::AggQuerySource;

namespace {

constexpr int FILES = 4;
constexpr int EVENTS = 600;
constexpr std::size_t CHECKPOINT_SIZE = 6000;
constexpr std::uint64_t ONE_SECOND_US = 1'000'000;
constexpr std::uint64_t FIVE_SECONDS_US = 5'000'000;
constexpr std::uint64_t BASE_TS = 1'000'000'000;
constexpr double MOMENT_TOL = 1e-9;
// A sketch answers with a value near the rank, so it lies between the values
// next to the rank, widened by twice its relative accuracy.
constexpr double SKETCH_ACCURACY = 0.01;

struct Ev {
    bool counter = false;
    std::string cat;
    std::string name;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;
    bool has_dur = true;
    std::string fhash;
    std::vector<std::pair<std::string, std::uint64_t>> args;
};

class Acc {
   public:
    explicit Acc(const Ev& e) : e_(e) {}

    bool has_phase() const { return true; }
    bool is_metadata() const { return false; }
    bool is_counter() const { return e_.counter; }
    bool is_profile() const { return e_.counter && e_.cat != "sys"; }
    bool is_system() const { return e_.counter && e_.cat == "sys"; }
    std::string_view name() const { return e_.name; }
    std::string_view cat() const { return e_.cat; }
    std::uint64_t pid() const { return e_.pid; }
    std::uint64_t tid() const { return e_.tid; }
    std::uint64_t ts() const { return e_.ts; }
    std::uint64_t dur() const { return e_.dur; }
    bool has_dur() const { return e_.has_dur; }

    bool arg_is_number(std::string_view k) const {
        return k != "fhash" && find(k) != nullptr;
    }
    std::string_view arg_string(std::string_view k) const {
        return k == "fhash" ? std::string_view(e_.fhash) : std::string_view{};
    }
    void metric_args(agg::MetricArgs& m) const {
        for (const auto& [name, v] : e_.args) {
            for (std::size_t i = 0; i < agg::METRIC_ARG_COUNT; ++i)
                if (name == agg::METRIC_ARG_NAMES[i]) {
                    m.add(static_cast<agg::MetricArg>(i), v);
                    break;
                }
        }
    }

   private:
    const std::uint64_t* find(std::string_view k) const {
        for (const auto& [name, v] : e_.args)
            if (name == k) return &v;
        return nullptr;
    }
    const Ev& e_;
};

std::uint64_t next(std::uint64_t& s) {
    s = mmix_lcg_step(s);
    return s >> 17;
}

agg::AggregationConfig make_config(std::uint64_t interval_us) {
    agg::AggregationConfig cfg;
    cfg.time_interval_us = interval_us;
    cfg.group_by_file = false;
    cfg.compute_percentiles = true;
    return cfg;
}

// POSIX read/write and STDIO fread/fwrite carry ret and offset. Open carries a
// ret that is a descriptor and must not count as a size. Compute carries
// numeric args only. "rare" has fewer than 64 events, so its quantiles stay
// exact. Every 17th POSIX transfer is a pre-aggregated counter.
std::vector<Ev> make_events(std::uint64_t seed) {
    static const char* const FILE_HASHES[] = {
        "1a2b3c4d", "deadbeef", "0badf00d", "cafe1234",
        "feed5678", "abcdef01", "13572468"};
    std::vector<Ev> out;
    std::uint64_t s = seed;
    for (int i = 0; i < EVENTS; ++i) {
        Ev e;
        e.pid = 100 + next(s) % 4;
        e.tid = e.pid * 10 + next(s) % 3;
        e.ts =
            BASE_TS + static_cast<std::uint64_t>(i) * 23'000 + next(s) % 3000;
        e.dur = 1 + next(s) % 10000;
        const std::uint64_t pick = next(s) % 10;
        if (i % 75 == 0) {
            e.cat = "app";
            e.name = "rare";
        } else if (pick < 3) {
            e.cat = "POSIX";
            e.name = "read";
        } else if (pick < 5) {
            e.cat = "POSIX";
            e.name = "write";
        } else if (pick < 6) {
            e.cat = "POSIX";
            e.name = "open";
        } else if (pick < 7) {
            e.cat = "STDIO";
            e.name = "fread";
        } else if (pick < 8) {
            e.cat = "STDIO";
            e.name = "fwrite";
        } else {
            e.cat = "app";
            e.name = "compute";
        }
        const bool transfer = e.name == "read" || e.name == "write" ||
                              e.name == "fread" || e.name == "fwrite";
        if (e.cat != "app") e.fhash = FILE_HASHES[next(s) % 7];
        if (transfer && e.cat == "POSIX" && i % 17 == 0) {
            e.counter = true;
            e.dur = 0;
            const std::uint64_t cnt = 1 + next(s) % 20;
            const std::uint64_t d = 50 + next(s) % 500;
            e.args = {{"dftu_cnt", cnt},         {"dur_sum", d * cnt},
                      {"dur_min", d / 2},        {"dur_max", d * 2},
                      {"ret_sum", 4096 * cnt},   {"ret_min", 512},
                      {"ret_max", 65536},        {"offset_sum", 8192 * cnt},
                      {"offset_min", 0},         {"offset_max", 1 << 20},
                      {"flops_sum", 1000 * cnt}, {"flops_min", 10},
                      {"flops_max", 5000}};
            out.push_back(std::move(e));
            continue;
        }
        if (transfer) {
            e.args.emplace_back("ret", 1 + next(s) % (1 << 20));
            e.args.emplace_back("offset", next(s) % (1 << 20));
            if (next(s) % 3 != 0) e.args.emplace_back("level", next(s) % 4);
        } else if (e.name == "open") {
            e.args.emplace_back("ret", 3 + next(s) % 100);
        } else {
            e.args.emplace_back("iter", static_cast<std::uint64_t>(i));
        }
        if (next(s) % 2 == 0) e.args.emplace_back("flops", next(s) % 100000);
        out.push_back(std::move(e));
    }
    return out;
}

// With `cut_mid_line` the gzip members are cut every CHECKPOINT_SIZE bytes of
// text wherever that falls, so most members end inside a line.
std::string write_trace(const std::string& dir, int n,
                        const std::vector<Ev>& events, bool cut_mid_line) {
    std::ostringstream out;
    out << "[\n";
    int id = 0;
    for (const Ev& e : events) {
        out << R"({"id":)" << id++ << R"(,"name":")" << e.name << R"(","cat":")"
            << e.cat << R"(","pid":)" << e.pid << R"(,"tid":)" << e.tid
            << R"(,"ph":")" << (e.counter ? "C" : "X") << R"(","ts":)" << e.ts;
        if (!e.counter) out << R"(,"dur":)" << e.dur;
        out << R"(,"args":{)";
        bool first = true;
        for (const auto& [k, v] : e.args) {
            out << (first ? "" : ",") << '"' << k << "\":" << v;
            first = false;
        }
        if (!e.fhash.empty())
            out << (first ? "" : ",") << R"("fhash":")" << e.fhash << '"';
        out << "}}\n";
    }
    out << "]\n";
    const std::string pfw = dir + "/t" + std::to_string(n) + ".pfw";
    {
        std::ofstream file(pfw);
        file << out.str();
    }
    const std::string gz = pfw + ".gz";
    if (!cut_mid_line) {
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
            pfw, gz, CHECKPOINT_SIZE));
    } else {
        const std::string text = out.str();
        utilities::fileio::compress::GzipMemberCompressor comp;
        REQUIRE(comp.valid());
        std::ofstream file(gz, std::ios::binary);
        for (std::size_t at = 0; at < text.size(); at += CHECKPOINT_SIZE) {
            const std::size_t len = std::min(CHECKPOINT_SIZE, text.size() - at);
            auto member = comp.compress_member(text.data() + at, len);
            REQUIRE(member.has_value());
            file.write(reinterpret_cast<const char*>(member->data()),
                       static_cast<std::streamsize>(member->size()));
        }
    }
    common::fs::remove(pfw);
    return gz;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool is_transfer(const Ev& e) {
    const std::string c = lower(e.cat);
    return (c == "posix" || c == "stdio") &&
           (e.name == "read" || e.name == "write" || e.name == "fread" ||
            e.name == "fwrite");
}

struct Ask {
    bool by_pid = false;
    bool by_tid = false;
    bool by_file = false;
    std::uint64_t window = 0;
    bool quantiles = false;
    std::set<std::string> categories;
    std::set<std::string> names;
    std::set<std::uint64_t> pids;
    std::uint64_t ts_begin = 0;
    std::uint64_t ts_end = UINT64_MAX;
};

using Key = std::tuple<std::string, std::string, std::uint64_t, std::uint64_t,
                       std::uint64_t, std::uint64_t>;

bool kept(const Ev& e, const Ask& a) {
    if (!a.categories.empty() && !a.categories.count(lower(e.cat)))
        return false;
    if (!a.names.empty() && !a.names.count(e.name)) return false;
    if (!a.pids.empty() && !a.pids.count(e.pid)) return false;
    return e.ts >= a.ts_begin && e.ts < a.ts_end;
}

std::uint64_t file_of(const Ev& e) {
    return e.fhash.empty() ? 0 : agg::agg_hash_of_hex(e.fhash);
}

Key key_of(const Ev& e, const Ask& a) {
    return {lower(e.cat),
            e.name,
            a.by_pid ? e.pid : 0,
            a.by_tid ? e.tid : 0,
            a.by_file ? file_of(e) : 0,
            a.window ? e.ts / a.window * a.window : 0};
}

struct Truth {
    std::set<std::uint64_t> pids, tids, files, pid_tids, pid_files;
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> spans;
    std::vector<double> dur, size, off;
    agg::AggregationMetrics metrics{0.01};
};

// A per-event aggregator, one state per group key of the query.
std::map<Key, Truth> reference(const std::vector<Ev>& events, const Ask& ask,
                               const agg::AggregationConfig& cfg) {
    std::map<Key, Truth> out;
    for (const Ev& e : events) {
        if (!kept(e, ask)) continue;
        Truth& t = out[key_of(e, ask)];
        agg::oracle_event(Acc(e), t.metrics, cfg);
        t.pids.insert(e.pid);
        t.tids.insert(e.tid);
        t.pid_tids.insert(agg::pid_tid_id(e.pid, e.tid));
        if (!e.fhash.empty())
            t.pid_files.insert(agg::pid_file_id(e.pid, file_of(e)));
        if (!e.fhash.empty()) t.files.insert(file_of(e));
        const std::uint64_t end =
            e.ts + (e.counter ? cfg.time_interval_us : e.dur);
        auto [it, fresh] = t.spans.try_emplace(e.pid, e.ts, end);
        if (!fresh) {
            it->second.first = std::min(it->second.first, e.ts);
            it->second.second = std::max(it->second.second, end);
        }
        if (e.counter) continue;
        t.dur.push_back(static_cast<double>(e.dur));
        for (const auto& [k, v] : e.args) {
            if (k == "ret" && is_transfer(e))
                t.size.push_back(static_cast<double>(v));
            if (k == "offset") t.off.push_back(static_cast<double>(v));
        }
    }
    return out;
}

bool near(double a, double b, double scale, double tol) {
    return std::abs(a - b) <= tol * std::max(scale, 1e-300);
}

void check_stat(const FieldStat& a, const FieldStat& b, const char* what) {
    INFO("field: ", what);
    REQUIRE(a.n == b.n);
    if (a.n == 0) return;
    CHECK(a.sum == b.sum);
    CHECK(a.min == b.min);
    CHECK(a.max == b.max);
    const double s2 = std::max(std::abs(a.cm2), std::abs(b.cm2));
    CHECK(near(a.cm2, b.cm2, s2, MOMENT_TOL));
    CHECK(near(a.cm3, b.cm3, std::pow(s2, 1.5), MOMENT_TOL));
    CHECK(near(a.cm4, b.cm4, s2 * s2, MOMENT_TOL));
    CHECK(near(a.moment_mean(), b.moment_mean(),
               std::max(std::abs(a.moment_mean()), 1.0), MOMENT_TOL));
}

double exact_rule(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    if (q <= 0.0) return v.front();
    if (q >= 1.0) return v.back();
    const double pos = q * static_cast<double>(n - 1);
    const auto lo = static_cast<std::size_t>(pos);
    const double frac = pos - static_cast<double>(lo);
    if (lo + 1 >= n) return v[lo];
    return v[lo] * (1.0 - frac) + v[lo + 1] * frac;
}

void check_quantiles(const agg::QuantileStore& q,
                     const std::vector<double>& truth, const char* what) {
    INFO("quantiles of ", what);
    REQUIRE(q.count() == truth.size());
    if (truth.empty()) return;
    CHECK(q.is_exact() == (truth.size() <= agg::QuantileStore::EXACT_CAPACITY));
    std::vector<double> sorted = truth;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t last = sorted.size() - 1;
    for (double p : {0.1, 0.5, 0.9, 0.99}) {
        const double got = q.quantile(p);
        if (q.is_exact()) {
            CHECK(got == doctest::Approx(exact_rule(truth, p)).epsilon(1e-12));
        } else {
            const auto lo =
                static_cast<std::size_t>(p * static_cast<double>(last));
            const double slack = 2 * SKETCH_ACCURACY;
            CHECK(got >= sorted[lo == 0 ? 0 : lo - 1] * (1.0 - slack));
            CHECK(got <= sorted[std::min(lo + 2, last)] * (1.0 + slack));
        }
    }
}

Key row_key(const AggQueryResult& r, const trace::AggKey& k) {
    return {std::string(r.text(k.cat)),
            std::string(r.text(k.name)),
            k.pid,
            k.tid,
            k.file,
            k.window};
}

void check_group(const agg::AggPartial& p, const Truth& t, bool quantiles) {
    const agg::AggregationMetrics& ref = t.metrics;

    CHECK(p.count == ref.count);
    CHECK(p.ts == ref.ts);
    CHECK(p.te == ref.te);
    check_stat(p.duration, ref.duration.stat, "duration");
    check_stat(p.size, ref.size.stat, "size");
    check_stat(p.offset, ref.offset.stat, "offset");

    CHECK(p.pids.is_exact());
    CHECK(p.pids.count() == t.pids.size());
    CHECK(p.tids.count() == t.tids.size());
    CHECK(p.pid_tids.count() == t.pid_tids.size());
    CHECK(p.pid_files.count() == t.pid_files.size());
    CHECK(p.files.count() == t.files.size());
    REQUIRE(p.pid_spans.size() == t.spans.size());
    std::size_t at = 0;
    for (const auto& [pid, span] : t.spans) {
        CHECK(p.pid_spans[at].pid == pid);
        CHECK(p.pid_spans[at].first == span.first);
        CHECK(p.pid_spans[at].last == span.second);
        ++at;
    }

    if (quantiles) {
        check_quantiles(p.duration_q, t.dur, "duration");
        check_quantiles(p.size_q, t.size, "size");
        check_quantiles(p.offset_q, t.off, "offset");
    } else {
        CHECK(p.duration_q.count() == 0);
    }
}

struct Fixture {
    dftu_utils_test::TestEnvironment env{10};
    std::vector<Ev> events;  // all files
    std::vector<std::string> files;
    std::string index_dir;
    std::string index_path;
};

std::unique_ptr<Fixture> build(std::uint64_t interval_us,
                               const std::string& tag,
                               bool cut_mid_line = false,
                               bool reversed = false) {
    auto fx = std::make_unique<Fixture>();
    for (int i = 0; i < FILES; ++i) {
        auto events = make_events(1000 + static_cast<std::uint64_t>(i));
        if (reversed) std::reverse(events.begin(), events.end());
        fx->files.push_back(
            write_trace(fx->env.get_dir(), i, events, cut_mid_line));
        fx->events.insert(fx->events.end(), events.begin(), events.end());
    }
    fx->index_dir = fx->env.get_dir() + "/idx_" + tag;
    index::IndexerOptions o;
    o.index_dir = fx->index_dir;
    o.checkpoint_size = CHECKPOINT_SIZE;
    o.parallelism = 4;
    o.aggregation = make_config(interval_us);
    auto ix = index::Indexer::open(fx->files, o);
    ix.build();
    fx->index_path =
        trace::internal::determine_index_path(fx->files[0], fx->index_dir);
    return fx;
}

AggQuery make_query(const Fixture& fx, const Ask& ask,
                    std::uint64_t interval_us) {
    AggQuery q;
    AggQuerySource source;
    source.index_path = fx.index_path;
    for (const auto& f : fx.files) source.files.emplace_back(f);
    q.sources.push_back(std::move(source));
    q.config = make_config(interval_us);
    q.by_pid = ask.by_pid;
    q.by_tid = ask.by_tid;
    q.by_file = ask.by_file;
    q.window_us = ask.window;
    q.quantiles = ask.quantiles;
    for (const auto& c : ask.categories) q.categories.emplace_back(c);
    for (const auto& n : ask.names) q.names.emplace_back(n);
    for (auto p : ask.pids) q.pids.push_back(p);
    q.ts_begin = ask.ts_begin;
    q.ts_end = ask.ts_end;
    return q;
}

Runtime& runtime() {
    static Runtime rt;
    return rt;
}

AggQueryResult run(const AggQuery& q) {
    Runtime& rt = runtime();
    AggQueryResult out;
    std::string error;
    rt.run_blocking("agg-query", [&](CoroScope&) -> coro::CoroTask<void> {
        auto r = co_await trace::run_agg_query(q);
        if (r)
            out = std::move(*r);
        else
            error = r.error().format();
        co_return;
    });
    REQUIRE_MESSAGE(error.empty(), error);
    return out;
}

// Compares every row with the per-event aggregator. Returns the result so a
// case can assert how it was answered.
AggQueryResult check(const Fixture& fx, const Ask& ask,
                     std::uint64_t interval_us, const char* what,
                     std::size_t workers = 0) {
    INFO("case: ", what);
    AggQuery q = make_query(fx, ask, interval_us);
    q.workers = workers;
    AggQueryResult r = run(q);
    const auto truth = reference(fx.events, ask, make_config(interval_us));
    REQUIRE(r.rows.size() == truth.size());
    for (const auto& [k, p] : r.rows) {
        const Key key = row_key(r, k);
        const auto t = truth.find(key);
        REQUIRE_MESSAGE(t != truth.end(), std::get<0>(key), " ",
                        std::get<1>(key));
        check_group(p, t->second, ask.quantiles);
    }
    const auto& s = r.stats;
    MESSAGE(std::string(what), ": files ", s.files, ", without partials ",
            s.files_without_partials, ", partials read ", s.partials_read,
            ", merged ", s.partials_merged, ", pruned ", s.partials_pruned,
            ", re-read ", s.partials_reread, " in ", s.chunks_reread,
            " chunk reads, events read ", s.events_read, ", result map ",
            s.groups, " rows");
    return r;
}

std::size_t partial_count(const Fixture& fx) {
    auto snapshot = common::unwrap(index::plan::open_snapshot(fx.index_path));
    std::size_t n = 0;
    for (const auto& f : fx.files) {
        const auto file =
            snapshot.file(index::store::internal::get_logical_path(f));
        REQUIRE(file);
        common::unwrap(index::for_each_agg_partial(
            snapshot, *file,
            [&](std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                std::uint32_t, agg::PartialKind,
                std::span<const std::uint8_t>) {
                ++n;
                return true;
            }));
    }
    return n;
}

// A trace of counters, pre-aggregated records and events over three processes,
// two threads and five 5 s windows, some naming a file in fhash and some naming
// none, with an int64, a uint64 and a double arg on nearly every record.
std::string mixed_trace() {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < 600; ++i) {
        const std::uint64_t pid = 700 + static_cast<std::uint64_t>(i % 3);
        const std::uint64_t tid = pid * 10 + static_cast<std::uint64_t>(i % 2);
        const std::uint64_t ts =
            BASE_TS + static_cast<std::uint64_t>(i) * 41'000;
        out << R"({"id":)" << i << R"(,"pid":)" << pid << R"(,"tid":)" << tid
            << R"(,"ts":)" << ts;
        switch (i % 6) {
            case 0:
                out << R"(,"ph":"C","cat":"POSIX","name":"read","args":{)"
                    << R"("dftu_cnt":3,"dur_sum":840,"dur_min":100,)"
                    << R"("dur_max":900,"ret_sum":12288,"ret_min":4096,)"
                    << R"("ret_max":8192,"offset_sum":6144,)"
                    << R"("offset_min":2048,"offset_max":4096})";
                break;
            case 1:
                out << R"(,"ph":"C","cat":"sys","name":"cpu","args":{)"
                    << R"("dftu_cnt":2,"user_pct":)" << 10 + i
                    << R"(,"sys_pct":5,"delta":-)" << i % 5 << "}";
                break;
            case 2:
                out << R"(,"ph":"A","cat":"app","name":"step","dur":)" << 40 + i
                    << R"(,"args":{"dftu_cnt":2,"delta":-)" << i % 7
                    << R"(,"scale":0.5,"offset":)"
                    << (std::uint64_t{1} << 40) + static_cast<std::uint64_t>(i);
                if (i % 4 == 2) out << R"(,"fhash":"0123456789abcdef")";
                out << "}";
                break;
            case 3:
                out << R"(,"ph":"X","cat":"STDIO","name":"fread","dur":)"
                    << 20 + i << R"(,"args":{"ret":8192,"offset":)" << i
                    << R"(,"level":2,"fhash":"fedcba9876543210"})";
                break;
            case 4:
                out << R"(,"ph":"X","cat":"app","name":"compute","dur":)"
                    << 5 + i << R"(,"args":{"iter":)" << i << R"(,"delta":-)"
                    << i % 3 << R"(,"scale":2.25,)"
                    << R"("offset":)" << (std::uint64_t{1} << 50) + i << "}";
                break;
            default:
                out << R"(,"ph":"X","cat":"POSIX","name":"write","dur":)"
                    << 30 + i << R"(,"args":{"ret":4096,"offset":)" << 2 * i
                    << R"(,"fhash":"0123456789abcdef"})";
                break;
        }
        out << "}\n";
    }
    out << "]\n";
    return out.str();
}

// The same stat of two answers of one query. Both fold the same values of the
// same events, but the chunk tasks merge in their own order, and a sum of
// doubles that outgrows its mantissa may differ in its last bits.
void check_same_stat(const FieldStat& a, const FieldStat& b, const char* what) {
    INFO("field: ", what);
    REQUIRE(a.n == b.n);
    if (a.n == 0) return;
    CHECK(near(a.sum, b.sum, std::max(std::abs(a.sum), std::abs(b.sum)),
               MOMENT_TOL));
    CHECK(a.min == b.min);
    CHECK(a.max == b.max);
    const double s2 = std::max(std::abs(a.cm2), std::abs(b.cm2));
    CHECK(near(a.cm2, b.cm2, s2, MOMENT_TOL));
    CHECK(near(a.cm3, b.cm3, std::pow(s2, 1.5), MOMENT_TOL));
    CHECK(near(a.cm4, b.cm4, s2 * s2, MOMENT_TOL));
}

void check_same_quantiles(const agg::QuantileStore& a,
                          const agg::QuantileStore& b) {
    REQUIRE(a.count() == b.count());
    if (a.count() == 0) return;
    for (double p : {0.0, 0.1, 0.5, 0.9, 0.99, 1.0})
        CHECK(a.quantile(p) == b.quantile(p));
}

void check_same_partial(const agg::AggPartial& a, const agg::AggPartial& b) {
    CHECK(a.count == b.count);
    CHECK(a.ts == b.ts);
    CHECK(a.te == b.te);
    CHECK(a.ts_last_start == b.ts_last_start);
    check_same_stat(a.duration, b.duration, "duration");
    check_same_stat(a.size, b.size, "size");
    check_same_stat(a.offset, b.offset, "offset");
    CHECK(a.duration_values == b.duration_values);
    CHECK(a.size_values == b.size_values);
    CHECK(a.offset_values == b.offset_values);
    CHECK(a.pids.count() == b.pids.count());
    CHECK(a.tids.count() == b.tids.count());
    CHECK(a.files.count() == b.files.count());
    CHECK(a.pid_tids.count() == b.pid_tids.count());
    CHECK(a.pid_files.count() == b.pid_files.count());
    REQUIRE(a.pid_spans.size() == b.pid_spans.size());
    for (std::size_t i = 0; i < a.pid_spans.size(); ++i) {
        CHECK(a.pid_spans[i].pid == b.pid_spans[i].pid);
        CHECK(a.pid_spans[i].first == b.pid_spans[i].first);
        CHECK(a.pid_spans[i].last == b.pid_spans[i].last);
    }
    check_same_quantiles(a.duration_q, b.duration_q);
    check_same_quantiles(a.size_q, b.size_q);
    check_same_quantiles(a.offset_q, b.offset_q);
}

}  // namespace

TEST_SUITE("AggQuery") {
    TEST_CASE("category and name, no window, come from the partials alone") {
        auto fx = build(FIVE_SECONDS_US, "five");
        REQUIRE(partial_count(*fx) > FILES);
        Ask ask;
        const auto r = check(*fx, ask, FIVE_SECONDS_US, "cat+name");
        CHECK(r.stats.chunks_reread == 0);
        CHECK(r.stats.partials_merged == r.stats.partials_read);
        CHECK(r.stats.files_without_partials == 0);
        // The rows hold no quantile stores that were not asked for.
        for (const auto& [k, p] : r.rows) CHECK(p.duration_q.count() == 0);

        Ask with_q;
        with_q.quantiles = true;
        check(*fx, with_q, FIVE_SECONDS_US, "cat+name+quantiles");
    }

    TEST_CASE("the distinct pairs come from the partials without a read") {
        auto fx = build(FIVE_SECONDS_US, "five");
        Ask ask;
        ask.quantiles = true;
        const auto r = check(*fx, ask, FIVE_SECONDS_US, "pairs");
        // check() compared the pair counts of every row with the events'.
        CHECK(r.stats.chunks_reread == 0);
        CHECK(r.stats.partials_merged == r.stats.partials_read);
        std::size_t pairs = 0;
        for (const auto& [k, p] : r.rows) pairs += p.pid_tids.count();
        CHECK(pairs > 0);
    }

    TEST_CASE("the comparator reads its report from the partials alone") {
        // One process and thread per file, every event in one 5 s window: no
        // partial crosses a window or holds a second pid or tid.
        dftu_utils_test::TestEnvironment env{10};
        std::vector<std::string> files;
        for (int f = 0; f < 3; ++f) {
            std::vector<Ev> events;
            for (int i = 0; i < 200; ++i) {
                Ev e;
                e.cat = i % 2 == 0 ? "POSIX" : "STDIO";
                e.name = i % 4 < 2 ? "read" : "write";
                e.pid = 500 + static_cast<std::uint64_t>(f);
                e.tid = e.pid * 10;
                e.ts = BASE_TS + static_cast<std::uint64_t>(i) * 4000;
                e.dur = 10 + static_cast<std::uint64_t>(i % 7);
                e.fhash = "1a2b3c4d";
                e.args.emplace_back("ret", 4096);
                events.push_back(std::move(e));
            }
            files.push_back(write_trace(env.get_dir(), f, events, false));
        }
        const std::string index_dir = env.get_dir() + "/idx_cmp";
        index::IndexerOptions o;
        o.index_dir = index_dir;
        o.checkpoint_size = CHECKPOINT_SIZE;
        o.parallelism = 2;
        auto ix = index::Indexer::open(files, o);
        ix.build();

        dstd::Vector<trace::comparator::ComparisonFilter> filters;
        filters.emplace_back();
        dstd::StringIntern strings;
        trace::comparator::SideAggregate side;
        std::string error;
        runtime().run_blocking(
            "comparator-side", [&](CoroScope&) -> coro::CoroTask<void> {
                auto r = co_await trace::comparator::aggregate_side(
                    files, index_dir, filters, FIVE_SECONDS_US, 0, strings);
                if (r)
                    side = std::move(*r);
                else
                    error = r.error().format();
                co_return;
            });
        REQUIRE_MESSAGE(error.empty(), error);
        CHECK(side.metadata_chunks_reread == 0);
        CHECK(side.group_chunks_reread == 0);
        CHECK(side.meta.process_count == 3);
        CHECK(side.meta.thread_count == 3);
        CHECK(side.groups[0].size() == 4);
    }

    TEST_CASE("dropped parts leave the kept parts of the rows unchanged") {
        auto fx = build(FIVE_SECONDS_US, "five");
        const auto check_keep = [&](const Ask& ask, const char* what) {
            INFO("case: ", what);
            AggQuery full = make_query(*fx, ask, FIVE_SECONDS_US);
            AggQuery trimmed = full;
            trimmed.keep.tids = false;
            trimmed.keep.files = false;
            trimmed.keep.pid_files = false;
            trimmed.keep.pid_spans = false;
            trimmed.keep.offset = false;
            const AggQueryResult a = run(full);
            const AggQueryResult b = run(trimmed);
            REQUIRE(a.rows.size() == b.rows.size());
            std::map<Key, const agg::AggPartial*> by_key;
            for (const auto& [k, p] : a.rows) by_key[row_key(a, k)] = &p;
            for (const auto& [k, p] : b.rows) {
                const auto it = by_key.find(row_key(b, k));
                REQUIRE(it != by_key.end());
                const agg::AggPartial& want = *it->second;
                CHECK(p.count == want.count);
                CHECK(p.ts == want.ts);
                CHECK(p.te == want.te);
                check_same_stat(p.duration, want.duration, "duration");
                check_same_stat(p.size, want.size, "size");
                check_same_quantiles(p.duration_q, want.duration_q);
                check_same_quantiles(p.size_q, want.size_q);
                CHECK(p.pids.count() == want.pids.count());
                CHECK(p.pid_tids.count() == want.pid_tids.count());
                CHECK(p.tids.empty());
                CHECK(p.files.empty());
                CHECK(p.pid_files.empty());
                CHECK(p.pid_spans.empty());
                CHECK(p.offset.n == 0);
                CHECK(p.offset_q.count() == 0);
                CHECK(p.offset_values == 0);
            }
            CHECK(a.stats.chunks_reread == b.stats.chunks_reread);
        };
        SUBCASE("category and name from the stored partials") {
            Ask ask;
            ask.quantiles = true;
            check_keep(ask, "cat+name");
        }
        SUBCASE("pid and tid keys, some rows from a re-read") {
            Ask ask;
            ask.by_pid = true;
            ask.by_tid = true;
            ask.quantiles = true;
            check_keep(ask, "pid+tid");
        }
    }

    TEST_CASE("windows re-read only the chunks that cross a boundary") {
        SUBCASE("5 s windows") {
            auto fx = build(FIVE_SECONDS_US, "five");
            Ask ask;
            ask.window = FIVE_SECONDS_US;
            const auto r = check(*fx, ask, FIVE_SECONDS_US, "5 s window");
            CHECK(r.stats.chunks_reread > 0);
            CHECK(r.stats.partials_merged > 0);
            CHECK(r.stats.partials_merged + r.stats.partials_reread ==
                  r.stats.partials_read);
            ask.quantiles = true;
            check(*fx, ask, FIVE_SECONDS_US, "5 s window+quantiles");
        }
        SUBCASE("1 s windows") {
            auto fx = build(ONE_SECOND_US, "one");
            Ask ask;
            ask.window = ONE_SECOND_US;
            const auto r = check(*fx, ask, ONE_SECOND_US, "1 s window");
            CHECK(r.stats.chunks_reread > 0);
            CHECK(r.stats.partials_merged > 0);
        }
    }

    TEST_CASE("sums only splits partials by their time bins") {
        // Rows keep only the count and the n and sum of duration and size.
        const auto check_sums =
            [](const Fixture& fx, const Ask& ask,
               std::uint64_t interval_us) -> AggQueryResult {
            AggQuery q = make_query(fx, ask, interval_us);
            q.sums_only = true;
            AggQueryResult r = run(q);
            const auto truth =
                reference(fx.events, ask, make_config(interval_us));
            CHECK(r.rows.empty());
            REQUIRE(r.sums.size() == truth.size());
            for (const auto& [k, p] : r.sums) {
                const Key key = row_key(r, k);
                const auto t = truth.find(key);
                REQUIRE(t != truth.end());
                const agg::AggregationMetrics& ref = t->second.metrics;
                CHECK(p.count == ref.count);
                CHECK(p.duration.n == ref.duration.stat.n);
                CHECK(p.duration.sum == ref.duration.stat.sum);
                CHECK(p.size.n == ref.size.stat.n);
                CHECK(p.size.sum == ref.size.stat.sum);
            }
            return r;
        };
        SUBCASE("pid, tid and 5 s windows over 1 s bins read no chunk") {
            auto fx = build(ONE_SECOND_US, "one");
            Ask ask;
            ask.by_pid = true;
            ask.by_tid = true;
            ask.window = FIVE_SECONDS_US;
            const auto r = check_sums(*fx, ask, ONE_SECOND_US);
            CHECK(r.stats.partials_binned > 0);
            CHECK(r.stats.chunks_reread == 0);
            CHECK(r.stats.events_read == 0);
        }
        SUBCASE("events out of time order fill the same bins") {
            auto fx = build(ONE_SECOND_US, "reversed", false, true);
            Ask ask;
            ask.by_pid = true;
            ask.by_tid = true;
            ask.window = FIVE_SECONDS_US;
            const auto r = check_sums(*fx, ask, ONE_SECOND_US);
            CHECK(r.stats.partials_binned > 0);
            CHECK(r.stats.chunks_reread == 0);
        }
        SUBCASE("a window that is not a multiple of the bins re-reads") {
            auto fx = build(FIVE_SECONDS_US, "five");
            Ask ask;
            ask.by_tid = true;
            ask.window = 2 * ONE_SECOND_US;
            const auto r = check_sums(*fx, ask, FIVE_SECONDS_US);
            CHECK(r.stats.partials_binned == 0);
            CHECK(r.stats.chunks_reread > 0);
        }
    }

    TEST_CASE("keys finer than category and name re-read their chunks") {
        auto fx = build(FIVE_SECONDS_US, "five");
        const std::size_t partials = partial_count(*fx);
        SUBCASE("pid") {
            Ask ask;
            ask.by_pid = true;
            const auto r = check(*fx, ask, FIVE_SECONDS_US, "pid");
            CHECK(r.stats.chunks_reread > 0);
            CHECK(r.stats.partials_read == partials);
        }
        SUBCASE("tid") {
            Ask ask;
            ask.by_tid = true;
            const auto r = check(*fx, ask, FIVE_SECONDS_US, "tid");
            CHECK(r.stats.chunks_reread > 0);
        }
        SUBCASE("file") {
            Ask ask;
            ask.by_file = true;
            const auto r = check(*fx, ask, FIVE_SECONDS_US, "file");
            CHECK(r.stats.chunks_reread > 0);
            CHECK(r.stats.partials_merged == 0);
        }
        SUBCASE("pid, tid and a window together") {
            Ask ask;
            ask.by_pid = true;
            ask.by_tid = true;
            ask.window = FIVE_SECONDS_US;
            ask.quantiles = true;
            check(*fx, ask, FIVE_SECONDS_US, "pid+tid+window+quantiles");
        }
    }

    TEST_CASE("a re-read answers from the chunk column blocks") {
        auto fx = std::make_unique<Fixture>();
        const std::string pfw = fx->env.get_dir() + "/mixed.pfw";
        {
            std::ofstream file(pfw);
            file << mixed_trace();
        }
        const std::string gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
            pfw, gz, CHECKPOINT_SIZE));
        common::fs::remove(pfw);
        fx->files.push_back(gz);
        fx->index_dir = fx->env.get_dir() + "/idx_mixed";
        index::IndexerOptions o;
        o.index_dir = fx->index_dir;
        o.checkpoint_size = CHECKPOINT_SIZE;
        o.aggregation = make_config(FIVE_SECONDS_US);
        index::Indexer::open(fx->files, o).build();
        fx->index_path =
            trace::internal::determine_index_path(gz, fx->index_dir);

        // Every key finer than category and name, with quantiles: the stored
        // partials answer none of it whole, so every chunk is re-read.
        Ask ask;
        ask.by_pid = true;
        ask.by_tid = true;
        ask.by_file = true;
        ask.window = FIVE_SECONDS_US;
        ask.quantiles = true;

        AggQueryResult by_columns = run(make_query(*fx, ask, FIVE_SECONDS_US));
        REQUIRE(by_columns.stats.chunks_reread > 0);
        CHECK(by_columns.stats.files_without_partials == 0);
        CHECK(by_columns.stats.chunks_from_columns ==
              by_columns.stats.chunks_reread);
        CHECK(by_columns.stats.max_batch_events == 0);
        CHECK(by_columns.rows.size() > 20);

        // A filter keeps every record and so parses every re-read chunk.
        AggQuery filtered = make_query(*fx, ask, FIVE_SECONDS_US);
        filtered.filter = duql::parse_or_throw("ts >= 0");
        AggQueryResult by_events = run(filtered);
        CHECK(by_events.stats.chunks_from_columns == 0);
        CHECK(by_events.stats.chunks_reread == by_columns.stats.chunks_reread);
        CHECK(by_events.stats.max_batch_events > 0);
        CHECK(by_events.stats.events_read == by_columns.stats.events_read);

        REQUIRE(by_columns.rows.size() == by_events.rows.size());
        for (const auto& [k, a] : by_columns.rows) {
            const auto b = by_events.rows.find([&] {
                trace::AggKey key = k;
                key.cat =
                    by_events.strings().get_or_insert(by_columns.text(k.cat));
                key.name =
                    by_events.strings().get_or_insert(by_columns.text(k.name));
                return key;
            }());
            REQUIRE_MESSAGE(b != by_events.rows.end(), by_columns.text(k.cat),
                            " ", by_columns.text(k.name), " pid ", k.pid,
                            " tid ", k.tid, " file ", k.file, " window ",
                            k.window);
            check_same_partial(a, b->second);
        }
    }

    TEST_CASE("filters on category, name, pid and time") {
        auto fx = build(FIVE_SECONDS_US, "five");
        SUBCASE("category and name") {
            Ask ask;
            ask.categories = {"posix"};
            ask.names = {"read", "write"};
            const auto r = check(*fx, ask, FIVE_SECONDS_US, "cat+name filter");
            CHECK(r.stats.partials_pruned > 0);
            CHECK(r.stats.chunks_reread == 0);
        }
        SUBCASE("pid and time range with windows") {
            Ask ask;
            ask.pids = {100, 102};
            ask.ts_begin = BASE_TS + 3 * ONE_SECOND_US;
            ask.ts_end = BASE_TS + 9 * ONE_SECOND_US;
            ask.window = FIVE_SECONDS_US;
            const auto r = check(*fx, ask, FIVE_SECONDS_US, "pid+time+window");
            CHECK(r.stats.chunks_reread > 0);
        }
    }

    TEST_CASE("a file indexed without partials is read whole") {
        auto fx = build(FIVE_SECONDS_US, "five");
        {
            auto opened = index::Index::open(
                index::store::internal::normalize_index_root(fx->index_path));
            REQUIRE(opened.has_value());
            const auto snapshot = opened->snapshot();
            REQUIRE(snapshot.has_value());
            const auto file = snapshot->file(
                index::store::internal::get_logical_path(fx->files[0]));
            REQUIRE(file.has_value());
            const auto state = index::extension_state(
                *snapshot, *file, index::store::IndexExtension::Agg);
            REQUIRE(state.has_value());
            auto rebuilt = index::rebuild(
                *opened,
                index::store::internal::get_logical_path(fx->files[0]));
            REQUIRE(rebuilt.has_value());
            rebuilt->finish(index::store::IndexExtension::Agg,
                            state->params_hash);
            REQUIRE(
                default_runtime().submit(rebuilt->commit()).get().has_value());
        }
        SUBCASE("no window") {
            Ask ask;
            const auto r = check(*fx, ask, FIVE_SECONDS_US, "no partials");
            CHECK(r.stats.files_without_partials == 1);
            CHECK(r.stats.chunks_reread > 0);
        }
        SUBCASE("windows, pid and quantiles") {
            Ask ask;
            ask.window = FIVE_SECONDS_US;
            ask.by_pid = true;
            ask.quantiles = true;
            const auto r =
                check(*fx, ask, FIVE_SECONDS_US, "no partials+window+pid");
            CHECK(r.stats.files_without_partials == 1);
        }
        SUBCASE("a time range prunes the chunks read") {
            Ask ask;
            ask.ts_begin = BASE_TS + 2 * ONE_SECOND_US;
            ask.ts_end = BASE_TS + 4 * ONE_SECOND_US;
            const auto r = check(*fx, ask, FIVE_SECONDS_US, "no partials+time");
            CHECK(r.stats.files_without_partials == 1);
        }
    }

    TEST_CASE("an index built under other params is read whole") {
        auto fx = build(FIVE_SECONDS_US, "five");
        Ask ask;
        // The query asks with another config than the index was built with.
        AggQuery q = make_query(*fx, ask, FIVE_SECONDS_US);
        q.config.compute_percentiles = false;
        const AggQueryResult r = run(q);
        CHECK(r.stats.files_without_partials == FILES);
        CHECK(r.stats.partials_read == 0);
        CHECK(r.stats.chunks_reread > 0);
        CHECK(r.stats.chunks_from_columns == r.stats.chunks_reread);
        CHECK(r.stats.events_read == fx->events.size());
    }

    TEST_CASE("a read of many tiny events decodes them in bounded batches") {
        constexpr int TINY_EVENTS = 40000;
        constexpr std::size_t ONE_CHUNK = std::size_t{64} << 20;
        auto fx = std::make_unique<Fixture>();
        std::ostringstream text;
        text << "[\n";
        for (int i = 0; i < TINY_EVENTS; ++i)
            text << R"({"id":)" << i << R"(,"name":"r","cat":"c","pid":1,)"
                 << R"("tid":1,"ph":"X","ts":)" << BASE_TS + i
                 << R"(,"dur":1,"args":{}})" << "\n";
        text << "]\n";
        fx->files.push_back(dftu_utils_test::write_gz_trace(
            fx->env.get_dir() + "/tiny.pfw.gz", text.str()));
        fx->index_dir = fx->env.get_dir() + "/idx_tiny";
        index::IndexerOptions o;
        o.index_dir = fx->index_dir;
        o.checkpoint_size = ONE_CHUNK;
        o.aggregation = make_config(FIVE_SECONDS_US);
        index::Indexer::open(fx->files, o).build();
        fx->index_path =
            trace::internal::determine_index_path(fx->files[0], fx->index_dir);

        AggQuery q = make_query(*fx, Ask{}, FIVE_SECONDS_US);
        q.config.compute_percentiles = false;
        // A filter keeps every event and so answers from the parse alone.
        q.filter = duql::parse_or_throw("ts >= 0");
        const AggQueryResult r = run(q);
        CHECK(r.stats.chunks_from_columns == 0);
        CHECK(r.stats.events_read == TINY_EVENTS);
        CHECK(r.stats.max_batch_events > 0);
        CHECK(r.stats.max_batch_events <=
              trace::views::detail::FoldEventBuffer::RETAIN_SLOTS);
    }

    TEST_CASE("a member cut inside a line neither doubles nor drops it") {
        auto fx = build(FIVE_SECONDS_US, "cut", true);
        SUBCASE("5 s windows") {
            Ask ask;
            ask.window = FIVE_SECONDS_US;
            const auto r = check(*fx, ask, FIVE_SECONDS_US, "cut 5 s window");
            CHECK(r.stats.partials_merged > 0);
            CHECK(r.stats.chunks_reread > 0);
        }
        SUBCASE("pid and tid") {
            Ask ask;
            ask.by_pid = true;
            ask.by_tid = true;
            const auto r = check(*fx, ask, FIVE_SECONDS_US, "cut pid+tid");
            CHECK(r.stats.chunks_reread > 0);
        }
        SUBCASE("file") {
            Ask ask;
            ask.by_file = true;
            check(*fx, ask, FIVE_SECONDS_US, "cut file");
        }
    }

    TEST_CASE(
        "parallel reads merge in file order, so the sums match bit for bit") {
        auto fx = build(FIVE_SECONDS_US, "five");
        Ask ask;
        ask.quantiles = true;
        AggQuery q = make_query(*fx, ask, FIVE_SECONDS_US);
        q.ordered = true;
        q.workers = 1;
        const AggQueryResult serial = run(q);
        std::map<Key, const agg::AggPartial*> want;
        for (const auto& [k, p] : serial.rows) want[row_key(serial, k)] = &p;
        q.workers = 0;
        for (int round = 0; round < 8; ++round) {
            const AggQueryResult got = run(q);
            REQUIRE(got.rows.size() == serial.rows.size());
            for (const auto& [k, p] : got.rows) {
                const auto it = want.find(row_key(got, k));
                REQUIRE(it != want.end());
                const FieldStat& a = it->second->duration;
                CHECK(p.duration.sum == a.sum);
                CHECK(p.duration.cmean == a.cmean);
                CHECK(p.duration.cm2 == a.cm2);
                CHECK(p.duration.cm3 == a.cm3);
                CHECK(p.size.cm2 == it->second->size.cm2);
            }
        }
    }

    TEST_CASE("parallel reads give the serial result") {
        auto fx = build(FIVE_SECONDS_US, "five");
        Ask ask;
        ask.by_pid = true;
        ask.window = FIVE_SECONDS_US;
        ask.quantiles = true;
        const auto timed = [&](std::size_t workers, AggQueryResult& out) {
            AggQuery q = make_query(*fx, ask, FIVE_SECONDS_US);
            q.workers = workers;
            const auto t0 = std::chrono::steady_clock::now();
            out = run(q);
            return std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                .count();
        };
        AggQueryResult serial, parallel;
        timed(1, serial);  // warm
        const double serial_ms = timed(1, serial);
        const double parallel_ms = timed(0, parallel);
        MESSAGE("serial ", serial_ms, " ms, parallel ", parallel_ms, " ms");
        REQUIRE(serial.rows.size() == parallel.rows.size());
        for (const auto& [k, a] : serial.rows) {
            const auto b = parallel.rows.find([&] {
                trace::AggKey key = k;
                // Ids are per result, so compare through the text.
                key.cat = parallel.strings().get_or_insert(serial.text(k.cat));
                key.name =
                    parallel.strings().get_or_insert(serial.text(k.name));
                return key;
            }());
            REQUIRE(b != parallel.rows.end());
            const agg::AggPartial& c = b->second;
            CHECK(a.count == c.count);
            CHECK(a.ts == c.ts);
            CHECK(a.te == c.te);
            CHECK(a.ts_last_start == c.ts_last_start);
            check_stat(a.duration, c.duration, "duration");
            check_stat(a.size, c.size, "size");
            CHECK(a.pids.count() == c.pids.count());
            CHECK(a.tids.count() == c.tids.count());
            CHECK(a.files.count() == c.files.count());
            CHECK(a.duration_q.count() == c.duration_q.count());
        }
        CHECK(serial.stats.partials_merged == parallel.stats.partials_merged);
        CHECK(serial.stats.partials_reread == parallel.stats.partials_reread);
        CHECK(serial.stats.events_read == parallel.stats.events_read);
    }
}
