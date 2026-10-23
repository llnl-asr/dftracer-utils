#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial_update.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_core.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/memory/heap.h>
#include <doctest/doctest.h>

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

#include "aggregation_oracle.h"

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

struct ArgIds {
    std::map<std::string, std::uint32_t, std::less<>> ids;
    std::uint32_t operator()(std::string_view k) {
        auto it = ids.find(k);
        if (it == ids.end()) {
            it = ids.emplace(std::string(k),
                             static_cast<std::uint32_t>(ids.size() * 3 + 1))
                     .first;
        }
        return it->second;
    }
};

using Key = std::pair<std::string, std::string>;

std::uint64_t next(std::uint64_t& s) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return s >> 17;
}

AggregationConfig make_config(bool percentiles) {
    AggregationConfig cfg;
    cfg.group_by_file = false;
    cfg.compute_percentiles = percentiles;
    cfg.custom_metric_fields = {"flops"};
    return cfg;
}

// POSIX read/write and STDIO fread/fwrite carry ret and offset; open carries a
// ret that is a descriptor and must not count as a size; compute carries
// numeric args only; "rare" has fewer than 64 events so its quantiles stay
// exact; every 17th POSIX transfer is a pre-aggregated counter.
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

// The per-event oracle on all events, one entry per (cat, name).
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
          std::size_t e, const AggregationConfig& cfg, ArgIds& ids) {
    for (std::size_t i = b; i < e; ++i) {
        auto it =
            into.try_emplace(key_of(events[i]), cfg.sketch_accuracy).first;
        REQUIRE(update_partial(it->second, Acc(events[i]), cfg, ids));
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

// tol 0 means bit-exact moments (a round trip); otherwise a relative bound on
// the moments (a different order of merges).
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
                             const Truth& t, const ArgIds& ids) {
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

    const std::size_t custom =
        ref.custom_metrics ? ref.custom_metrics->size() : 0;
    REQUIRE(p.args.size() == custom);
    if (!ref.custom_metrics) return;
    for (const auto& [name, stats] : *ref.custom_metrics) {
        const auto id = ids.ids.find(name);
        REQUIRE(id != ids.ids.end());
        const auto arg =
            std::find_if(p.args.begin(), p.args.end(),
                         [&](const ArgStat& a) { return a.id == id->second; });
        REQUIRE(arg != p.args.end());
        check_stat(arg->stat, stats.stat, 1e-9, name.c_str());
    }
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
    REQUIRE(a.args.size() == b.args.size());
    for (std::size_t i = 0; i < a.args.size(); ++i) {
        CHECK(a.args[i].id == b.args[i].id);
        check_stat(a.args[i].stat, b.args[i].stat, tol, "arg");
        CHECK(a.args[i].quantiles.count() == b.args[i].quantiles.count());
    }
}

// Chunk bounds as event indexes.
std::vector<std::size_t> bounds(std::size_t n, std::size_t chunks) {
    std::vector<std::size_t> out{0};
    for (std::size_t c = 1; c < chunks; ++c)
        out.push_back(n * (c * c) / (chunks * chunks) + c);
    out.push_back(n);
    return out;
}

Partials merged_chunks(const std::vector<Ev>& events,
                       const AggregationConfig& cfg, ArgIds& ids,
                       std::size_t chunks,
                       const std::vector<std::size_t>& order,
                       bool through_wire) {
    const auto cut = bounds(events.size(), chunks);
    std::vector<Partials> per_chunk(chunks);
    for (std::size_t c = 0; c < chunks; ++c)
        feed(per_chunk[c], events, cut[c], cut[c + 1], cfg, ids);
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
            ArgIds ids;
            std::vector<std::size_t> order;
            for (std::size_t c = 0; c < chunks; ++c) order.push_back(c);
            const auto merged =
                merged_chunks(events, cfg, ids, chunks, order, true);
            REQUIRE(merged.size() == ref.size());
            for (const auto& [k, p] : merged) {
                INFO("group: ", k.first, " ", k.second);
                const auto r = ref.find(k);
                REQUIRE(r != ref.end());
                check_against_reference(p, r->second, truth.at(k), ids);
            }
        }
    }

    TEST_CASE("one partial over the whole file equals the aggregator") {
        const auto cfg = make_config(true);
        const auto events = make_events(2500, 5);
        const auto ref = reference(events, cfg);
        const auto truth = truth_of(events, cfg);
        ArgIds ids;
        Partials whole;
        feed(whole, events, 0, events.size(), cfg, ids);
        for (const auto& [k, p] : whole) {
            INFO("group: ", k.first, " ", k.second);
            check_against_reference(p, ref.at(k), truth.at(k), ids);
        }
    }

    TEST_CASE("the order of the merges does not change the exact fields") {
        const auto cfg = make_config(true);
        const auto events = make_events(3000, 23);
        ArgIds ids;
        const auto forward =
            merged_chunks(events, cfg, ids, 5, {0, 1, 2, 3, 4}, false);
        const auto shuffled =
            merged_chunks(events, cfg, ids, 5, {3, 0, 4, 2, 1}, false);
        const auto reverse =
            merged_chunks(events, cfg, ids, 5, {4, 3, 2, 1, 0}, true);
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
        ArgIds ids;
        Partials whole;
        feed(whole, events, 0, events.size(), cfg, ids);
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
        CHECK(back->args.empty());
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
        ArgIds ids;
        Partials whole;
        feed(whole, events, 0, events.size(), cfg, ids);
        const AggPartial& p = whole.at({"posix", "read"});
        REQUIRE(p.size.n > 0);
        REQUIRE_FALSE(p.args.empty());
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
        ArgIds ids;
        auto build = [&](std::uint64_t lo, std::uint64_t hi) {
            AggPartial p;
            for (const Ev& e : tid_events(lo, hi))
                REQUIRE(update_partial(p, Acc(e), cfg, ids));
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

        // Two lists whose union fits stay a list; one that does not fits not.
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
        // Thread 7 of two processes is two threads; file 5 of pid 1 seen
        // twice is one access; an event without a file adds no pair.
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
        // Debug mode counts allocations; ctest sets
        // DFTRACER_UTILS_MEMORY_DEBUG=1.
        const auto cfg = make_config(true);
        ArgIds ids;
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
            update_partial(warm, Acc(small[0]), cfg, ids);
        }
        REQUIRE(dftracer::utils::memory::debug_allocation_count() > 0);

        auto count_for = [&](const std::vector<Ev>& evs) {
            const auto before =
                dftracer::utils::memory::debug_allocation_count();
            AggPartial p;
            for (const Ev& e : evs) update_partial(p, Acc(e), cfg, ids);
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
        // growing 1..64 (7 blocks) then a sketch and its bins; the tid list
        // (3), the pid, file and span vectors (1 each) and the arg vector (3).
        constexpr std::uint64_t BOUND = 60;
        CHECK(n_small <= BOUND);
        CHECK(n_large <= BOUND);
        CHECK(n_large <= n_small + 16);
    }

    TEST_CASE("sizes and a typical serialized partial") {
        const auto cfg = make_config(false);
        ArgIds ids;
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
            REQUIRE(update_partial(p, Acc(e), cfg, ids));
        }
        Vector<std::uint8_t> bytes;
        p.serialize_into(bytes);
        std::fprintf(stderr,
                     "sizeof AggPartial %zu FieldStat %zu QuantileStore %zu "
                     "DistinctIds %zu PidSpan %zu ArgStat %zu\n"
                     "typical partial (100 events, 1 pid, 2 tids, 1 file): "
                     "%zu bytes\n",
                     sizeof(AggPartial), sizeof(FieldStat),
                     sizeof(QuantileStore), sizeof(DistinctIds),
                     sizeof(PidSpan), sizeof(ArgStat), bytes.size());
        CHECK(bytes.size() < 400);
        check_same(p, round_trip(p), 0.0);
    }
}
