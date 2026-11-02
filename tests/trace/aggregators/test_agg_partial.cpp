// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial_update.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_core.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/memory/heap.h>
#include <doctest/doctest.h>
#include <mmix_lcg.h>
#include <trace/aggregators/aggregation_oracle.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dftracer::utils::index::schemas::dft::agg;
using dftracer::utils::dataframe::FieldStat;
using dftracer::utils::dataframe::FieldStatDomain;
using dftracer::utils::dstd::Span;
using dftracer::utils::dstd::Vector;

namespace {

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
    void metric_args(MetricArgs& m) const {
        for (const auto& [name, v] : e_.args) {
            for (std::size_t i = 0; i < METRIC_ARG_COUNT; ++i)
                if (name == METRIC_ARG_NAMES[i]) {
                    m.add(static_cast<MetricArg>(i), v);
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

using Key = std::pair<std::string, std::string>;

std::uint64_t next(std::uint64_t& s) {
    s = mmix_lcg_step(s);
    return s >> 17;
}

AggregationConfig make_config(bool percentiles) {
    AggregationConfig cfg;
    cfg.group_by_file = false;
    cfg.compute_percentiles = percentiles;
    return cfg;
}

// POSIX read/write and STDIO fread/fwrite carry ret and offset. Open carries a
// ret that is a descriptor and must not count as a size. Compute carries
// numeric args only. "rare" has fewer than 64 events, so its quantiles stay
// exact. Every 17th POSIX transfer is a pre-aggregated counter.
std::vector<Ev> make_events(std::size_t n, std::uint64_t seed) {
    static const char* const FILES[] = {"1a2b3c4d", "deadbeef", "0badf00d",
                                        "cafe1234", "feed5678", "abcdef01",
                                        "13572468"};
    std::vector<Ev> out;
    out.reserve(n);
    std::uint64_t s = seed;
    for (std::size_t i = 0; i < n; ++i) {
        Ev e;
        e.pid = 100 + next(s) % 4;
        e.tid = e.pid * 10 + next(s) % 6;
        e.ts = 1000 + i * 13 + next(s) % 50;
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
        if (e.cat != "app") e.fhash = FILES[next(s) % 7];
        if (transfer && e.cat == "POSIX" && i % 17 == 0) {
            e.counter = true;
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
            e.args.emplace_back("iter", i);
        }
        if (next(s) % 2 == 0) e.args.emplace_back("flops", next(s) % 100000);
        out.push_back(std::move(e));
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

Key key_of(const Ev& e) { return {lower(e.cat), e.name}; }

bool is_transfer(const Ev& e) {
    const std::string c = lower(e.cat);
    return (c == "posix" || c == "stdio") &&
           (e.name == "read" || e.name == "write" || e.name == "fread" ||
            e.name == "fwrite");
}

struct Truth {
    std::set<std::uint64_t> pids, tids, files;
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> spans;
    std::vector<double> dur, size, off;
};

std::map<Key, Truth> truth_of(const std::vector<Ev>& events,
                              const AggregationConfig& cfg) {
    std::map<Key, Truth> out;
    for (const Ev& e : events) {
        Truth& t = out[key_of(e)];
        t.pids.insert(e.pid);
        t.tids.insert(e.tid);
        if (!e.fhash.empty()) t.files.insert(agg_hash_of_hex(e.fhash));
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

std::map<Key, AggregationMetrics> reference(const std::vector<Ev>& events,
                                            const AggregationConfig& cfg) {
    std::map<Key, AggregationMetrics> out;
    for (const Ev& e : events) {
        auto it = out.try_emplace(key_of(e), cfg.sketch_accuracy).first;
        oracle_event(Acc(e), it->second, cfg);
    }
    return out;
}

using Partials = std::map<Key, AggPartial>;

void feed(Partials& into, const std::vector<Ev>& events, std::size_t b,
          std::size_t e, const AggregationConfig& cfg) {
    for (std::size_t i = b; i < e; ++i) {
        auto it =
            into.try_emplace(key_of(events[i]), cfg.sketch_accuracy).first;
        REQUIRE(update_partial(it->second, Acc(events[i]), cfg));
    }
}

AggPartial round_trip(const AggPartial& p) {
    Vector<std::uint8_t> bytes;
    p.serialize_into(bytes);
    auto back = AggPartial::deserialize(Span<const std::uint8_t>(bytes));
    REQUIRE(back.has_value());
    return std::move(*back);
}

bool near(double a, double b, double scale, double tol) {
    return std::abs(a - b) <= tol * std::max(scale, 1e-300);
}

// tol 0 means bit-exact moments after a round trip. Otherwise it is a relative
// bound for a different order of merges.
void check_stat(const FieldStat& a, const FieldStat& b, double tol,
                const char* what) {
    INFO("field: ", what);
    REQUIRE(a.n == b.n);
    if (a.n == 0) return;
    CHECK(a.domain == b.domain);
    CHECK(a.sum == b.sum);
    CHECK(a.min == b.min);
    CHECK(a.max == b.max);
    if (a.domain != FieldStatDomain::F64 && a.domain == b.domain) {
        CHECK(a.esum == b.esum);
        CHECK(a.emin == b.emin);
        CHECK(a.emax == b.emax);
        CHECK(a.ecarry == b.ecarry);
    }
    const double s2 = std::max(std::abs(a.cm2), std::abs(b.cm2));
    CHECK(near(a.cm2, b.cm2, s2, tol));
    CHECK(near(a.cm3, b.cm3, std::pow(s2, 1.5), tol));
    CHECK(near(a.cm4, b.cm4, s2 * s2, tol));
    CHECK(near(a.moment_mean(), b.moment_mean(),
               std::max(std::abs(a.moment_mean()), 1.0), tol));
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

constexpr double QS[] = {0.1, 0.5, 0.9, 0.99};
constexpr double SKETCH_BOUND = 0.03;

void check_quantiles(const QuantileStore& q, const std::vector<double>& truth,
                     const char* what) {
    INFO("quantiles of ", what);
    REQUIRE(q.count() == truth.size());
    if (truth.empty()) return;
    CHECK(q.is_exact() == (truth.size() <= 64));
    for (double p : QS) {
        const double want = exact_rule(truth, p);
        if (truth.size() <= 64) {
            CHECK(q.quantile(p) == doctest::Approx(want).epsilon(1e-12));
        } else {
            CHECK(std::abs(q.quantile(p) - want) <=
                  SKETCH_BOUND * std::abs(want) + 1e-9);
        }
    }
}

void check_against_reference(const AggPartial& p, const AggregationMetrics& ref,
                             const Truth& t) {
    CHECK(p.count == ref.count);
    CHECK(p.ts == ref.ts);
    CHECK(p.te == ref.te);
    check_stat(p.duration, ref.duration.stat, 1e-9, "duration");
    check_stat(p.size, ref.size.stat, 1e-9, "size");
    check_stat(p.offset, ref.offset.stat, 1e-9, "offset");

    CHECK(p.pids.is_exact());
    CHECK(p.pids.count() == t.pids.size());
    CHECK(p.tids.count() == t.tids.size());
    CHECK(p.files.count() == t.files.size());
    CHECK(p.files.count() == ref.distinct_files.estimate());
    REQUIRE(p.pids.ids().size() == t.pids.size());
    CHECK(std::equal(p.pids.ids().begin(), p.pids.ids().end(), t.pids.begin()));

    REQUIRE(p.pid_spans.size() == t.spans.size());
    std::size_t at = 0;
    for (const auto& [pid, span] : t.spans) {
        CHECK(p.pid_spans[at].pid == pid);
        CHECK(p.pid_spans[at].first == span.first);
        CHECK(p.pid_spans[at].last == span.second);
        ++at;
    }

    check_quantiles(p.duration_q, t.dur, "duration");
    check_quantiles(p.size_q, t.size, "size");
    check_quantiles(p.offset_q, t.off, "offset");
}

void check_same(const AggPartial& a, const AggPartial& b, double tol) {
    CHECK(a.count == b.count);
    CHECK(a.ts == b.ts);
    CHECK(a.te == b.te);
    check_stat(a.duration, b.duration, tol, "duration");
    check_stat(a.size, b.size, tol, "size");
    check_stat(a.offset, b.offset, tol, "offset");
    CHECK(a.duration_q.count() == b.duration_q.count());
    CHECK(a.size_q.count() == b.size_q.count());
    CHECK(a.offset_q.count() == b.offset_q.count());
    CHECK(a.duration_q.is_exact() == b.duration_q.is_exact());
    for (auto d : {&AggPartial::pids, &AggPartial::tids, &AggPartial::files}) {
        const auto& x = a.*d;
        const auto& y = b.*d;
        CHECK(x.is_exact() == y.is_exact());
        CHECK(x.count() == y.count());
        CHECK(std::equal(x.ids().begin(), x.ids().end(), y.ids().begin(),
                         y.ids().end()));
    }
    REQUIRE(a.pid_spans.size() == b.pid_spans.size());
    for (std::size_t i = 0; i < a.pid_spans.size(); ++i) {
        CHECK(a.pid_spans[i].pid == b.pid_spans[i].pid);
        CHECK(a.pid_spans[i].first == b.pid_spans[i].first);
        CHECK(a.pid_spans[i].last == b.pid_spans[i].last);
    }
}

std::vector<std::size_t> bounds(std::size_t n, std::size_t chunks) {
    std::vector<std::size_t> out{0};
    for (std::size_t c = 1; c < chunks; ++c)
        out.push_back(n * (c * c) / (chunks * chunks) + c);
    out.push_back(n);
    return out;
}

Partials merged_chunks(const std::vector<Ev>& events,
                       const AggregationConfig& cfg, std::size_t chunks,
                       const std::vector<std::size_t>& order,
                       bool through_wire) {
    const auto cut = bounds(events.size(), chunks);
    std::vector<Partials> per_chunk(chunks);
    for (std::size_t c = 0; c < chunks; ++c)
        feed(per_chunk[c], events, cut[c], cut[c + 1], cfg);
    Partials out;
    for (std::size_t c : order) {
        for (auto& [k, p] : per_chunk[c]) {
            auto it = out.find(k);
            if (it == out.end()) {
                it = out.try_emplace(k, cfg.sketch_accuracy).first;
            }
            if (through_wire) {
                AggPartial back = round_trip(p);
                it->second.merge(back);
            } else {
                it->second.merge(p);
            }
        }
    }
    return out;
}

}  // namespace

TEST_SUITE("AggPartial") {
    TEST_CASE("merged chunk partials equal today's aggregator on the file") {
        const auto cfg = make_config(true);
        const auto events = make_events(4000, 11);
        const auto ref = reference(events, cfg);
        const auto truth = truth_of(events, cfg);

        for (std::size_t chunks : {3u, 4u, 5u}) {
            INFO("chunks: ", chunks);
            std::vector<std::size_t> order;
            for (std::size_t c = 0; c < chunks; ++c) order.push_back(c);
            const auto merged = merged_chunks(events, cfg, chunks, order, true);
            REQUIRE(merged.size() == ref.size());
            for (const auto& [k, p] : merged) {
                INFO("group: ", k.first, " ", k.second);
                const auto r = ref.find(k);
                REQUIRE(r != ref.end());
                check_against_reference(p, r->second, truth.at(k));
            }
        }
    }

    TEST_CASE("one partial over the whole file equals the aggregator") {
        const auto cfg = make_config(true);
        const auto events = make_events(2500, 5);
        const auto ref = reference(events, cfg);
        const auto truth = truth_of(events, cfg);
        Partials whole;
        feed(whole, events, 0, events.size(), cfg);
        for (const auto& [k, p] : whole) {
            INFO("group: ", k.first, " ", k.second);
            check_against_reference(p, ref.at(k), truth.at(k));
        }
    }

    TEST_CASE("the metric args of a POD event clamp like get<uint64>") {
        dftracer::utils::dstd::StringIntern intern;
        const MetricArgIds ids(intern);
        const auto id = [&intern](std::string_view name) {
            return intern.get_or_insert(name);
        };
        dftracer::utils::trace::views::detail::FoldEvent ev;
        // A repeated arg keeps its first value. A negative or too large number
        // clamps to 0. A string arg is present with no number. An arg the
        // metric rules do not read is left alone.
        ev.args.emplace_back(id("dur_sum"), 12.0);
        ev.args.emplace_back(id("dur_sum"), 99.0);
        ev.args.emplace_back(id("dur_min"), -3.0);
        ev.args.emplace_back(id("dur_max"), 1e30);
        ev.args.emplace_back(id("dur"), 3.5);
        ev.args.emplace_back(id("ret_sum"), std::int64_t{-5});
        ev.args.emplace_back(id("ret_min"), std::int64_t{7});
        ev.args.emplace_back(id("offset_sum"), id("text"));
        ev.args.emplace_back(id("offset_max"), std::uint64_t{1} << 63);
        ev.args.emplace_back(id("flops"), 1234.0);

        const PodAccessor acc(ev, intern, ids);
        MetricArgs m;
        acc.metric_args(m);
        CHECK(m.has(MetricArg::DurSum));
        CHECK(m.get(MetricArg::DurSum) == 12);
        CHECK(m.has(MetricArg::DurMin));
        CHECK(m.get(MetricArg::DurMin) == 0);
        CHECK(m.has(MetricArg::DurMax));
        CHECK(m.get(MetricArg::DurMax) == 0);
        CHECK(m.get(MetricArg::Dur) == 3);
        CHECK(m.has(MetricArg::RetSum));
        CHECK(m.get(MetricArg::RetSum) == 0);
        CHECK(m.get(MetricArg::RetMin) == 7);
        CHECK(m.has(MetricArg::OffsetSum));
        CHECK(m.get(MetricArg::OffsetSum) == 0);
        CHECK(m.get(MetricArg::OffsetMax) == (std::uint64_t{1} << 63));
        CHECK_FALSE(m.has(MetricArg::Count));
        CHECK_FALSE(m.has(MetricArg::DftCnt));
        CHECK_FALSE(m.has(MetricArg::Offset));
        CHECK_FALSE(m.has(MetricArg::Ret));
    }

    TEST_CASE("a counter's fixed args give the partial their sums and bounds") {
        const auto cfg = make_config(false);
        AggPartial p;

        Ev preaggregated;
        preaggregated.cat = "POSIX";
        preaggregated.name = "read";
        preaggregated.pid = 1;
        preaggregated.tid = 2;
        preaggregated.ts = 1000;
        preaggregated.counter = true;
        preaggregated.args = {{"count", 4},          {"dur_sum", 400},
                              {"dur_min", 90},       {"dur_max", 110},
                              {"ret_sum", 8192},     {"ret_min", 2048},
                              {"ret_max", 4096},     {"offset_sum", 65536},
                              {"offset_min", 16384}, {"offset_max", 32768}};
        REQUIRE(update_partial(p, Acc(preaggregated), cfg));
        CHECK(p.count == 4);
        CHECK(p.duration.n == 4);
        CHECK(p.duration.sum == 400.0);
        CHECK(p.duration.min == 90.0);
        CHECK(p.duration.max == 110.0);
        CHECK(p.size.n == 4);
        CHECK(p.size.sum == 8192.0);
        CHECK(p.size.min == 2048.0);
        CHECK(p.size.max == 4096.0);
        CHECK(p.offset.n == 4);
        CHECK(p.offset.sum == 65536.0);
        CHECK(p.offset.min == 16384.0);
        CHECK(p.offset.max == 32768.0);

        // Without a sum, min and max of a field, its plain arg gives all three,
        // and an event without count args counts once.
        Ev plain = preaggregated;
        plain.args = {{"dur", 50}, {"ret", 100}, {"offset", 7}};
        REQUIRE(update_partial(p, Acc(plain), cfg));
        CHECK(p.count == 5);
        CHECK(p.duration.n == 5);
        CHECK(p.duration.sum == 450.0);
        CHECK(p.duration.min == 50.0);
        CHECK(p.duration.max == 110.0);
        CHECK(p.size.n == 5);
        CHECK(p.size.sum == 8292.0);
        CHECK(p.size.min == 100.0);
        CHECK(p.size.max == 4096.0);
        CHECK(p.offset.n == 5);
        CHECK(p.offset.sum == 65543.0);
        CHECK(p.offset.min == 7.0);
        CHECK(p.offset.max == 32768.0);

        // dftu_cnt wins over count, and neither touches the other fields.
        Ev counted = preaggregated;
        counted.args = {{"dftu_cnt", 2}, {"count", 9}};
        REQUIRE(update_partial(p, Acc(counted), cfg));
        CHECK(p.count == 7);
        CHECK(p.duration.n == 5);
        CHECK(p.size.n == 5);
        CHECK(p.offset.n == 5);
    }

    TEST_CASE("an event without a duration counts but adds no duration") {
        const auto cfg = make_config(true);
        AggPartial p;

        Ev timed;
        timed.cat = "POSIX";
        timed.name = "open";
        timed.pid = 1;
        timed.tid = 2;
        timed.ts = 1000;
        timed.dur = 40;
        REQUIRE(update_partial(p, Acc(timed), cfg));

        Ev instant = timed;
        instant.ts = 2000;
        instant.dur = 0;
        instant.has_dur = false;
        REQUIRE(update_partial(p, Acc(instant), cfg));

        CHECK(p.count == 2);
        CHECK(p.duration.n == 1);
        CHECK(p.duration.sum == 40.0);
        CHECK(p.duration.min == 40.0);
        CHECK(p.duration.max == 40.0);
        CHECK(p.duration_values == 1);
    }

    TEST_CASE("the order of the merges does not change the exact fields") {
        const auto cfg = make_config(true);
        const auto events = make_events(3000, 23);
        const auto forward =
            merged_chunks(events, cfg, 5, {0, 1, 2, 3, 4}, false);
        const auto shuffled =
            merged_chunks(events, cfg, 5, {3, 0, 4, 2, 1}, false);
        const auto reverse =
            merged_chunks(events, cfg, 5, {4, 3, 2, 1, 0}, true);
        REQUIRE(forward.size() == shuffled.size());
        REQUIRE(forward.size() == reverse.size());
        for (const auto& [k, p] : forward) {
            INFO("group: ", k.first, " ", k.second);
            check_same(p, shuffled.at(k), 1e-9);
            check_same(p, reverse.at(k), 1e-9);
        }
    }

    TEST_CASE("a round trip keeps every field bit for bit") {
        const auto cfg = make_config(true);
        const auto events = make_events(3000, 31);
        Partials whole;
        feed(whole, events, 0, events.size(), cfg);
        for (const auto& [k, p] : whole) {
            INFO("group: ", k.first, " ", k.second);
            const AggPartial back = round_trip(p);
            check_same(p, back, 0.0);
            for (double q : QS) {
                if (p.duration_q.count() > 0)
                    CHECK(p.duration_q.quantile(q) ==
                          back.duration_q.quantile(q));
            }
        }
    }

    TEST_CASE("a partial built from no event round trips empty") {
        AggPartial empty;
        Vector<std::uint8_t> bytes;
        empty.serialize_into(bytes);
        auto back = AggPartial::deserialize(Span<const std::uint8_t>(bytes));
        REQUIRE(back.has_value());
        CHECK(back->count == 0);
        CHECK(back->ts == UINT64_MAX);
        CHECK(back->te == 0);
        CHECK(back->duration.n == 0);
        CHECK(back->pids.empty());
        CHECK(back->pid_spans.empty());
        CHECK_FALSE(back->has_estimate());

        AggPartial into;
        into.merge(*back);
        into.merge(empty);
        CHECK(into.count == 0);
        CHECK(into.pids.empty());
    }

    TEST_CASE("deserialize refuses truncated and over-long bytes") {
        const auto cfg = make_config(true);
        const auto events = make_events(1500, 3);
        Partials whole;
        feed(whole, events, 0, events.size(), cfg);
        const AggPartial& p = whole.at({"posix", "read"});
        REQUIRE(p.size.n > 0);
        REQUIRE(p.duration_q.count() > 0);
        Vector<std::uint8_t> bytes;
        p.serialize_into(bytes);
        REQUIRE(AggPartial::deserialize(Span<const std::uint8_t>(bytes))
                    .has_value());

        for (std::size_t len = 0; len < bytes.size(); ++len) {
            INFO("prefix length: ", len);
            CHECK_FALSE(AggPartial::deserialize(
                            Span<const std::uint8_t>(bytes.data(), len))
                            .has_value());
        }
        Vector<std::uint8_t> longer = bytes;
        longer.push_back(0);
        CHECK_FALSE(AggPartial::deserialize(Span<const std::uint8_t>(longer))
                        .has_value());

        Vector<std::uint8_t> bad_flags = bytes;
        bad_flags[0] = 0xF0;
        CHECK_FALSE(AggPartial::deserialize(Span<const std::uint8_t>(bad_flags))
                        .has_value());
    }

    TEST_CASE("time bins add up by key and survive the wire") {
        AggPartial p;
        p.count = 6;
        p.bin_interval = 1'000'000;
        // Out of order, with (7, 2, 4) twice.
        p.bins.push_back(TimeBin{7, 2, 4, 1, 1, 10, 0, 0});
        p.bins.push_back(TimeBin{3, 9, 1, 2, 1, 20, 2, 4096});
        p.bins.push_back(TimeBin{7, 1, 9, 1, 1, 5, 1, 8});
        p.bins.push_back(TimeBin{7, 2, 4, 1, 1, 30, 1, 16});
        p.bins.push_back(TimeBin{7, 2, 2, 1, 0, 0, 0, 0});
        p.normalize_bins();
        REQUIRE(p.bins.size() == 4);
        CHECK(p.bins[0].pid == 3);
        CHECK(p.bins[1].tid == 1);
        CHECK(p.bins[2].bin == 2);
        const TimeBin& added = p.bins[3];
        CHECK(added.pid == 7);
        CHECK(added.tid == 2);
        CHECK(added.bin == 4);
        CHECK(added.count == 2);
        CHECK(added.duration_n == 2);
        CHECK(added.duration_sum == 40);
        CHECK(added.size_n == 1);
        CHECK(added.size_sum == 16);

        Vector<std::uint8_t> bytes;
        p.serialize_into(bytes);
        const AggPartial back = round_trip(p);
        CHECK(back.bin_interval == p.bin_interval);
        REQUIRE(back.bins.size() == p.bins.size());
        for (std::size_t i = 0; i < p.bins.size(); ++i) {
            CHECK(back.bins[i].pid == p.bins[i].pid);
            CHECK(back.bins[i].tid == p.bins[i].tid);
            CHECK(back.bins[i].bin == p.bins[i].bin);
            CHECK(back.bins[i].count == p.bins[i].count);
            CHECK(back.bins[i].duration_n == p.bins[i].duration_n);
            CHECK(back.bins[i].duration_sum == p.bins[i].duration_sum);
            CHECK(back.bins[i].size_n == p.bins[i].size_n);
            CHECK(back.bins[i].size_sum == p.bins[i].size_sum);
        }

        AggPartial::Fields no_bins;
        no_bins.bins = false;
        auto skipped = AggPartial::deserialize(Span<const std::uint8_t>(bytes),
                                               p.accuracy, no_bins);
        REQUIRE(skipped.has_value());
        CHECK(skipped->bins.empty());
        CHECK(skipped->bin_interval == 0);
        CHECK(skipped->count == p.count);

        for (std::size_t len = 0; len < bytes.size(); ++len) {
            INFO("prefix length: ", len);
            CHECK_FALSE(AggPartial::deserialize(
                            Span<const std::uint8_t>(bytes.data(), len))
                            .has_value());
        }

        AggPartial merged = round_trip(p);
        merged.merge(back);
        CHECK(merged.count == 2 * p.count);
        CHECK(merged.bins.empty());
        CHECK(merged.bin_interval == 0);
    }

    TEST_CASE("the exact sum keeps its carry through merge and the wire") {
        AggPartial a, b;
        a.size.add(UINT64_MAX);
        b.size.add(std::uint64_t{5});
        a.merge(b);
        REQUIRE(a.size.ecarry == 1);
        const AggPartial back = round_trip(a);
        CHECK(back.size.ecarry == 1);
        CHECK(back.size.esum == a.size.esum);
        CHECK(back.size.domain == FieldStatDomain::U64);
        CHECK(back.size.emax == a.size.emax);
        CHECK(back.size.n == 2);
    }

    TEST_CASE("distinct ids are exact up to 256 and an estimate past it") {
        const auto cfg = make_config(false);
        auto tid_events = [&](std::uint64_t lo, std::uint64_t hi) {
            std::vector<Ev> out;
            for (std::uint64_t t = lo; t < hi; ++t) {
                Ev e;
                e.cat = "app";
                e.name = "x";
                e.pid = 1;
                e.tid = t;
                e.ts = 100 + t;
                e.dur = 5;
                out.push_back(std::move(e));
            }
            return out;
        };
        auto build = [&](std::uint64_t lo, std::uint64_t hi) {
            AggPartial p;
            for (const Ev& e : tid_events(lo, hi))
                REQUIRE(update_partial(p, Acc(e), cfg));
            return p;
        };

        AggPartial exact = build(0, 256);
        CHECK(exact.tids.is_exact());
        CHECK(exact.tids.count() == 256);
        CHECK_FALSE(exact.has_estimate());

        AggPartial over = build(0, 600);
        CHECK_FALSE(over.tids.is_exact());
        CHECK(over.has_estimate());
        CHECK(over.pids.is_exact());
        CHECK(std::abs(static_cast<double>(over.tids.count()) - 600.0) <=
              0.03 * 600.0);
        AggPartial over_back = round_trip(over);
        CHECK_FALSE(over_back.tids.is_exact());
        CHECK(over_back.tids.count() == over.tids.count());

        // Two lists whose union fits stay a list. One that does not fit does
        // not.
        AggPartial a = build(0, 100);
        a.merge(build(50, 150));
        CHECK(a.tids.is_exact());
        CHECK(a.tids.count() == 150);
        AggPartial b = build(0, 200);
        b.merge(build(100, 400));
        CHECK_FALSE(b.tids.is_exact());
        CHECK(std::abs(static_cast<double>(b.tids.count()) - 400.0) <=
              0.03 * 400.0);
        // A sketch merged with a list and a list merged with a sketch.
        AggPartial c = build(0, 600);
        c.merge(build(590, 650));
        CHECK(std::abs(static_cast<double>(c.tids.count()) - 650.0) <=
              0.03 * 650.0);
        AggPartial d = build(590, 650);
        d.merge(build(0, 600));
        CHECK(std::abs(static_cast<double>(d.tids.count()) - 650.0) <=
              0.03 * 650.0);
        CHECK(d.pids.count() == 1);
    }

    TEST_CASE(
        "distinct (pid, tid) and (pid, file) pairs merge and round trip") {
        AggPartial a;
        // Thread 7 of two processes is two threads. File 5 of pid 1 seen
        // twice is one access. An event without a file adds no pair.
        a.observe(1, 7, 5);
        a.observe(1, 7, 5);
        a.observe(2, 7, 5);
        a.observe(2, 8, 0);
        CHECK(a.tids.count() == 2);
        CHECK(a.pid_tids.count() == 3);
        CHECK(a.files.count() == 1);
        CHECK(a.pid_files.count() == 2);

        AggPartial b;
        b.observe(1, 7, 6);
        b.observe(2, 8, 5);
        a.merge(b);
        CHECK(a.pid_tids.count() == 3);
        CHECK(a.pid_files.count() == 3);
        CHECK_FALSE(a.has_estimate());

        AggPartial back = round_trip(a);
        CHECK(back.pid_tids.count() == 3);
        CHECK(back.pid_files.count() == 3);

        // Past 256 pairs the set is an estimate, though pids and files are not.
        AggPartial many;
        for (std::uint64_t pid = 1; pid <= 300; ++pid) many.observe(pid, 1, 9);
        CHECK(many.pids.is_exact() == false);
        CHECK(many.files.is_exact());
        CHECK_FALSE(many.pid_files.is_exact());
        CHECK(many.has_estimate());
        AggPartial many_back = round_trip(many);
        CHECK(many_back.pid_files.count() == many.pid_files.count());
    }

    TEST_CASE("building a partial allocates a bounded number of blocks") {
        // Debug mode counts allocations. ctest sets
        // DFTRACER_UTILS_MEMORY_DEBUG=1.
        const auto cfg = make_config(true);
        auto events = [&](std::size_t n) {
            std::vector<Ev> out;
            std::uint64_t s = 99;
            for (std::size_t i = 0; i < n; ++i) {
                Ev e;
                e.cat = "POSIX";
                e.name = "read";
                e.pid = 7;
                e.tid = 70 + next(s) % 4;
                e.ts = 5000 + i;
                e.dur = 1 + next(s) % 10000;
                e.fhash = "1a2b3c4d";
                e.args = {{"ret", 1 + next(s) % (1 << 20)},
                          {"offset", next(s) % (1 << 20)},
                          {"level", next(s) % 4}};
                out.push_back(std::move(e));
            }
            return out;
        };
        const auto small = events(1000);
        const auto large = events(40000);
        {
            AggPartial warm;
            update_partial(warm, Acc(small[0]), cfg);
        }
        REQUIRE(dftracer::utils::memory::debug_allocation_count() > 0);

        auto count_for = [&](const std::vector<Ev>& evs) {
            const auto before =
                dftracer::utils::memory::debug_allocation_count();
            AggPartial p;
            for (const Ev& e : evs) update_partial(p, Acc(e), cfg);
            REQUIRE(p.count == evs.size());
            return dftracer::utils::memory::debug_allocation_count() - before;
        };
        const auto n_small = count_for(small);
        const auto n_large = count_for(large);
        std::fprintf(stderr,
                     "allocations: 1000 events %llu, 40000 events %llu\n",
                     static_cast<unsigned long long>(n_small),
                     static_cast<unsigned long long>(n_large));
        // Per partial: 4 quantile stores (duration, size, offset, level) each
        // growing 1..64 (7 blocks) then a sketch and its bins. Then the tid
        // list (3), the pid, file and span vectors (1 each) and the arg vector
        // (3).
        constexpr std::uint64_t BOUND = 60;
        CHECK(n_small <= BOUND);
        CHECK(n_large <= BOUND);
        CHECK(n_large <= n_small + 16);
    }

    TEST_CASE("sizes and a typical serialized partial") {
        const auto cfg = make_config(false);
        AggPartial p;
        std::uint64_t s = 5;
        for (std::size_t i = 0; i < 100; ++i) {
            Ev e;
            e.cat = "POSIX";
            e.name = "read";
            e.pid = 4242;
            e.tid = 4300 + i % 2;
            e.ts = 1700000000000000ULL + i * 40;
            e.dur = 20 + next(s) % 400;
            e.fhash = "1a2b3c4d5e6f7081";
            e.args = {{"ret", 4096}, {"offset", i * 4096}};
            REQUIRE(update_partial(p, Acc(e), cfg));
        }
        Vector<std::uint8_t> bytes;
        p.serialize_into(bytes);
        std::fprintf(stderr,
                     "sizeof AggPartial %zu FieldStat %zu QuantileStore %zu "
                     "DistinctIds %zu PidSpan %zu\n"
                     "typical partial (100 events, 1 pid, 2 tids, 1 file): "
                     "%zu bytes\n",
                     sizeof(AggPartial), sizeof(FieldStat),
                     sizeof(QuantileStore), sizeof(DistinctIds),
                     sizeof(PidSpan), bytes.size());
        CHECK(bytes.size() < 400);
        check_same(p, round_trip(p), 0.0);
    }
}
