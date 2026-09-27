// Benchmark: DataFrame masks of duql expression leaves over one frame, the
// median of several rounds per query.
//
// Built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   duql_mask_bench [rows] [rounds]

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/mask.h>
#include <dftracer/utils/duql/query.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace df = dftracer::utils::dataframe;
namespace duql = dftracer::utils::duql;

namespace {

df::DataFrame frame(std::int64_t rows) {
    constexpr std::string_view NAMES[] = {"read",     "write",  "open64",
                                          "MPI_Send", "fwrite", "lseek64"};
    std::vector<std::int64_t> dur, size, retry;
    std::vector<std::uint8_t> retry_valid((rows + 7) / 8, 0);
    std::vector<std::string_view> name;
    for (std::int64_t i = 0; i < rows; ++i) {
        dur.push_back((i * 37) % 200);
        size.push_back((i * 131) % 4096);
        retry.push_back(i % 3);
        if (i % 5 == 0)
            retry_valid[static_cast<std::size_t>(i / 8)] |=
                static_cast<std::uint8_t>(1U << (i % 8));
        name.push_back(NAMES[i % 6]);
    }
    df::DataFrame f;
    f.names = {"dur", "size", "retry", "name"};
    f.columns.push_back(df::Series::flat_i64(dur.data(), rows));
    f.columns.push_back(df::Series::flat_i64(size.data(), rows));
    f.columns.push_back(
        df::Series::flat_i64(retry.data(), rows, retry_valid.data()));
    f.columns.push_back(df::Series::strings(name, nullptr));
    return f;
}

}  // namespace

int main(int argc, char** argv) {
    const std::int64_t rows = argc > 1 ? std::atoll(argv[1]) : 1'000'000;
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 15;
    const df::DataFrame f = frame(rows);
    const char* const QUERIES[] = {
        "dur * 2 + size > 300",     "dur % 7 == 0 and size > 100",
        "abs(dur - 100) < 20",      "coalesce(retry, 0) > 0",
        R"(lower(name) == "read")", "len(name) > 4",
    };
    std::printf("%-32s %10s\n", "query", "ms");
    for (const char* q : QUERIES) {
        auto parsed = duql::Query::from_string(q);
        if (!parsed) {
            std::printf("%-32s %10s\n", q, "n/a");
            continue;
        }
        std::vector<double> t;
        std::int64_t kept = 0;
        for (int r = 0; r < rounds; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            const df::Series m = df::evaluate_mask(parsed->root(), f);
            t.push_back(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count());
            kept = m.length() - m.null_count();
        }
        std::sort(t.begin(), t.end());
        std::printf("%-32s %10.2f  (%lld known)\n", q, t[t.size() / 2],
                    static_cast<long long>(kept));
    }
    return 0;
}
