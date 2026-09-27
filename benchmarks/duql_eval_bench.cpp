// Benchmark: duql filter evaluation per record, on JSON records and on the
// field maps the indexed reader builds, for literal-leaf and expression
// filters. A filter the build cannot parse prints n/a.
//
// Built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   duql_eval_bench [records] [rounds]

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
#include <vector>

namespace duql = dftracer::utils::duql;
using dftracer::utils::json::JsonValue;

namespace {

constexpr const char* NAMES[] = {
    "read",          "write",         "open64",         "MPI_Send",
    "MPI_Allreduce", "cudaMemcpy",    "pread64",        "fwrite",
    "MPI_Recv",      "cudaMemcpyAsync", "H5Dwrite",     "lseek64"};
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
        std::printf("%-44s %10.1f %10.1f %8zu%s\n", text, json_ns, map_ns,
                    kept, kept == kept_map ? "" : " (map differs)");
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
    return 0;
}
