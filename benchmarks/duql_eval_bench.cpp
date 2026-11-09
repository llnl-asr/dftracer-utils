// SPDX-License-Identifier: MIT
// Benchmark: duql filter evaluation per record, on JSON records and on the
// field maps the indexed reader builds, for literal-leaf and expression
// filters. A filter the build cannot parse prints n/a.
//
// Built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   duql_eval_bench [records] [rounds]

#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/tuple.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/json/value.h>
#include <simdjson.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace dftracer::utils;

using dftracer::utils::json::JsonValue;

namespace {

constexpr const char* NAMES[] = {
    "read",          "write",           "open64",   "MPI_Send",
    "MPI_Allreduce", "cudaMemcpy",      "pread64",  "fwrite",
    "MPI_Recv",      "cudaMemcpyAsync", "H5Dwrite", "lseek64"};
constexpr const char* CATS[] = {"POSIX", "STDIO", "MPI"};

dftracer::utils::dstd::String record(std::uint32_t i) {
    dftracer::utils::dstd::String r =
        R"({"name":")" + dftracer::utils::dstd::String(NAMES[i % 12]) +
        R"(","cat":")" + CATS[i % 3] + R"(","pid":)" +
        dftracer::utils::dstd::to_string(i % 7) + R"(,"dur":)" +
        dftracer::utils::dstd::to_string((i * 37) % 200) +
        R"(,"args":{"size":)" +
        dftracer::utils::dstd::to_string((i * 131) % 4096);
    if (i % 5 == 0)
        r += R"(,"retry":)" + dftracer::utils::dstd::to_string(i % 3);
    return r + "}}";
}

// The fields of `q` from `v`, as the indexed reader stores them.
duql::ValueMap map_of(const duql::Query& q, JsonValue v) {
    duql::ValueMap m;
    for (const auto& f : q.fields()) {
        JsonValue x = v.at(dftracer::utils::dstd::String(f));
        if (!x.exists() &&
            !dftracer::utils::dstd::StringView(f).starts_with("args."))
            x = v.at("args." + dftracer::utils::dstd::String(f));
        if (!x.exists()) continue;
        if (x.is_string())
            m[dftracer::utils::dstd::String(f)] = dftracer::utils::dstd::String(
                x.get<dftracer::utils::dstd::StringView>());
        else if (x.is_int())
            m[dftracer::utils::dstd::String(f)] = x.get<std::int64_t>();
        else if (x.is_uint())
            m[dftracer::utils::dstd::String(f)] = x.get<std::uint64_t>();
        else if (x.is_number())
            m[dftracer::utils::dstd::String(f)] = x.get<double>();
    }
    return m;
}

template <class F>
double ns_per_record(std::size_t n, int rounds, F&& body) {
    dftracer::utils::dstd::Vector<double> times;
    for (int r = 0; r < rounds; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        body();
        const auto t1 = std::chrono::steady_clock::now();
        times.push_back(
            std::chrono::duration<double, std::nano>(t1 - t0).count() /
            static_cast<double>(n));
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

// A path of `len` bytes holding "/scratch/"; every fifth ends in ".h5".
dftracer::utils::dstd::String long_path(std::uint32_t i, std::size_t len) {
    dftracer::utils::dstd::String s =
        "/p/lustre/scratch/run" + dftracer::utils::dstd::to_string(i % 97);
    std::uint32_t x = i * 2654435761u + 1;
    while (s.size() + 3 < len) {
        s += '/';
        for (int k = 0; k < 7 && s.size() + 3 < len; ++k) {
            x = x * 1664525u + 1013904223u;
            s += static_cast<char>('a' + (x >> 24) % 26);
        }
    }
    s.resize(len - 3);
    return s + (i % 5 == 0 ? ".h5" : ".dt");
}

}  // namespace

int main(int argc, char** argv) {
    const bool smoke =
        argc == 2 && dftracer::utils::dstd::StringView(argv[1]) == "--smoke";
    const std::size_t n =
        smoke ? 2000
              : (argc > 1 ? static_cast<std::size_t>(std::atoll(argv[1]))
                          : 200000);
    const int rounds = smoke ? 1 : (argc > 2 ? std::atoi(argv[2]) : 7);

    simdjson::dom::parser parser;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> texts;
    texts.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        texts.push_back(record(static_cast<std::uint32_t>(i)));
    dftracer::utils::dstd::Vector<simdjson::padded_string> padded;
    padded.reserve(n);
    for (const auto& t : texts) padded.emplace_back(t);
    dftracer::utils::dstd::Vector<simdjson::dom::document> docs(n);
    dftracer::utils::dstd::Vector<simdjson::dom::element> roots(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (parser.parse_into_document(docs[i], padded[i]).get(roots[i]) !=
            simdjson::SUCCESS) {
            std::fprintf(stderr, "parse failed\n");
            return 1;
        }
    }

    const char* filters[] = {
        R"(cat == "POSIX" and dur > 50)",
        R"(name in ["read", "write"] or pid == 3)",
        "not (args.size > 1000)",
        "retry == 1",
        "args.size * 2 > 2000",
        R"(lower(name) == "read" and dur // 10 == 5)",
        "exists(args.retry)",
        "(retry ?? 0) >= 1",
        R"(name like "MPI_%")",
        R"(name like "%Memcpy%")",
        R"(name like "%p%e%d%")",
        R"(name like "_write")",
        R"(name ilike "%READ%")",
        R"(name ~ "^MPI_.*e$")",
        R"q(name ~ "(read|write)64")q",
        R"(name ~* "memcpy")",
    };
    std::printf("%-44s %9s %9s %9s %9s %9s %7s\n", "filter", "dom ns", "od ns",
                "match ns", "cb ns", "map ns", "kept");
    simdjson::dom::parser dom_parser;
    simdjson::ondemand::parser od_parser;
    dftracer::utils::dstd::StringArena strings;
    json::Document document;
    for (const char* text : filters) {
        const auto q = duql::try_parse(text);
        if (!q) {
            std::printf("%-44s %9s %9s %9s %9s %9s %7s\n", text, "n/a", "n/a",
                        "n/a", "n/a", "n/a", "-");
            continue;
        }
        const auto& root = duql::detail::QueryAccess::root(*q);
        dftracer::utils::dstd::Vector<duql::ValueMap> maps;
        maps.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            maps.push_back(map_of(*q, JsonValue(roots[i])));
        std::size_t kept = 0;
        const double dom_ns = ns_per_record(n, rounds, [&] {
            kept = 0;
            for (std::size_t i = 0; i < n; ++i) {
                simdjson::dom::element el;
                if (dom_parser.parse(padded[i]).get(el) != simdjson::SUCCESS)
                    std::abort();
                kept += duql::evaluate(root, JsonValue(el)) ? 1 : 0;
            }
        });
        std::size_t kept_od = 0;
        const double od_ns = ns_per_record(n, rounds, [&] {
            kept_od = 0;
            for (std::size_t i = 0; i < n; ++i) {
                simdjson::ondemand::document doc;
                if (od_parser.iterate(padded[i]).get(doc) != simdjson::SUCCESS)
                    std::abort();
                strings.clear();
                kept_od +=
                    duql::evaluate(root,
                                   duql::OnDemandEvent(
                                       doc,
                                       dftracer::utils::dstd::StringView(
                                           padded[i].data(), padded[i].size()),
                                       strings))
                        ? 1
                        : 0;
            }
        });
        std::size_t kept_match = 0;
        const double match_ns = ns_per_record(n, rounds, [&] {
            kept_match = 0;
            for (std::size_t i = 0; i < n; ++i) {
                auto event = document.parse(texts[i]);
                if (!event) std::abort();
                kept_match += q->matches(*event) ? 1 : 0;
            }
        });
        double dur_sum = 0;
        const double cb_ns = ns_per_record(n, rounds, [&] {
            dur_sum = 0;
            for (std::size_t i = 0; i < n; ++i) {
                auto event = document.parse(texts[i]);
                if (!event) std::abort();
                if (!q->matches(*event)) continue;
                auto dur = event->find("dur");
                if (dur) dur_sum += dur->as_double().value_or(0);
            }
        });
        std::size_t kept_map = 0;
        const double map_ns = ns_per_record(n, rounds, [&] {
            kept_map = 0;
            for (std::size_t i = 0; i < n; ++i)
                kept_map += q->evaluate(maps[i]) ? 1 : 0;
        });
        std::printf("%-44s %9.1f %9.1f %9.1f %9.1f %9.1f %7zu%s%s%s\n", text,
                    dom_ns, od_ns, match_ns, cb_ns, map_ns, kept,
                    kept == kept_map ? "" : " (map differs)",
                    kept == kept_od ? "" : " (od differs)",
                    kept == kept_match ? "" : " (matches differs)");
        if (dur_sum < 0) std::abort();
    }
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::StringView> names;
    names.reserve(n);
    for (std::size_t i = 0; i < n; ++i) names.push_back(NAMES[i % 12]);
    const dataframe::Series col = dataframe::Series::strings(names);
    struct Kernel {
        const char* label;
        dataframe::Series (dataframe::Series::*fn)(
            dftracer::utils::dstd::StringView) const;
        const char* pattern;
    };
    const Kernel kernels[] = {
        {"str_starts_with MPI_ (control)", &dataframe::Series::str_starts_with,
         "MPI_"},
        {"str_contains Memcpy (control)", &dataframe::Series::str_contains,
         "Memcpy"},
        {"str_like MPI_%", &dataframe::Series::str_like, "MPI_%"},
        {"str_like %p%e%d%", &dataframe::Series::str_like, "%p%e%d%"},
        {"str_like _write", &dataframe::Series::str_like, "_write"},
        {"str_search (read|write)64", &dataframe::Series::str_search,
         "(read|write)64"},
        {"str_search cuda.*Async", &dataframe::Series::str_search,
         "cuda.*Async"},
        {"str_matches MPI_.*", &dataframe::Series::str_matches, "MPI_.*"},
    };
    std::printf("\n%-44s %10s\n", "DataFrame kernel", "ns/row");
    for (const Kernel& k : kernels) {
        const double ns = ns_per_record(n, rounds, [&] {
            const dataframe::Series m = (col.*k.fn)(k.pattern);
            if (!m.handle()) std::abort();
        });
        std::printf("%-44s %10.1f\n", k.label, ns);
    }

    constexpr const char* VARIED[] = {"MPI_", "cuda", "read",   "wri",
                                      "H5",   "64",   "Memcpy", "zzz"};
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::StringView>
        same_needles, varied_needles;
    same_needles.reserve(n);
    varied_needles.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        same_needles.push_back("");
        varied_needles.push_back(VARIED[i % 8]);
    }
    std::printf("\n%-44s %10s\n", "DataFrame expr string predicate", "ns/row");
    for (const auto& [label, op, lit] :
         {dftracer::utils::dstd::make_tuple(
              "starts_with", dataframe::StrPredOp::StartsWith, "MPI_"),
          dftracer::utils::dstd::make_tuple(
              "contains", dataframe::StrPredOp::Contains, "Memcpy")}) {
        const dftracer::utils::dstd::Vector<const dataframe::Series*> in0{&col};
        const dataframe::Expr lit_e =
            dataframe::expr_str_pred(op, dataframe::expr_col(0), lit);
        const double lit_ns = ns_per_record(n, rounds, [&] {
            auto out = dataframe::eval(lit_e, in0);
            if (!out || !out->handle()) std::abort();
        });
        std::printf("%-12s %-31s %10.1f\n", label, "literal needle", lit_ns);
        std::fill(same_needles.begin(), same_needles.end(), lit);
        const dataframe::Series same = dataframe::Series::strings(same_needles);
        const dataframe::Series varied =
            dataframe::Series::strings(varied_needles);
        auto col_pred = dataframe::expr_str_pred_col(op, dataframe::expr_col(0),
                                                     dataframe::expr_col(1));
        if (!col_pred) std::abort();
        const dataframe::Expr col_e = *col_pred;
        const dftracer::utils::dstd::Vector<const dataframe::Series*> in_same{
            &col, &same};
        const dftracer::utils::dstd::Vector<const dataframe::Series*> in_varied{
            &col, &varied};
        const double same_ns = ns_per_record(n, rounds, [&] {
            auto out = dataframe::eval(col_e, in_same);
            if (!out || !out->handle()) std::abort();
        });
        std::printf("%-12s %-31s %10.1f\n", label, "column needle, same",
                    same_ns);
        const double var_ns = ns_per_record(n, rounds, [&] {
            auto out = dataframe::eval(col_e, in_varied);
            if (!out || !out->handle()) std::abort();
        });
        std::printf("%-12s %-31s %10.1f\n", label, "column needle, varied",
                    var_ns);
    }

    constexpr std::size_t LONG_LENGTHS[] = {256, 1024, 4096};
    const char* long_filters[] = {
        R"q(fname ~ "/scratch/.*\\.h5$")q",
        R"q(fname ~ "(read|write)[0-9]+\\.h5$")q",
    };
    std::printf("\n%-44s %10s\n", "long value filter (map path)", "ns/record");
    for (const std::size_t len : LONG_LENGTHS) {
        const std::size_t m = std::min<std::size_t>(n, (64u << 20) / len);
        dftracer::utils::dstd::Vector<duql::ValueMap> maps(m);
        for (std::size_t i = 0; i < m; ++i)
            maps[i]["fname"] = dftracer::utils::dstd::String(
                long_path(static_cast<std::uint32_t>(i), len));
        for (const char* text : long_filters) {
            const auto q = duql::try_parse(text);
            if (!q) continue;
            std::size_t kept = 0;
            const double ns = ns_per_record(m, rounds, [&] {
                kept = 0;
                for (std::size_t i = 0; i < m; ++i)
                    kept += q->evaluate(maps[i]) ? 1 : 0;
            });
            std::printf("%-34s %5zu B %10.1f  kept %zu\n", text, len, ns, kept);
        }
        dftracer::utils::dstd::Vector<dftracer::utils::dstd::StringView> views;
        views.reserve(m);
        dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> own;
        own.reserve(m);
        for (std::size_t i = 0; i < m; ++i)
            own.push_back(long_path(static_cast<std::uint32_t>(i), len));
        for (const auto& v : own) views.push_back(v);
        const dataframe::Series longs = dataframe::Series::strings(views);
        for (const auto& [label, fn, pattern] :
             {dftracer::utils::dstd::make_tuple("str_search /scratch/.*\\.h5$",
                                                &dataframe::Series::str_search,
                                                "/scratch/.*\\.h5$"),
              dftracer::utils::dstd::make_tuple("str_matches .*/scratch/.*",
                                                &dataframe::Series::str_matches,
                                                ".*/scratch/.*")}) {
            const double ns = ns_per_record(m, rounds, [&] {
                const dataframe::Series r = (longs.*fn)(pattern);
                if (!r.handle()) std::abort();
            });
            std::printf("%-34s %5zu B %10.1f\n", label, len, ns);
        }
    }
    return 0;
}
