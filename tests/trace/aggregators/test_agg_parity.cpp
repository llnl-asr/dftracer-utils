#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Parity references for the aggregate outputs of dftracer_comparator, the
// View aggregate queries and dftracer_gen_dlio_config, taken from the code as
// it was before the aggregation redesign. The fixtures are generated from
// fixed seeds, so the references are small text files under data/.
//
// Set DFTRACER_UTILS_AGG_PARITY_RECORD=<dir> to write the references to <dir>
// instead of comparing against data/.
//
// Comparison rules:
//  - counts, sums, mins, maxes, timestamps, bucket starts, busy and active
//    times, every string and every key: exact.
//  - means, variances, standard deviations, skewness, kurtosis, concurrency
//    and utilization: relative tolerance 1e-9. The comparator's deltas and
//    percentage changes are derived from two such values and follow the
//    same rule.
//  - quantiles (p50, p90, p99 columns, the comparator's *_p50/_p95/_p99
//    metrics, and the DLIO distribution parameters that are sampled from
//    quantiles): relative tolerance 0.01, the accuracy of the DDSketch. A
//    delta or percentage change of two quantiles is bounded by the error of
//    both operands.
//  - View rows are compared as a set keyed by the group key columns, never by
//    position. Comparator entries (nodes, groups, metrics) are matched by
//    name or label, not by position, because the report orders groups by
//    values that move with the quantiles. The DLIO YAML lines keep their
//    order, which is part of that output.
//  - a field or row in the reference that is missing in the output fails, and
//    so does one in the output that is missing in the reference.

namespace {

namespace df = dftracer::utils::dataframe;
using dftracer::utils::dstd::Map;
using dftracer::utils::dstd::String;
using dftracer::utils::dstd::StringView;
using dftracer::utils::dstd::Vector;

constexpr const char* RECORD_ENV = "DFTRACER_UTILS_AGG_PARITY_RECORD";
constexpr double TOL_EXACT = 0.0;
constexpr double TOL_MOMENT = 1e-9;
constexpr double TOL_QUANTILE = 0.01;
constexpr int FILES_PER_SET = 6;
constexpr int EVENTS_PER_FILE = 700;
constexpr std::uint64_t BASE_TS = 1000000000ULL;
constexpr std::uint64_t SPAN_US = 12000000ULL;
constexpr std::uint64_t ONE_SECOND_US = 1000000ULL;
constexpr std::uint64_t FIVE_SECONDS_US = 5000000ULL;

String read_text(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return String(ss.view());
}

// Writes the reference when recording and returns "", else returns the
// reference text (a failure when the file is missing).
String reference(const char* file, StringView produced) {
    if (const char* dir = std::getenv(RECORD_ENV)) {
        fs::create_directories(dir);
        std::ofstream ofs(std::string(dir) + "/" + file, std::ios::binary);
        ofs << produced;
        return String();
    }
    const std::string path =
        std::string(DFTRACER_UTILS_AGG_PARITY_DATA_DIR) + "/" + file;
    String text = read_text(path);
    INFO("missing reference " << path);
    REQUIRE(!text.empty());
    return text;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t n) { return next() % n; }
};

enum class Kind { Read, Write, Open, Close, Fwrite, Step, Fetch };

// One rank file. The variant changes durations, sizes and one event name so
// the comparator report has differences in every direction.
std::string write_trace(const std::string& dir, int file, std::uint64_t seed,
                        bool variant) {
    const std::string pfw = dir + "/rank_" + std::to_string(file) + ".pfw";
    {
        std::ofstream ofs(pfw);
        Rng rng(seed * 1000 + static_cast<std::uint64_t>(file));
        const std::uint64_t pid = 1000 + static_cast<std::uint64_t>(file);
        const std::uint64_t step = SPAN_US / EVENTS_PER_FILE;
        const std::uint64_t sizes[] = {4096, 65536, 1048576, 262144};
        ofs << "[\n";
        for (int i = 0; i < EVENTS_PER_FILE; ++i) {
            const std::uint64_t ts = BASE_TS +
                                     static_cast<std::uint64_t>(i) * step +
                                     rng.below(step / 2);
            const std::uint64_t tid = pid * 10 + rng.below(3);
            const std::uint64_t r = rng.below(100);
            const std::uint64_t size = sizes[rng.below(4)];
            const std::uint64_t offset = rng.below(1024) * 4096;
            const std::uint64_t fh = rng.below(5);
            Kind kind;
            const char* cat;
            const char* name;
            std::uint64_t dur;
            if (r < 35) {
                kind = Kind::Read;
                cat = "POSIX";
                name = "read";
                dur = 50 + rng.below(2000);
                if (variant) dur = dur * 9 / 10;
            } else if (r < 60) {
                kind = Kind::Write;
                cat = "POSIX";
                name = "write";
                dur = 80 + rng.below(3000);
                if (variant) dur = dur * 3 / 2;
            } else if (r < 70) {
                kind = Kind::Open;
                cat = "POSIX";
                name = "open";
                dur = 20 + rng.below(200);
            } else if (r < 78) {
                kind = Kind::Close;
                cat = "POSIX";
                name = variant ? "fsync" : "close";
                dur = 10 + rng.below(100);
            } else if (r < 88) {
                kind = Kind::Fwrite;
                cat = "STDIO";
                name = "fwrite";
                dur = 5 + rng.below(50);
            } else if (r < 96) {
                kind = Kind::Step;
                cat = "COMPUTE";
                name = "step";
                dur = 1000 + rng.below(8000);
            } else {
                kind = Kind::Fetch;
                cat = "dataloader";
                name = "fetch.iter";
                dur = 200 + rng.below(1500);
            }
            ofs << R"({"id":)" << i << R"(,"ph":"X","pid":)" << pid
                << R"(,"tid":)" << tid << R"(,"name":")" << name
                << R"(","cat":")" << cat << R"(","ts":)" << ts << R"(,"dur":)"
                << dur << R"(,"args":{)";
            switch (kind) {
                case Kind::Read:
                    ofs << R"("fhash":"f)" << fh << R"(","size":)"
                        << (variant ? size * 2 : size) << R"(,"offset":)"
                        << offset << R"(,"ret":)" << size;
                    break;
                case Kind::Write:
                    ofs << R"("fhash":"f)" << fh << R"(","size":)" << size
                        << R"(,"offset":)" << offset << R"(,"ret":)" << size;
                    break;
                case Kind::Open:
                case Kind::Close:
                    ofs << R"("fhash":"f)" << fh << '"';
                    break;
                case Kind::Fwrite:
                    ofs << R"("size":)" << size / 16;
                    break;
                case Kind::Step:
                    ofs << R"("epoch":)" << i / 100 << R"(,"step":)" << i % 100;
                    break;
                case Kind::Fetch:
                    ofs << R"("level":)" << rng.below(10);
                    break;
            }
            ofs << "}}\n";
        }
        ofs << "]\n";
    }
    const std::string gz = pfw + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
    fs::remove(pfw);
    return gz;
}

// DLIO components over two ranks with fixed durations, as the gen_dlio_config
// binary test does, plus a seeded jitter so the fitted distributions are not
// degenerate.
void write_dlio_traces(const std::string& dir) {
    fs::create_directories(dir);
    for (int file = 0; file < 2; ++file) {
        const std::string pfw = dir + "/dlio_" + std::to_string(file) + ".pfw";
        {
            std::ofstream ofs(pfw);
            Rng rng(77 + static_cast<std::uint64_t>(file));
            ofs << "[\n";
            std::uint64_t id = 1;
            for (int rank = 0; rank < 2; ++rank) {
                const std::uint64_t pid =
                    1000 + static_cast<std::uint64_t>(file * 2 + rank);
                const std::uint64_t tid = 2000 + pid;
                std::uint64_t ts =
                    BASE_TS + static_cast<std::uint64_t>(rank) * 100000ULL;
                for (int i = 0; i < 200; ++i) {
                    const auto n = static_cast<std::uint64_t>(i);
                    const struct {
                        const char* cat;
                        const char* name;
                        std::uint64_t pid;
                        std::uint64_t dur;
                    } events[] = {{"dataloader", "fetch.block", pid,
                                   100 + (n * 7) % 500 + rng.below(40)},
                                  {"dataloader", "fetch.iter", pid,
                                   50 + (n * 3) % 100 + rng.below(20)},
                                  {"data", "preprocess", pid + 100000,
                                   80 + (n * 5) % 200 + rng.below(30)}};
                    for (const auto& e : events) {
                        ofs << R"({"id":)" << id++ << R"(,"pid":)" << e.pid
                            << R"(,"tid":)" << tid << R"(,"name":")" << e.name
                            << R"(","cat":")" << e.cat << R"(","ph":"X","ts":)"
                            << ts << R"(,"dur":)" << e.dur
                            << R"(,"args":{"hhash":"h1"}})" << "\n";
                        ts += e.dur;
                    }
                }
            }
            ofs << "]\n";
        }
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, pfw + ".gz"));
        fs::remove(pfw);
    }
}

struct Fixtures {
    dftu_utils_test::ScopedTestDir root{"agg_parity"};
    std::string baseline_dir;
    std::string variant_dir;
    std::string dlio_dir;
    std::vector<std::string> baseline_files;

    Fixtures() {
        baseline_dir = root.str() + "/baseline";
        variant_dir = root.str() + "/variant";
        dlio_dir = root.str() + "/dlio";
        fs::create_directories(baseline_dir);
        fs::create_directories(variant_dir);
        for (int f = 0; f < FILES_PER_SET; ++f) {
            baseline_files.push_back(write_trace(baseline_dir, f, 11, false));
            write_trace(variant_dir, f, 29, true);
        }
        write_dlio_traces(dlio_dir);
    }
};

Fixtures& fixtures() {
    static Fixtures f;
    return f;
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

String escape(StringView s) {
    String out;
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7e || c == '\\') {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

String number_text(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return String(buf);
}

Vector<String> split(StringView s, char sep) {
    Vector<String> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto p = s.find(sep, start);
        if (p == StringView::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

bool starts_with(StringView s, StringView prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

// Relative closeness with a small absolute floor for values that are zero up
// to rounding.
bool rel_close(double x, double y, double tol) {
    return std::fabs(x - y) <= tol * std::max(std::fabs(x), std::fabs(y)) +
                                   (tol > 0.0 ? 1e-12 : 0.0);
}

// ---------------------------------------------------------------------------
// View aggregate tables
// ---------------------------------------------------------------------------

// Line 1: column names. Line 2: S for a text column, N for a number. Then one
// line per row, sorted by the key columns, tab separated.
String table_text(const df::DataFrame& t, const Vector<String>& keys) {
    const std::size_t ncols = t.names.size();
    Vector<bool> text(ncols);
    Vector<bool> is_key(ncols);
    for (std::size_t c = 0; c < ncols; ++c) {
        text[c] = t.columns[c].type() == df::TypeId::String;
        is_key[c] = std::find(keys.begin(), keys.end(),
                              StringView(t.names[c])) != keys.end();
    }
    Vector<std::pair<String, String>> rows;
    for (std::int64_t r = 0; r < t.num_rows(); ++r) {
        String line, key;
        for (std::size_t c = 0; c < ncols; ++c) {
            String cell;
            if (t.columns[c].is_null(r)) {
                cell = "null";
            } else if (text[c]) {
                cell = escape(t.columns[c].string_at(r));
            } else {
                cell = number_text(bnum(t, r, t.names[c]));
            }
            if (c) line += '\t';
            line += cell;
            if (is_key[c]) {
                key += cell;
                key += '\x1f';
            }
        }
        rows.emplace_back(key + line, line);
    }
    std::sort(rows.begin(), rows.end());
    String out;
    for (std::size_t c = 0; c < ncols; ++c) {
        if (c) out += '\t';
        out += StringView(t.names[c]);
    }
    out += '\n';
    for (std::size_t c = 0; c < ncols; ++c) {
        if (c) out += '\t';
        out += text[c] ? 'S' : 'N';
    }
    out += '\n';
    for (const auto& row : rows) {
        out += row.second;
        out += '\n';
    }
    return out;
}

// Relative tolerance of a numeric column, by its name. An unclassified name
// fails the test so that a new column gets a rule on purpose. The bare names
// epoch, level, size, step, offset and ret are the per-group means of those
// event arguments.
double column_tolerance(StringView name, bool& known) {
    known = true;
    if (name == "n" || name == "time_bucket" || starts_with(name, "busy") ||
        starts_with(name, "active") || starts_with(name, "count") ||
        starts_with(name, "n_") || starts_with(name, "sum") ||
        starts_with(name, "min") || starts_with(name, "max"))
        return TOL_EXACT;
    if (name == "epoch" || name == "level" || name == "size" ||
        name == "step" || name == "offset" || name == "ret" ||
        starts_with(name, "mean") || starts_with(name, "var") ||
        starts_with(name, "std") || starts_with(name, "skew") ||
        starts_with(name, "kurt") || starts_with(name, "concurrency") ||
        starts_with(name, "utilization"))
        return TOL_MOMENT;
    if (name.size() > 1 && name[0] == 'p' &&
        std::isdigit(static_cast<unsigned char>(name[1])))
        return TOL_QUANTILE;
    known = false;
    return TOL_EXACT;
}

bool cells_close(const String& a, const String& b, double tol) {
    if (a == b) return true;
    if (tol == 0.0 || a == "null" || b == "null") return false;
    return rel_close(std::strtod(a.c_str(), nullptr),
                     std::strtod(b.c_str(), nullptr), tol);
}

struct ParsedTable {
    Vector<String> cols;
    Vector<String> kinds;
    Map<String, Vector<String>> rows;
};

ParsedTable parse_table(StringView text, const Vector<String>& keys) {
    ParsedTable t;
    const auto lines = split(text, '\n');
    REQUIRE(lines.size() >= 2);
    t.cols = split(lines[0], '\t');
    t.kinds = split(lines[1], '\t');
    Vector<std::size_t> key_idx;
    for (const auto& k : keys) {
        const auto it = std::find(t.cols.begin(), t.cols.end(), k);
        REQUIRE(it != t.cols.end());
        key_idx.push_back(static_cast<std::size_t>(it - t.cols.begin()));
    }
    for (std::size_t i = 2; i < lines.size(); ++i) {
        if (lines[i].empty()) continue;
        auto cells = split(lines[i], '\t');
        REQUIRE(cells.size() == t.cols.size());
        String key;
        for (auto k : key_idx) {
            key += cells[k];
            key += '\x1f';
        }
        const bool fresh = t.rows.emplace(key, std::move(cells)).second;
        CHECK_MESSAGE(fresh, "duplicate group key");
    }
    return t;
}

void compare_table(const char* label, StringView ref_text, StringView out_text,
                   const Vector<String>& keys) {
    INFO(label);
    const ParsedTable ref = parse_table(ref_text, keys);
    const ParsedTable out = parse_table(out_text, keys);
    CHECK(ref.cols == out.cols);
    CHECK(ref.kinds == out.kinds);
    if (ref.cols != out.cols) return;
    for (const auto& [key, row] : ref.rows) {
        const auto it = out.rows.find(key);
        CHECK_MESSAGE(it != out.rows.end(),
                      "row missing in output: " << escape(key));
        if (it == out.rows.end()) continue;
        for (std::size_t c = 0; c < ref.cols.size(); ++c) {
            double tol = TOL_EXACT;
            if (ref.kinds[c] == "N") {
                bool known = false;
                tol = column_tolerance(ref.cols[c], known);
                CHECK_MESSAGE(known, "column without a rule: " << ref.cols[c]);
            }
            CHECK_MESSAGE(cells_close(row[c], it->second[c], tol),
                          ref.cols[c] << " at " << escape(key) << ": " << row[c]
                                      << " vs " << it->second[c]);
        }
    }
    for (const auto& entry : out.rows)
        CHECK_MESSAGE(ref.rows.count(entry.first) != 0,
                      "row not in reference: " << escape(entry.first));
}

using BuildView = View (*)(const View&);

struct ViewQuery {
    const char* file;
    Vector<String> keys;
    BuildView build;
};

// The aggregate specs are arguments of View::agg, which takes std::vector.
const std::vector<AggSpec>& moment_aggs() {
    static const std::vector<AggSpec> aggs{
        {AggOp::Count, "", "n"},
        {AggOp::Sum, "dur", "sum_dur"},
        {AggOp::Min, "dur", "min_dur"},
        {AggOp::Max, "dur", "max_dur"},
        {AggOp::Mean, "dur", "mean_dur"},
        {AggOp::Var, "dur", "var_dur"},
        {AggOp::Std, "dur", "std_dur"},
        {AggOp::Skew, "dur", "skew_dur"},
        {AggOp::Kurt, "dur", "kurt_dur"},
        {AggOp::Pct, "dur", "p50_dur", "", 0.5},
        {AggOp::Pct, "dur", "p90_dur", "", 0.9},
        {AggOp::Pct, "dur", "p99_dur", "", 0.99}};
    return aggs;
}

const std::vector<AggSpec>& occupancy_aggs() {
    static const std::vector<AggSpec> aggs{
        {AggOp::Count, "", "n"},
        {AggOp::Busy, "", "busy"},
        {AggOp::Active, "", "active"},
        {AggOp::Concurrency, "", "concurrency"},
        {AggOp::Utilization, "", "utilization"},
        {AggOp::SetUnion, "name", "names"},
        {AggOp::ArgMax, "name", "top_name", "dur"}};
    return aggs;
}

Vector<ViewQuery> view_queries() {
    Vector<ViewQuery> queries{
        {"view_cat.tsv",
         {"cat"},
         [](const View& v) {
             return v.group_by({GroupKey::cat()}).agg(moment_aggs());
         }},
        {"view_name.tsv",
         {"name"},
         [](const View& v) {
             return v.group_by({GroupKey::name()}).agg(moment_aggs());
         }},
        {"view_cat_name.tsv",
         {"cat", "name"},
         [](const View& v) {
             return v.group_by({GroupKey::cat(), GroupKey::name()})
                 .agg(moment_aggs());
         }},
        {"view_pid.tsv",
         {"pid"},
         [](const View& v) {
             return v.group_by({GroupKey::pid()}).agg(moment_aggs());
         }},
        {"view_tid.tsv",
         {"tid"},
         [](const View& v) {
             return v.group_by({GroupKey::tid()}).agg(moment_aggs());
         }},
        {"view_pid_tid_name.tsv",
         {"pid", "tid", "name"},
         [](const View& v) {
             return v
                 .group_by({GroupKey::pid(), GroupKey::tid(), GroupKey::name()})
                 .agg({{AggOp::Count, "", "n"},
                       {AggOp::Sum, "dur", "sum_dur"},
                       {AggOp::Mean, "dur", "mean_dur"}});
         }},
        {"view_io_cat.tsv",
         {"io_cat"},
         [](const View& v) {
             return v.group_by({GroupKey::io_cat()}).agg(moment_aggs());
         }},
        {"view_cat_1s.tsv",
         {"time_bucket", "cat"},
         [](const View& v) {
             return v.group_by({GroupKey::cat()})
                 .time_bucket(ONE_SECOND_US)
                 .agg(moment_aggs());
         }},
        {"view_cat_5s.tsv",
         {"time_bucket", "cat"},
         [](const View& v) {
             return v.group_by({GroupKey::cat()})
                 .time_bucket(FIVE_SECONDS_US)
                 .agg(moment_aggs());
         }},
        {"view_name_5s.tsv",
         {"time_bucket", "name"},
         [](const View& v) {
             return v.group_by({GroupKey::name()})
                 .time_bucket(FIVE_SECONDS_US)
                 .agg({{AggOp::Count, "", "n"},
                       {AggOp::Sum, "dur", "sum_dur"},
                       {AggOp::Pct, "dur", "p90_dur", "", 0.9}});
         }},
        {"view_name_numeric_args.tsv",
         {"name"},
         [](const View& v) {
             return v.group_by({GroupKey::name()}).agg_numeric_args();
         }},
        {"view_name_numeric_args_pct.tsv",
         {"name"},
         [](const View& v) {
             return v.group_by({GroupKey::name()})
                 .agg_numeric_args({AggSpec(AggOp::Pct, "", "p50", "", 0.5),
                                    AggSpec(AggOp::Pct, "", "p99", "", 0.99)});
         }},
        {"view_cat_occupancy.tsv",
         {"cat"},
         [](const View& v) {
             return v.group_by({GroupKey::cat()}).agg(occupancy_aggs());
         }},
        {"view_all_occupancy.tsv", {}, [](const View& v) {
             return v.agg(occupancy_aggs());
         }}};
    return queries;
}

std::vector<ViewFile> view_files(const std::vector<std::string>& gz) {
    std::vector<ViewFile> files;
    for (const auto& f : gz) files.push_back({f, determine_index_path(f, "")});
    return files;
}

// ---------------------------------------------------------------------------
// Comparator report
// ---------------------------------------------------------------------------

String replace_all(String s, StringView from, StringView to) {
    if (from.empty()) return s;
    for (auto p = s.find(from); p != String::npos;
         p = s.find(from, p + to.size()))
        s.replace(p, from.size(), to);
    return s;
}

// The scratch directory name differs on every run and the wall-clock time of
// the report is not an output of the aggregation; both are fixed to constants.
String normalize_report(String text) {
    text = replace_all(std::move(text), fixtures().root.str(), "<DIR>");
    const StringView tag = "\"execution_time_ms\":";
    for (auto p = text.find(tag); p != String::npos;
         p = text.find(tag, p + 1)) {
        std::size_t b = p + tag.size();
        while (b < text.size() && text[b] == ' ') ++b;
        std::size_t e = b;
        while (e < text.size() &&
               (std::isdigit(static_cast<unsigned char>(text[e])) ||
                text[e] == '.' || text[e] == '-' || text[e] == 'e' ||
                text[e] == '+'))
            ++e;
        text.replace(b, e - b, "0");
    }
    return text;
}

// The arguments go to run_process_capture, which takes std strings.
String run_comparator(const std::vector<std::string>& extra) {
    const std::string binary = dftu_utils_test::find_binary_by_name(
        "DFTRACER_UTILS_COMPARATOR_PATH", "dftracer_comparator");
    REQUIRE(!binary.empty());
    std::vector<std::string> args{"--baseline", fixtures().baseline_dir,
                                  "--variant",  fixtures().variant_dir,
                                  "--format",   "json"};
    args.insert(args.end(), extra.begin(), extra.end());
    int rc = -1;
    const std::string out =
        dftu_utils_test::run_process_capture(binary, args, false, &rc);
    REQUIRE(rc == 0);
    return normalize_report(String(out));
}

using Elem = simdjson::dom::element;
using Obj = simdjson::dom::object;
using Arr = simdjson::dom::array;
using Fields = Map<String, Elem>;
using EachFn = void (*)(const Elem&, const Elem&, const String&);

enum class MetricKind { Exact, Moment, Quantile };

MetricKind metric_kind(StringView name, bool& known) {
    static constexpr StringView exact[] = {
        "count",   "unique_files",  "proc_files", "processes",
        "threads", "time_pipeline", "time_io",    "total_bytes"};
    static constexpr StringView moment[] = {"dur_mean", "size_mean",
                                            "transfer_size", "bandwidth"};
    known = true;
    if (std::find(std::begin(exact), std::end(exact), name) != std::end(exact))
        return MetricKind::Exact;
    if (std::find(std::begin(moment), std::end(moment), name) !=
        std::end(moment))
        return MetricKind::Moment;
    const auto p = name.rfind("_p");
    if (p != StringView::npos && p + 2 < name.size() &&
        std::isdigit(static_cast<unsigned char>(name[p + 2])))
        return MetricKind::Quantile;
    known = false;
    return MetricKind::Exact;
}

Fields fields_of(const Elem& e, const String& where) {
    Fields out;
    Obj obj;
    REQUIRE_MESSAGE(e.get(obj) == simdjson::SUCCESS,
                    where << ": not an object");
    for (auto field : obj) out.emplace(String(field.key), field.value);
    return out;
}

Vector<Elem> items_of(const Elem& e, const String& where) {
    Vector<Elem> out;
    Arr arr;
    REQUIRE_MESSAGE(e.get(arr) == simdjson::SUCCESS, where << ": not an array");
    for (auto item : arr) out.push_back(item);
    return out;
}

String string_field(const Fields& f, StringView key) {
    const auto it = f.find(key);
    StringView sv;
    if (it == f.end() || it->second.get(sv) != simdjson::SUCCESS)
        return String();
    return String(sv);
}

double number_field(const Fields& f, StringView key) {
    double v = 0;
    const auto it = f.find(key);
    if (it != f.end() && it->second.get(v) != simdjson::SUCCESS) v = 0;
    return v;
}

void compare_key_sets(const Fields& ref, const Fields& out,
                      const String& where) {
    for (const auto& entry : ref)
        CHECK_MESSAGE(out.count(entry.first) != 0,
                      where << ": field missing in output: " << entry.first);
    for (const auto& entry : out)
        CHECK_MESSAGE(ref.count(entry.first) != 0,
                      where << ": field not in reference: " << entry.first);
}

// A scalar that is not a metric number: strings and bools exact, numbers
// exact (counts and byte totals).
void compare_exact(const Elem& ref, const Elem& out, const String& where) {
    const bool numbers = ref.is_number() && out.is_number();
    const bool same_type = ref.type() == out.type() || numbers;
    CHECK_MESSAGE(same_type, where << ": type differs");
    if (numbers) {
        double a = 0, b = 0;
        REQUIRE(ref.get(a) == simdjson::SUCCESS);
        REQUIRE(out.get(b) == simdjson::SUCCESS);
        CHECK_MESSAGE(a == b, where << ": " << a << " vs " << b);
        return;
    }
    CHECK_MESSAGE(simdjson::minify(ref) == simdjson::minify(out),
                  where << ": " << simdjson::minify(ref) << " vs "
                        << simdjson::minify(out));
}

void compare_metric(const Elem& ref_e, const Elem& out_e, const String& where) {
    const Fields ref = fields_of(ref_e, where);
    const Fields out = fields_of(out_e, where);
    compare_key_sets(ref, out, where);
    bool known = false;
    const MetricKind kind = metric_kind(string_field(ref, "name"), known);
    CHECK_MESSAGE(known, where << ": metric without a rule");
    const double tol = kind == MetricKind::Exact    ? TOL_EXACT
                       : kind == MetricKind::Moment ? TOL_MOMENT
                                                    : TOL_QUANTILE;
    // The operands and what is derived from them. The delta of two
    // quantiles is off by the error of each, and a percentage by the error of
    // the ratio, so those bounds are in terms of the operands.
    const double scale = std::max({std::fabs(number_field(ref, "baseline")),
                                   std::fabs(number_field(ref, "variant")),
                                   std::fabs(number_field(out, "baseline")),
                                   std::fabs(number_field(out, "variant"))});
    for (const auto& [key, value] : ref) {
        const auto it = out.find(key);
        if (it == out.end()) continue;
        const String field = where + "." + key;
        const double a = number_field(ref, key);
        const double b = number_field(out, key);
        if (key == "baseline" || key == "variant") {
            CHECK_MESSAGE(rel_close(a, b, tol),
                          field << ": " << a << " vs " << b);
        } else if (key == "delta") {
            CHECK_MESSAGE(std::fabs(a - b) <= 2 * tol * scale,
                          field << ": " << a << " vs " << b);
        } else if (key == "pct_change") {
            if (kind == MetricKind::Quantile) {
                CHECK_MESSAGE(
                    std::fabs(a - b) <=
                        2.1 * TOL_QUANTILE *
                            (100 + std::max(std::fabs(a), std::fabs(b))),
                    field << ": " << a << " vs " << b);
            } else {
                CHECK_MESSAGE(rel_close(a, b, TOL_MOMENT),
                              field << ": " << a << " vs " << b);
            }
        } else if (key == "cohens_d") {
            CHECK_MESSAGE(rel_close(a, b, TOL_MOMENT),
                          field << ": " << a << " vs " << b);
        } else if (key == "is_regression" && kind == MetricKind::Quantile &&
                   (std::fabs(number_field(ref, "pct_change")) < 5 ||
                    std::fabs(number_field(out, "pct_change")) < 5)) {
            // The sign of a change within the sketch error is not stable.
        } else {
            compare_exact(value, it->second, field);
        }
    }
}

// Entries of `array` keyed by `id_key`; the order of the array is not compared.
void compare_named(const Elem& ref_e, const Elem& out_e, StringView id_key,
                   const String& where, EachFn each) {
    Map<String, Elem> ref, out;
    for (const auto& item : items_of(ref_e, where))
        ref.emplace(string_field(fields_of(item, where), id_key), item);
    for (const auto& item : items_of(out_e, where))
        out.emplace(string_field(fields_of(item, where), id_key), item);
    for (const auto& [id, item] : ref) {
        const auto it = out.find(id);
        CHECK_MESSAGE(it != out.end(), where << ": missing in output: " << id);
        if (it != out.end()) each(item, it->second, where + "/" + id);
    }
    for (const auto& entry : out)
        CHECK_MESSAGE(ref.count(entry.first) != 0,
                      where << ": not in reference: " << entry.first);
}

void compare_metrics(const Elem& ref, const Elem& out, const String& where) {
    compare_named(ref, out, "name", where + ".metrics", compare_metric);
}

void compare_node(const Elem& ref_e, const Elem& out_e, const String& where) {
    const Fields ref = fields_of(ref_e, where);
    const Fields out = fields_of(out_e, where);
    compare_key_sets(ref, out, where);
    if (ref.size() != out.size()) return;
    CHECK_MESSAGE(string_field(ref, "name") == string_field(out, "name"),
                  where << ".name");
    CHECK_MESSAGE(string_field(ref, "duql") == string_field(out, "duql"),
                  where << ".duql");
    compare_metrics(fields_of(ref.at("summary"), where).at("metrics"),
                    fields_of(out.at("summary"), where).at("metrics"),
                    where + ".summary");
    compare_named(ref.at("groups"), out.at("groups"), "label",
                  where + ".groups",
                  [](const Elem& r, const Elem& o, const String& w) {
                      const Fields rf = fields_of(r, w);
                      const Fields of = fields_of(o, w);
                      compare_key_sets(rf, of, w);
                      compare_metrics(rf.at("metrics"), of.at("metrics"), w);
                  });
    compare_named(ref.at("children"), out.at("children"), "name",
                  where + ".children", compare_node);
}

void compare_report(StringView ref_text, StringView out_text) {
    simdjson::dom::parser ref_parser, out_parser;
    Elem ref_root, out_root;
    REQUIRE(ref_parser.parse(ref_text).get(ref_root) == simdjson::SUCCESS);
    REQUIRE(out_parser.parse(out_text).get(out_root) == simdjson::SUCCESS);
    const Fields ref = fields_of(ref_root, "report");
    const Fields out = fields_of(out_root, "report");
    compare_key_sets(ref, out, "report");
    for (const char* key : {"baseline", "variant"})
        if (ref.count(key) && out.count(key))
            compare_exact(ref.at(key), out.at(key), key);
    for (const char* key : {"baseline_meta", "variant_meta"}) {
        if (!ref.count(key) || !out.count(key)) continue;
        const Fields rm = fields_of(ref.at(key), key);
        const Fields om = fields_of(out.at(key), key);
        compare_key_sets(rm, om, key);
        for (const auto& [k, v] : rm)
            if (om.count(k)) compare_exact(v, om.at(k), String(key) + "." + k);
    }
    if (ref.count("nodes") && out.count("nodes"))
        compare_named(ref.at("nodes"), out.at("nodes"), "name", "nodes",
                      compare_node);
}

// ---------------------------------------------------------------------------
// DLIO config
// ---------------------------------------------------------------------------

// The YAML config keeps its line order. Keys and text are exact; numbers come
// from sampled quantiles and fitted mixtures, so they get the sketch accuracy.
void compare_yaml(StringView ref_text, StringView out_text) {
    const auto ref = split(ref_text, '\n');
    const auto out = split(out_text, '\n');
    REQUIRE(ref.size() == out.size());
    for (std::size_t i = 0; i < ref.size(); ++i) {
        const auto rp = ref[i].find(": ");
        const auto op = out[i].find(": ");
        INFO("line " << i + 1 << ": " << ref[i] << " vs " << out[i]);
        if (rp == String::npos || op == String::npos) {
            CHECK(ref[i] == out[i]);
            continue;
        }
        CHECK(ref[i].substr(0, rp) == out[i].substr(0, op));
        const String rv = ref[i].substr(rp + 2);
        const String ov = out[i].substr(op + 2);
        char* rend = nullptr;
        char* oend = nullptr;
        const double x = std::strtod(rv.c_str(), &rend);
        const double y = std::strtod(ov.c_str(), &oend);
        const bool rnum = !rv.empty() && *rend == '\0';
        const bool onum = !ov.empty() && *oend == '\0';
        CHECK(rnum == onum);
        if (rnum && onum)
            CHECK(rel_close(x, y, TOL_QUANTILE));
        else
            CHECK(rv == ov);
    }
}

}  // namespace

TEST_SUITE("AggParity") {
    TEST_CASE("view aggregate tables") {
        Fixtures& fx = fixtures();
        const Vector<ViewQuery> queries = view_queries();
        for (const auto& q : queries) {
            SUBCASE(q.file) {
                View base = View::from_files(view_files(fx.baseline_files));
                const df::DataFrame t = q.build(base).collect().get();
                const String produced = table_text(t, q.keys);
                const String ref = reference(q.file, produced);
                if (!ref.empty()) compare_table(q.file, ref, produced, q.keys);
            }
        }
    }

    TEST_CASE("comparator report") {
        struct Run {
            const char* file;
            std::vector<std::string> args;
        };
        const Run runs[] = {
            {"comparator_5s.json", {}},
            {"comparator_1s.json", {"--time-interval", "1s"}},
            {"comparator_posix_2s.json",
             {"--duql", R"(cat == "POSIX")", "--time-interval", "2s"}},
            {"comparator_dlio_preset.json", {"--preset", "dlio"}}};
        for (const auto& run : runs) {
            SUBCASE(run.file) {
                const String produced = run_comparator(run.args);
                const String ref = reference(run.file, produced);
                if (!ref.empty()) compare_report(ref, produced);
            }
        }
    }

    TEST_CASE("dlio config") {
        const std::string binary = dftu_utils_test::find_binary_by_name(
            "DFTRACER_UTILS_GEN_DLIO_CONFIG_PATH", "dftracer_gen_dlio_config");
        REQUIRE(!binary.empty());
        const std::string out = fixtures().root.str() + "/dlio_config.yaml";
        REQUIRE(dftu_utils_test::run_process(
                    binary, {"-d", fixtures().dlio_dir, "-o", out,
                             "--simulation-iterations", "2"}) == 0);
        const String produced = read_text(out);
        const String ref = reference("dlio_config.yaml", produced);
        if (!ref.empty()) compare_yaml(ref, produced);
    }
}
