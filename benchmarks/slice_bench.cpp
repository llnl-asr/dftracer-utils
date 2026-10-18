// Benchmark: slice-heavy work on dataframe columns and the in-memory ops
// that build and drop many column buffers.
//
// The cases use only the public C++ API and the C ABI, so the same source
// builds against any version of the column buffer code. Inputs come from a
// fixed seed and are built before the clock starts.
//
// Cases (ns per op is wall time divided by operations, see each case):
//   slice int64              dftu_series_slice of 1000 rows at random offsets
//                            of a 10M-row Int64 column. One op is one slice
//                            and its free.
//   slice depth 4            a slice of a slice of a slice of a slice of the
//                            same column. One op is the four slices.
//   slice string             1000-row slices of a 1M-row String column.
//   slice nullable aligned   1000-row slices of a nullable 10M-row Int64
//                            column at offsets on a byte boundary.
//   slice nullable unaligned the same at offsets off a byte boundary, which
//                            repack the validity bits.
//   head tail                DataFrame head(1000) or tail(1000) of a 10M-row
//                            frame of Int64, Float64 and String columns.
//   filter half              Series filter of the 10M-row Int64 column with a
//                            mask that keeps every other row. One op is one
//                            filter.
//   concat 1000 slices       1000 slices of a two-column frame, each made a
//                            frame and concatenated by dftu_dataframe_concat.
//                            One op is the slices plus the concat.
//   arrow round trip         to_arrow and from_arrow of a 1M-row frame of
//                            Int64, nullable Float64 and String columns.
//   sort_by, group_by sum, unique, join inner
//                            the DataFrame ops on 1M rows. One op is one call.
//
// The default run is a driver. For every case it starts a binary again per
// side in alternating rounds, so each case has its own process and its own
// peak resident set. With --against OTHER the sides are OTHER then this
// binary, so two builds of this source compare A/B; without it there is one
// side. The table has the median ns per op and the median peak RSS.
//
//   slice_bench [--rounds N] [--label TEXT] [--against PATH]
//   slice_bench --smoke        (in this process, tiny sizes, no timing
//                               checks: every case ran and its result has
//                               the expected shape)
//   slice_bench --case NAME    (one case in this process; prints RESULT)

#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/dataframe/arrow.h>
#endif
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace df = dftracer::utils::dataframe;

namespace {

using Clock = std::chrono::steady_clock;
using df::DataFrame;
using df::Series;
using df::TypeId;

constexpr std::size_t MIB = std::size_t{1} << 20;
constexpr std::uint64_t SEED = 42;
constexpr int DEFAULT_ROUNDS = 3;
constexpr std::size_t OFFSET_TABLE = 1 << 16;

struct Scale {
    std::int64_t big_rows;
    std::int64_t string_rows;
    std::int64_t op_rows;
    std::int64_t slice_len;
    std::size_t slice_ops;
    std::size_t nested_ops;
    std::size_t string_ops;
    std::size_t head_ops;
    std::size_t filter_ops;
    std::size_t concat_parts;
    std::size_t concat_ops;
    std::size_t arrow_ops;
    std::size_t sort_reps;
    std::size_t group_reps;
    std::size_t unique_reps;
    std::size_t join_reps;
};

constexpr Scale SMOKE_SCALE{.big_rows = 20000,
                            .string_rows = 5000,
                            .op_rows = 5000,
                            .slice_len = 100,
                            .slice_ops = 200,
                            .nested_ops = 200,
                            .string_ops = 200,
                            .head_ops = 200,
                            .filter_ops = 2,
                            .concat_parts = 20,
                            .concat_ops = 2,
                            .arrow_ops = 20,
                            .sort_reps = 2,
                            .group_reps = 2,
                            .unique_reps = 2,
                            .join_reps = 2};
constexpr Scale FULL_SCALE{.big_rows = 10000000,
                           .string_rows = 1000000,
                           .op_rows = 1000000,
                           .slice_len = 1000,
                           .slice_ops = 16000000,
                           .nested_ops = 4000000,
                           .string_ops = 5000000,
                           .head_ops = 3000000,
                           .filter_ops = 500,
                           .concat_parts = 1000,
                           .concat_ops = 150,
                           .arrow_ops = 800000,
                           .sort_reps = 15,
                           .group_reps = 45,
                           .unique_reps = 35,
                           .join_reps = 50};

void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

double elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - start).count();
}

std::vector<std::int64_t> random_i64(std::size_t n, std::int64_t mod,
                                     std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::int64_t> out(n);
    for (auto& v : out) v = static_cast<std::int64_t>(rng() % mod);
    return out;
}

std::vector<double> random_f64(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> dist(0.0, 1000.0);
    std::vector<double> out(n);
    for (auto& v : out) v = dist(rng);
    return out;
}

Series int_column(std::int64_t rows) {
    std::vector<std::int64_t> values =
        random_i64(static_cast<std::size_t>(rows), 1 << 30, SEED);
    return Series::flat_i64(values.data(), rows);
}

// Every seventh row null.
std::vector<std::uint8_t> sparse_validity(std::int64_t rows) {
    std::vector<std::uint8_t> bits(static_cast<std::size_t>((rows + 7) / 8),
                                   0xFF);
    for (std::int64_t i = 0; i < rows; i += 7) {
        bits[static_cast<std::size_t>(i / 8)] &=
            static_cast<std::uint8_t>(~(1u << (i % 8)));
    }
    return bits;
}

Series string_column(std::int64_t rows) {
    std::mt19937_64 rng(SEED + 1);
    std::vector<std::string> values(static_cast<std::size_t>(rows));
    for (auto& v : values) {
        v = "row-" + std::to_string(rng() % 1000000) + "-" +
            std::to_string(rng() % 100);
    }
    return Series::strings(values);
}

std::vector<std::int64_t> offsets(std::int64_t rows, std::int64_t len,
                                  std::int64_t step, std::int64_t shift) {
    std::mt19937_64 rng(SEED + 2);
    std::int64_t slots = (rows - len - shift) / step;
    std::vector<std::int64_t> out(OFFSET_TABLE);
    for (auto& o : out) {
        o = static_cast<std::int64_t>(rng() %
                                      static_cast<std::uint64_t>(slots)) *
                step +
            shift;
    }
    return out;
}

double slice_loop(const Series& col, const std::vector<std::int64_t>& at,
                  std::int64_t len, std::size_t ops) {
    std::int64_t rows = 0;
    auto start = Clock::now();
    for (std::size_t i = 0; i < ops; ++i) {
        dftu_series* s =
            dftu_series_slice(col.handle(), at[i % OFFSET_TABLE], len);
        rows += dftu_series_length(s);
        dftu_series_free(s);
    }
    auto end = Clock::now();
    check(rows == len * static_cast<std::int64_t>(ops), "slice length");
    return elapsed_ns(start, end) / static_cast<double>(ops);
}

double slice_int64(const Scale& s) {
    Series col = int_column(s.big_rows);
    auto at = offsets(s.big_rows, s.slice_len, 1, 0);
    return slice_loop(col, at, s.slice_len, s.slice_ops);
}

double slice_depth4(const Scale& s) {
    Series col = int_column(s.big_rows);
    std::int64_t len1 = s.big_rows / 10;
    std::int64_t len2 = len1 / 10;
    std::int64_t len3 = len2 / 10;
    std::int64_t len4 = std::min(s.slice_len, len3);
    auto at = offsets(s.big_rows, len1, 1, 0);
    std::int64_t sum = 0;
    std::int64_t expect = 0;
    const auto* base = col.data<std::int64_t>();
    auto start = Clock::now();
    for (std::size_t i = 0; i < s.nested_ops; ++i) {
        std::int64_t o = at[i % OFFSET_TABLE];
        std::int64_t step = static_cast<std::int64_t>(i % 7);
        dftu_series* a = dftu_series_slice(col.handle(), o, len1);
        dftu_series* b = dftu_series_slice(a, step, len2);
        dftu_series* c = dftu_series_slice(b, step, len3);
        dftu_series* d = dftu_series_slice(c, step, len4);
        sum += static_cast<const std::int64_t*>(dftu_series_data(d))[0];
        dftu_series_free(d);
        dftu_series_free(c);
        dftu_series_free(b);
        dftu_series_free(a);
    }
    auto end = Clock::now();
    for (std::size_t i = 0; i < s.nested_ops; ++i) {
        expect +=
            base[at[i % OFFSET_TABLE] + 3 * static_cast<std::int64_t>(i % 7)];
    }
    check(sum == expect, "depth 4 values");
    return elapsed_ns(start, end) / static_cast<double>(s.nested_ops);
}

double slice_string(const Scale& s) {
    Series col = string_column(s.string_rows);
    auto at = offsets(s.string_rows, s.slice_len, 1, 0);
    return slice_loop(col, at, s.slice_len, s.string_ops);
}

double slice_nullable(const Scale& s, std::int64_t shift) {
    std::vector<std::int64_t> values =
        random_i64(static_cast<std::size_t>(s.big_rows), 1 << 30, SEED);
    std::vector<std::uint8_t> validity = sparse_validity(s.big_rows);
    Series col = Series::flat_i64(values.data(), s.big_rows, validity.data());
    values = {};
    auto at = offsets(s.big_rows, s.slice_len, 8, shift);
    return slice_loop(col, at, s.slice_len, s.nested_ops);
}

DataFrame wide_frame(std::int64_t rows, bool nullable) {
    DataFrame frame;
    frame.names = {"i", "f", "s"};
    frame.columns.push_back(int_column(rows));
    std::vector<double> f = random_f64(static_cast<std::size_t>(rows), SEED);
    std::vector<std::uint8_t> validity;
    if (nullable) validity = sparse_validity(rows);
    frame.columns.push_back(
        Series::flat_f64(f.data(), rows, nullable ? validity.data() : nullptr));
    frame.columns.push_back(string_column(rows));
    return frame;
}

double head_tail(const Scale& s) {
    DataFrame frame = wide_frame(s.big_rows, false);
    std::int64_t rows = 0;
    auto start = Clock::now();
    for (std::size_t i = 0; i < s.head_ops; ++i) {
        DataFrame part =
            (i & 1) != 0 ? frame.tail(s.slice_len) : frame.head(s.slice_len);
        rows += part.num_rows();
    }
    auto end = Clock::now();
    check(rows == s.slice_len * static_cast<std::int64_t>(s.head_ops),
          "head tail rows");
    return elapsed_ns(start, end) / static_cast<double>(s.head_ops);
}

double filter_half(const Scale& s) {
    Series col = int_column(s.big_rows);
    std::vector<std::uint8_t> bits(
        static_cast<std::size_t>((s.big_rows + 7) / 8), 0x55);
    Series mask = Series::flat(TypeId::Bool, bits.data(), s.big_rows);
    std::int64_t rows = 0;
    auto start = Clock::now();
    for (std::size_t i = 0; i < s.filter_ops; ++i) {
        Series kept = col.filter(mask);
        rows += kept.length();
    }
    auto end = Clock::now();
    check(rows == (s.big_rows / 2) * static_cast<std::int64_t>(s.filter_ops),
          "filter rows");
    return elapsed_ns(start, end) / static_cast<double>(s.filter_ops);
}

double concat_slices(const Scale& s) {
    Series ints = int_column(s.big_rows);
    Series strs = string_column(s.string_rows);
    std::int64_t len =
        s.string_rows / static_cast<std::int64_t>(s.concat_parts);
    auto at = offsets(s.string_rows, len, 1, 0);
    const char* names[] = {"i", "s"};
    std::vector<dftu_dataframe*> parts(s.concat_parts);
    std::int64_t rows = 0;
    auto start = Clock::now();
    for (std::size_t rep = 0; rep < s.concat_ops; ++rep) {
        for (std::size_t p = 0; p < s.concat_parts; ++p) {
            std::int64_t o = at[(rep * s.concat_parts + p) % OFFSET_TABLE];
            dftu_series* cols[] = {dftu_series_slice(ints.handle(), o, len),
                                   dftu_series_slice(strs.handle(), o, len)};
            parts[p] = dftu_dataframe_new(names, cols, 2);
        }
        dftu_dataframe* all = dftu_dataframe_concat(
            parts.data(), static_cast<std::int32_t>(parts.size()),
            DFTU_CONCAT_VERTICAL);
        rows += dftu_dataframe_num_rows(all);
        dftu_dataframe_free(all);
        for (dftu_dataframe* p : parts) dftu_dataframe_free(p);
    }
    auto end = Clock::now();
    check(
        rows == len * static_cast<std::int64_t>(s.concat_parts * s.concat_ops),
        "concat rows");
    return elapsed_ns(start, end) / static_cast<double>(s.concat_ops);
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW
double arrow_round_trip(const Scale& s) {
    DataFrame frame = wide_frame(s.op_rows, true);
    std::int64_t rows = 0;
    auto start = Clock::now();
    for (std::size_t i = 0; i < s.arrow_ops; ++i) {
        df::OwnedArrow out = frame.to_arrow();
        DataFrame back = DataFrame::from_arrow(out.schema(), out.array());
        rows += back.num_rows() * static_cast<std::int64_t>(back.num_columns());
    }
    auto end = Clock::now();
    check(rows == 3 * s.op_rows * static_cast<std::int64_t>(s.arrow_ops),
          "arrow round trip rows");
    return elapsed_ns(start, end) / static_cast<double>(s.arrow_ops);
}
#endif

DataFrame key_frame(std::int64_t rows, std::int64_t groups) {
    DataFrame frame;
    frame.names = {"k", "g", "v"};
    std::vector<std::int64_t> k =
        random_i64(static_cast<std::size_t>(rows), 1 << 30, SEED);
    std::vector<std::int64_t> g =
        random_i64(static_cast<std::size_t>(rows), groups, SEED + 3);
    std::vector<double> v = random_f64(static_cast<std::size_t>(rows), SEED);
    frame.columns.push_back(Series::flat_i64(k.data(), rows));
    frame.columns.push_back(Series::flat_i64(g.data(), rows));
    frame.columns.push_back(Series::flat_f64(v.data(), rows));
    return frame;
}

template <class Op>
double repeat_op(std::size_t reps, Op op) {
    auto start = Clock::now();
    for (std::size_t i = 0; i < reps; ++i) op();
    auto end = Clock::now();
    return elapsed_ns(start, end) / static_cast<double>(reps);
}

double sort_by(const Scale& s) {
    DataFrame frame = key_frame(s.op_rows, 1000);
    return repeat_op(s.sort_reps, [&] {
        DataFrame out = frame.sort_by("k");
        check(out.num_rows() == s.op_rows, "sort rows");
    });
}

double group_by_sum(const Scale& s) {
    DataFrame frame = key_frame(s.op_rows, 10000);
    std::vector<df::GroupAgg> aggs{{df::Agg::Sum, "v", "sum_v"}};
    return repeat_op(s.group_reps, [&] {
        DataFrame out = frame.group_by("g", aggs);
        check(out.num_rows() > 0 && out.num_rows() <= 10000, "group rows");
    });
}

double unique_rows(const Scale& s) {
    DataFrame frame = key_frame(s.op_rows, s.op_rows / 4);
    return repeat_op(s.unique_reps, [&] {
        DataFrame out = frame.unique({"g"});
        check(out.num_rows() > 0 && out.num_rows() <= s.op_rows / 4,
              "unique rows");
    });
}

double join_inner(const Scale& s) {
    auto rows = static_cast<std::size_t>(s.op_rows);
    std::vector<std::int64_t> left_keys(rows);
    std::iota(left_keys.begin(), left_keys.end(), 0);
    std::vector<std::int64_t> right_keys = left_keys;
    std::shuffle(left_keys.begin(), left_keys.end(), std::mt19937_64(SEED));
    std::shuffle(right_keys.begin(), right_keys.end(),
                 std::mt19937_64(SEED + 1));
    std::vector<double> lv = random_f64(rows, SEED);
    std::vector<double> rv = random_f64(rows, SEED + 1);
    DataFrame left;
    left.names = {"k", "lv"};
    left.columns.push_back(Series::flat_i64(left_keys.data(), s.op_rows));
    left.columns.push_back(Series::flat_f64(lv.data(), s.op_rows));
    DataFrame right;
    right.names = {"k", "rv"};
    right.columns.push_back(Series::flat_i64(right_keys.data(), s.op_rows));
    right.columns.push_back(Series::flat_f64(rv.data(), s.op_rows));
    return repeat_op(s.join_reps, [&] {
        DataFrame out = left.join(right, std::vector<std::string>{"k"});
        check(out.num_rows() == s.op_rows, "join rows");
    });
}

struct Case {
    std::string name;
    std::function<double(const Scale&)> run;
};

std::vector<Case> make_cases() {
    std::vector<Case> cases{
        {"slice int64", slice_int64},
        {"slice depth 4", slice_depth4},
        {"slice string", slice_string},
        {"slice nullable aligned",
         [](const Scale& s) { return slice_nullable(s, 0); }},
        {"slice nullable unaligned",
         [](const Scale& s) { return slice_nullable(s, 3); }},
        {"head tail", head_tail},
        {"filter half", filter_half},
        {"concat 1000 slices", concat_slices},
    };
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    cases.push_back({"arrow round trip", arrow_round_trip});
#endif
    cases.push_back({"sort_by", sort_by});
    cases.push_back({"group_by sum", group_by_sum});
    cases.push_back({"unique", unique_rows});
    cases.push_back({"join inner", join_inner});
    return cases;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

struct ChildResult {
    double ns;
    double rss_mib;
};

std::optional<ChildResult> run_child(const std::string& binary,
                                     const std::string& name) {
    int fds[2];
    if (::pipe(fds) != 0) return std::nullopt;
    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[1]);
        ::execl(binary.c_str(), binary.c_str(), "--case", name.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(fds[1]);
    std::string out;
    char buffer[256];
    ssize_t n;
    while ((n = ::read(fds[0], buffer, sizeof(buffer))) > 0) {
        out.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fds[0]);
    int status = 0;
    struct rusage usage{};
    ::wait4(pid, &status, 0, &usage);
    double ns = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        std::sscanf(out.c_str(), "RESULT %lf", &ns) != 1) {
        return std::nullopt;
    }
#if defined(__APPLE__)
    double rss_bytes = static_cast<double>(usage.ru_maxrss);
#else
    double rss_bytes = static_cast<double>(usage.ru_maxrss) * 1024.0;
#endif
    return ChildResult{ns, rss_bytes / static_cast<double>(MIB)};
}

int drive(const std::string& self, const std::string& against, int rounds,
          const std::string& label) {
    std::vector<Case> cases = make_cases();
    std::vector<std::string> sides;
    if (!against.empty()) sides.push_back(against);
    sides.push_back(self);
    std::vector<std::vector<std::vector<double>>> ns(
        sides.size(), std::vector<std::vector<double>>(cases.size()));
    auto rss = ns;
    std::printf("slice_bench label=\"%s\" rounds=%d hw=%u\n", label.c_str(),
                rounds, std::thread::hardware_concurrency());
    for (std::size_t side = 0; side < sides.size(); ++side) {
        std::printf("side %zu: %s\n", side, sides[side].c_str());
    }
    for (int round = 0; round < rounds; ++round) {
        for (std::size_t c = 0; c < cases.size(); ++c) {
            for (std::size_t side = 0; side < sides.size(); ++side) {
                auto result = run_child(sides[side], cases[c].name);
                if (!result) {
                    std::fprintf(stderr, "case failed: %s (side %zu)\n",
                                 cases[c].name.c_str(), side);
                    return 1;
                }
                ns[side][c].push_back(result->ns);
                rss[side][c].push_back(result->rss_mib);
                std::printf(
                    "raw round=%d case=\"%s\" side=%zu ns=%.1f rss_mib=%.1f\n",
                    round, cases[c].name.c_str(), side, result->ns,
                    result->rss_mib);
                std::fflush(stdout);
            }
        }
    }
    if (sides.size() == 1) {
        std::printf(
            "\n| case | median ns per op | peak RSS MiB |\n|---|---|---|\n");
        for (std::size_t c = 0; c < cases.size(); ++c) {
            std::printf("| %s | %.1f | %.1f |\n", cases[c].name.c_str(),
                        median(ns[0][c]), median(rss[0][c]));
        }
        return 0;
    }
    std::printf(
        "\n| case | side 0 median ns | side 1 median ns | ratio | peak RSS "
        "side 0 MiB | peak RSS side 1 MiB |\n|---|---|---|---|---|---|\n");
    for (std::size_t c = 0; c < cases.size(); ++c) {
        double a = median(ns[0][c]);
        double b = median(ns[1][c]);
        std::printf("| %s | %.1f | %.1f | %.3f | %.1f | %.1f |\n",
                    cases[c].name.c_str(), a, b, b / a, median(rss[0][c]),
                    median(rss[1][c]));
    }
    return 0;
}

int smoke() {
    int failures = 0;
    for (const Case& c : make_cases()) {
        bool ok = false;
        try {
            ok = c.run(SMOKE_SCALE) > 0.0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s\n", c.name.c_str(), e.what());
        }
        std::printf("smoke %-26s %s\n", c.name.c_str(), ok ? "ok" : "FAILED");
        if (!ok) ++failures;
    }
    return failures == 0 ? 0 : 1;
}

int run_one(const std::string& name) {
    for (const Case& c : make_cases()) {
        if (c.name != name) continue;
        try {
            std::printf("RESULT %.3f\n", c.run(FULL_SCALE));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s\n", name.c_str(), e.what());
            return 1;
        }
        return 0;
    }
    std::fprintf(stderr, "unknown case: %s\n", name.c_str());
    return 1;
}

int usage() {
    std::fprintf(stderr,
                 "usage: slice_bench [--smoke | --case NAME | [--rounds N] "
                 "[--label TEXT] [--against PATH]]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string_view first = argc > 1 ? argv[1] : "";
    if (first == "--smoke" && argc == 2) return smoke();
    if (first == "--case" && argc == 3) return run_one(argv[2]);
    int rounds = DEFAULT_ROUNDS;
    std::string label;
    std::string against;
    for (int i = 1; i < argc; i += 2) {
        std::string_view flag = argv[i];
        if (i + 1 >= argc) return usage();
        if (flag == "--rounds") {
            rounds = std::atoi(argv[i + 1]);
        } else if (flag == "--label") {
            label = argv[i + 1];
        } else if (flag == "--against") {
            against = argv[i + 1];
        } else {
            return usage();
        }
    }
    return rounds > 0 ? drive(argv[0], against, rounds, label) : usage();
}
