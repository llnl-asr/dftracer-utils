// The aggregation query over real indexes: every group-by, window and filter
// gives the rows today's per-event aggregator gives for the same events, and
// the chunks that cannot be answered from the stored partials are re-read.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_core.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/comparator/comparison_aggregation.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <rocksdb_exit_listener.h>
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
    std::string fhash;
    std::vector<std::pair<std::string, std::uint64_t>> args;
};

class Acc {
   public:
    explicit Acc(const Ev& e) : e_(e) {}

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

    bool arg_exists(std::string_view k) const {
        if (k == "fhash") return !e_.fhash.empty();
        return find(k) != nullptr;
    }
    bool arg_is_number(std::string_view k) const {
        return k != "fhash" && find(k) != nullptr;
    }
    std::uint64_t arg_uint(std::string_view k) const {
        const auto* v = find(k);
        return v ? *v : 0;
    }
    double arg_double(std::string_view k) const {
        return static_cast<double>(arg_uint(k));
    }
    std::string_view arg_string(std::string_view k) const {
        return k == "fhash" ? std::string_view(e_.fhash) : std::string_view{};
    }
    template <class Fn>
    void for_each_numeric_arg(Fn&& fn) const {
        for (const auto& [k, v] : e_.args)
            fn(std::string_view(k), v, static_cast<double>(v));
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
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return s >> 17;
}

agg::AggregationConfig make_config(std::uint64_t interval_us) {
    agg::AggregationConfig cfg;
    cfg.time_interval_us = interval_us;
    cfg.group_by_file = false;
    cfg.compute_percentiles = true;
    cfg.custom_metric_fields = {"flops"};
    return cfg;
}

// POSIX read/write and STDIO fread/fwrite carry ret and offset; open carries a
// ret that is a descriptor and must not count as a size; compute carries
// numeric args only; "rare" has fewer than 64 events so its quantiles stay
// exact; every 17th POSIX transfer is a pre-aggregated counter. Events are in
// time order and span about 14 s.
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
    fs::remove(pfw);
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

// What the query is asked, in the terms of the events.
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

// Today's aggregator, one state per group key of the query.
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

void check_group(const AggQueryResult& r, const agg::AggPartial& p,
                 const Truth& t, bool quantiles, bool args) {
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

    if (!args) {
        CHECK(p.args.empty());
        return;
    }
    const std::size_t custom =
        ref.custom_metrics ? ref.custom_metrics->size() : 0;
    REQUIRE(p.args.size() == custom);
    if (!ref.custom_metrics) return;
    for (const auto& [name, stats] : *ref.custom_metrics) {
        const auto arg = std::find_if(
            p.args.begin(), p.args.end(), [&](const agg::ArgStat& a) {
                return r.text(a.id) == std::string_view(name);
            });
        REQUIRE(arg != p.args.end());
        check_stat(arg->stat, stats.stat, std::string(name).c_str());
    }
}

struct Fixture {
    dftu_utils_test::TestEnvironment env{10};
    std::vector<Ev> events;  // all files
    std::vector<std::string> files;
    std::string index_dir;
    std::string index_path;
};

// Builds the index of the FILES traces under `interval_us`.
std::unique_ptr<Fixture> build(std::uint64_t interval_us,
                               const std::string& tag,
                               bool cut_mid_line = false) {
    auto fx = std::make_unique<Fixture>();
    for (int i = 0; i < FILES; ++i) {
        auto events = make_events(1000 + static_cast<std::uint64_t>(i));
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

// Runs `ask` and compares every row with today's aggregator. Returns the
// result so a case can assert how it was answered.
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
        check_group(r, p, t->second, ask.quantiles, q.args);
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

// Every partial of every file, to know how many chunks there are.
std::size_t partial_count(const Fixture& fx) {
    index::store::IndexDatabase db(fx.index_path,
                                   index::store::IndexOpenMode::ReadOnly);
    std::size_t n = 0;
    for (const auto& f : fx.files) {
        const int fid =
            db.get_file_info_id(index::store::internal::get_logical_path(f));
        REQUIRE(fid >= 0);
        agg::tier::for_each_partial(
            *db.db(), fid,
            [&](std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                std::uint32_t, agg::PartialKind,
                std::span<const std::uint8_t>) {
                ++n;
                return true;
            });
    }
    return n;
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
        o.aggregation = trace::comparator::comparison_index_config();
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
            index::store::IndexDatabase db(fx->index_path);
            const int fid = db.get_file_info_id(
                index::store::internal::get_logical_path(fx->files[0]));
            REQUIRE(fid >= 0);
            auto w = db.begin_write();
            const auto begin =
                layout::file_prefix(layout::Ext::Agg, layout::agg_kind::PARTIAL,
                                    static_cast<std::uint32_t>(fid));
            w->delete_range(layout::Family::Blob, begin,
                            layout::prefix_end(begin));
            w->commit();
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
        q.config.track_default_args = false;
        const AggQueryResult r = run(q);
        CHECK(r.stats.files_without_partials == FILES);
        CHECK(r.stats.partials_read == 0);
        CHECK(r.stats.chunks_reread > 0);
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
        q.config.track_default_args = false;
        const AggQueryResult r = run(q);
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
                // Ids are per result; compare through the text.
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
