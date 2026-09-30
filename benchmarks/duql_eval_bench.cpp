// Benchmark: duql filter evaluation per record, on JSON records and on the
// field maps the indexed reader builds, for literal-leaf and expression
// filters. A filter the build cannot parse prints n/a.
//
// Built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   duql_eval_bench [records] [rounds]

#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/json/json_value.h>
#include <simdjson.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace duql = dftracer::utils::duql;
using dftracer::utils::json::JsonValue;

namespace {

constexpr const char* NAMES[] = {
    "read",          "write",           "open64",   "MPI_Send",
    "MPI_Allreduce", "cudaMemcpy",      "pread64",  "fwrite",
    "MPI_Recv",      "cudaMemcpyAsync", "H5Dwrite", "lseek64"};
constexpr const char* CATS[] = {"POSIX", "STDIO", "MPI"};

std::string record(std::uint32_t i) {
    std::string r = R"({"name":")" + std::string(NAMES[i % 12]) +
                    R"(","cat":")" + CATS[i % 3] + R"(","pid":)" +
                    std::to_string(i % 7) + R"(,"dur":)" +
                    std::to_string((i * 37) % 200) + R"(,"args":{"size":)" +
                    std::to_string((i * 131) % 4096);
    if (i % 5 == 0) r += R"(,"retry":)" + std::to_string(i % 3);
    return r + "}}";
}

// The fields of `q` from `v`, as the indexed reader stores them.
duql::ValueMap map_of(const duql::Query& q, JsonValue v) {
    duql::ValueMap m;
    for (const auto& f : q.fields()) {
        JsonValue x = v.at(std::string(f));
        if (!x.exists() && !std::string_view(f).starts_with("args."))
            x = v.at("args." + std::string(f));
        if (!x.exists()) continue;
        if (x.is_string())
            m[std::string(f)] = std::string(x.get<std::string_view>());
        else if (x.is_int())
            m[std::string(f)] = x.get<std::int64_t>();
        else if (x.is_uint())
            m[std::string(f)] = x.get<std::uint64_t>();
        else if (x.is_number())
            m[std::string(f)] = x.get<double>();
    }
    return m;
}

template <class F>
double ns_per_record(std::size_t n, int rounds, F&& body) {
    std::vector<double> times;
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
std::string long_path(std::uint32_t i, std::size_t len) {
    std::string s = "/p/lustre/scratch/run" + std::to_string(i % 97);
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
    const std::size_t n =
        argc > 1 ? static_cast<std::size_t>(std::atoll(argv[1])) : 200000;
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 7;

    simdjson::dom::parser parser;
    std::vector<std::string> texts;
    texts.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        texts.push_back(record(static_cast<std::uint32_t>(i)));
    std::vector<simdjson::padded_string> padded;
    padded.reserve(n);
    for (const auto& t : texts) padded.emplace_back(t);
    std::vector<simdjson::dom::document> docs(n);
    std::vector<simdjson::dom::element> roots(n);
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
    std::printf("%-44s %10s %10s %8s\n", "filter", "json ns", "map ns", "kept");
    for (const char* text : filters) {
        const auto q = duql::try_parse(text);
        if (!q) {
            std::printf("%-44s %10s %10s %8s\n", text, "n/a", "n/a", "-");
            continue;
        }
        std::vector<duql::ValueMap> maps;
        maps.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            maps.push_back(map_of(*q, JsonValue(roots[i])));
        std::size_t kept = 0;
        const double json_ns = ns_per_record(n, rounds, [&] {
            kept = 0;
            for (std::size_t i = 0; i < n; ++i)
                kept += q->evaluate(JsonValue(roots[i])) ? 1 : 0;
        });
        std::size_t kept_map = 0;
        const double map_ns = ns_per_record(n, rounds, [&] {
            kept_map = 0;
            for (std::size_t i = 0; i < n; ++i)
                kept_map += q->evaluate(maps[i]) ? 1 : 0;
        });
        std::printf("%-44s %10.1f %10.1f %8zu%s\n", text, json_ns, map_ns, kept,
                    kept == kept_map ? "" : " (map differs)");
    }
    namespace df = dftracer::utils::dataframe;
    std::vector<std::string_view> names;
    names.reserve(n);
    for (std::size_t i = 0; i < n; ++i) names.push_back(NAMES[i % 12]);
    const df::Series col = df::Series::strings(names);
    struct Kernel {
        const char* label;
        df::Series (df::Series::*fn)(std::string_view) const;
        const char* pattern;
    };
    const Kernel kernels[] = {
        {"str_starts_with MPI_ (control)", &df::Series::str_starts_with,
         "MPI_"},
        {"str_contains Memcpy (control)", &df::Series::str_contains, "Memcpy"},
        {"str_like MPI_%", &df::Series::str_like, "MPI_%"},
        {"str_like %p%e%d%", &df::Series::str_like, "%p%e%d%"},
        {"str_like _write", &df::Series::str_like, "_write"},
        {"str_search (read|write)64", &df::Series::str_search,
         "(read|write)64"},
        {"str_search cuda.*Async", &df::Series::str_search, "cuda.*Async"},
        {"str_matches MPI_.*", &df::Series::str_matches, "MPI_.*"},
    };
    std::printf("\n%-44s %10s\n", "DataFrame kernel", "ns/row");
    for (const Kernel& k : kernels) {
        const double ns = ns_per_record(n, rounds, [&] {
            const df::Series m = (col.*k.fn)(k.pattern);
            if (!m.handle()) std::abort();
        });
        std::printf("%-44s %10.1f\n", k.label, ns);
    }

    constexpr const char* VARIED[] = {"MPI_", "cuda", "read",   "wri",
                                      "H5",   "64",   "Memcpy", "zzz"};
    std::vector<std::string_view> same_needles, varied_needles;
    same_needles.reserve(n);
    varied_needles.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        same_needles.push_back("");
        varied_needles.push_back(VARIED[i % 8]);
    }
    std::printf("\n%-44s %10s\n", "DataFrame expr string predicate", "ns/row");
    for (const auto& [label, op, lit] :
         {std::tuple{"starts_with", df::StrPredOp::StartsWith, "MPI_"},
          std::tuple{"contains", df::StrPredOp::Contains, "Memcpy"}}) {
        const std::vector<const df::Series*> in0{&col};
        const df::Expr lit_e = df::expr_str_pred(op, df::expr_col(0), lit);
        const double lit_ns = ns_per_record(n, rounds, [&] {
            if (!df::eval(lit_e, in0).handle()) std::abort();
        });
        std::printf("%-12s %-31s %10.1f\n", label, "literal needle", lit_ns);
        std::fill(same_needles.begin(), same_needles.end(), lit);
        const df::Series same = df::Series::strings(same_needles);
        const df::Series varied = df::Series::strings(varied_needles);
        const df::Expr col_e =
            df::expr_str_pred_col(op, df::expr_col(0), df::expr_col(1));
        const std::vector<const df::Series*> in_same{&col, &same};
        const std::vector<const df::Series*> in_varied{&col, &varied};
        const double same_ns = ns_per_record(n, rounds, [&] {
            if (!df::eval(col_e, in_same).handle()) std::abort();
        });
        std::printf("%-12s %-31s %10.1f\n", label, "column needle, same",
                    same_ns);
        const double var_ns = ns_per_record(n, rounds, [&] {
            if (!df::eval(col_e, in_varied).handle()) std::abort();
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
        std::vector<duql::ValueMap> maps(m);
        for (std::size_t i = 0; i < m; ++i)
            maps[i]["fname"] = long_path(static_cast<std::uint32_t>(i), len);
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
        std::vector<std::string_view> views;
        views.reserve(m);
        std::vector<std::string> own;
        own.reserve(m);
        for (std::size_t i = 0; i < m; ++i)
            own.push_back(long_path(static_cast<std::uint32_t>(i), len));
        for (const auto& v : own) views.push_back(v);
        const df::Series longs = df::Series::strings(views);
        for (const auto& [label, fn, pattern] :
             {std::tuple{"str_search /scratch/.*\\.h5$",
                         &df::Series::str_search, "/scratch/.*\\.h5$"},
              std::tuple{"str_matches .*/scratch/.*", &df::Series::str_matches,
                         ".*/scratch/.*"}}) {
            const double ns = ns_per_record(m, rounds, [&] {
                const df::Series r = (longs.*fn)(pattern);
                if (!r.handle()) std::abort();
            });
            std::printf("%-34s %5zu B %10.1f\n", label, len, ns);
        }
    }
    return 0;
}
