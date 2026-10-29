// The per-chunk numeric columns of the aggregation tier: that the codec gives
// back every value bit for bit, that a chunk block holds the chunk's records
// exactly, that the byte budget drops whole columns and lists them, and that a
// sharded merge copies the blocks.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/chunk_columns.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/sharded_view.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dftracer::utils;
namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace views = dftracer::utils::trace::views;
using index::Indexer;
using index::IndexerOptions;
using index::store::IndexDatabase;

namespace {

constexpr std::size_t CHECKPOINT_SIZE = 4096;
constexpr int RECORDS = 120;

// ---- the codec ------------------------------------------------------------

agg::ChunkColumn column_of(std::uint32_t path, agg::ColumnKind kind,
                           const std::vector<std::uint64_t>& rows,
                           const std::vector<std::uint64_t>& bits) {
    agg::ChunkColumn column;
    column.path = path;
    column.kind = kind;
    column.rows.assign(rows.begin(), rows.end());
    column.bits.assign(bits.begin(), bits.end());
    return column;
}

// `n` distinct integers from `first` on, so a column of them does not compress
// to nothing.
std::vector<std::uint64_t> ints(std::size_t n, std::int64_t first) {
    std::vector<std::uint64_t> out;
    for (std::size_t k = 0; k < n; ++k)
        out.push_back(static_cast<std::uint64_t>(
            first + static_cast<std::int64_t>(k) * 7));
    return out;
}

// A block of `rows` rows whose core columns differ row by row and whose ids
// name three dictionary entries.
agg::ChunkColumns core_block(std::size_t rows) {
    agg::ChunkColumns block;
    block.rows = rows;
    block.ids.assign({4, 7, 9});
    for (std::size_t r = 0; r < rows; ++r) {
        block.ts.push_back(1000 + r * 37);
        block.dur.push_back(5 + r * 3);
        block.has_dur.push_back(r % 3 == 1 ? 0 : 1);
        block.pid.push_back(10 + r % 2);
        block.tid.push_back(20 + r % 3);
        block.phase.push_back(static_cast<std::uint8_t>(r % 4));
        block.cat.push_back(r < rows / 2 ? 0 : 1);
        block.name.push_back(2);
        block.host.push_back(static_cast<std::uint32_t>(r % 3));
    }
    return block;
}

void check_core_columns(const agg::ChunkColumns& want,
                        const agg::ChunkColumns& got) {
    REQUIRE(got.rows == want.rows);
    REQUIRE(got.ids == want.ids);
    CHECK(got.ts == want.ts);
    CHECK(got.has_dur == want.has_dur);
    for (std::size_t r = 0; r < want.rows; ++r)
        if (want.has_dur[r] != 0) CHECK(got.dur[r] == want.dur[r]);
    CHECK(got.pid == want.pid);
    CHECK(got.tid == want.tid);
    CHECK(got.phase == want.phase);
    CHECK(got.cat == want.cat);
    CHECK(got.name == want.name);
    CHECK(got.host == want.host);
}

agg::ChunkColumns encode_then_decode(
    const agg::ChunkColumns& block, std::size_t budget = agg::NO_COLUMN_BUDGET,
    std::size_t column_count = std::numeric_limits<std::size_t>::max()) {
    dstd::Vector<std::uint8_t> bytes;
    const std::size_t count =
        column_count == std::numeric_limits<std::size_t>::max()
            ? block.columns.size()
            : column_count;
    agg::encode_chunk_columns(block, count, bytes, budget);
    auto got = agg::decode_chunk_columns(
        dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()));
    REQUIRE(got.has_value());
    return *got;
}

std::uint64_t double_bits_of(double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return bits;
}

bool same(const dstd::Vector<std::uint64_t>& got,
          const std::vector<std::uint64_t>& want) {
    return got.size() == want.size() &&
           std::equal(got.begin(), got.end(), want.begin());
}

// One column of `values` (reinterpreted as the bits of `kind`) on the rows
// `rows` of a block of `block_rows` rows, encoded and decoded again.
agg::ChunkColumn roundtrip_column(agg::ColumnKind kind,
                                  const std::vector<std::uint64_t>& bits,
                                  const std::vector<std::uint64_t>& rows,
                                  std::size_t block_rows) {
    auto block = core_block(block_rows);
    CHECK(rows.size() == bits.size());
    block.columns.push_back(column_of(1, kind, rows, bits));
    const auto got = encode_then_decode(block);
    check_core_columns(block, got);
    REQUIRE(got.columns.size() == 1);
    CHECK(got.columns[0].path == 1);
    CHECK(got.columns[0].kind == kind);
    CHECK(got.columns[0].rows == block.columns[0].rows);
    CHECK(same(got.columns[0].bits, bits));
    CHECK(got.dropped.empty());
    return got.columns[0];
}

// ---- the fixture trace ----------------------------------------------------

enum class NumKind : std::uint8_t { Int64, Uint64, Double };

struct Num {
    NumKind kind = NumKind::Double;
    std::int64_t i = 0;
    std::uint64_t u = 0;
    double d = 0;

    std::uint64_t bits() const {
        switch (kind) {
            case NumKind::Int64:
                return static_cast<std::uint64_t>(i);
            case NumKind::Uint64:
                return u;
            default:
                return double_bits_of(d);
        }
    }
    agg::ColumnKind column_kind() const {
        switch (kind) {
            case NumKind::Int64:
                return agg::ColumnKind::Int64;
            case NumKind::Uint64:
                return agg::ColumnKind::Uint64;
            default:
                return agg::ColumnKind::Double;
        }
    }
};

struct Row {
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;
    bool has_dur = false;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::uint8_t phase = 0;
    std::string cat;
    std::string name;
    std::string host;
    std::map<std::string, Num, std::less<>> args;
};

std::string_view quoted(const std::string& text, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\":\"";
    const auto at = text.find(pattern);
    if (at == std::string::npos) return {};
    const auto from = at + pattern.size();
    const auto to = text.find('"', from);
    if (to == std::string::npos) return {};
    return std::string_view(text).substr(from, to - from);
}

bool number(const std::string& text, std::string_view key, std::uint64_t& out) {
    const std::string pattern = "\"" + std::string(key) + "\":";
    const auto at = text.find(pattern);
    if (at == std::string::npos) return false;
    const char* from = text.c_str() + at + pattern.size();
    char* end = nullptr;
    const auto value = std::strtoull(from, &end, 10);
    if (end == from) return false;
    out = value;
    return true;
}

std::uint8_t phase_of(std::string_view letter) {
    if (letter == "X") return 1;
    if (letter == "C") return 2;
    if (letter == "A") return 3;
    if (letter == "M") return 4;
    return 0;
}

Num classify(const std::string& text) {
    Num n;
    if (text.find_first_of(".eE") != std::string::npos) {
        n.kind = NumKind::Double;
        n.d = std::strtod(text.c_str(), nullptr);
    } else if (!text.empty() && text[0] == '-') {
        n.kind = NumKind::Int64;
        n.i = std::strtoll(text.c_str(), nullptr, 10);
    } else {
        const auto u = std::strtoull(text.c_str(), nullptr, 10);
        const auto max = static_cast<unsigned long long>(
            std::numeric_limits<std::int64_t>::max());
        if (u > max) {
            n.kind = NumKind::Uint64;
            n.u = u;
        } else {
            n.kind = NumKind::Int64;
            n.i = static_cast<std::int64_t>(u);
        }
    }
    return n;
}

std::string trimmed(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

Row parse_row(const std::string& line) {
    Row row;
    std::string head = line;
    std::string args;
    const auto args_at = head.find("\"args\":{");
    if (args_at != std::string::npos) {
        const auto close = head.find('}', args_at);
        REQUIRE(close != std::string::npos);
        args = head.substr(args_at + 8, close - args_at - 8);
        head.erase(args_at, close + 1 - args_at);
    }
    row.cat = std::string(quoted(head, "cat"));
    row.name = std::string(quoted(head, "name"));
    row.phase = phase_of(quoted(head, "ph"));
    row.host = std::string(quoted(args, "hhash"));
    number(head, "pid", row.pid);
    number(head, "tid", row.tid);
    number(head, "ts", row.ts);
    row.has_dur = number(head, "dur", row.dur);

    std::size_t at = 0;
    while ((at = args.find('"', at)) != std::string::npos) {
        const auto key_end = args.find('"', at + 1);
        REQUIRE(key_end != std::string::npos);
        const std::string key = args.substr(at + 1, key_end - at - 1);
        const auto value_at = key_end + 2;
        REQUIRE(value_at < args.size());
        if (args[value_at] == '"') {
            const auto str_end = args.find('"', value_at + 1);
            REQUIRE(str_end != std::string::npos);
            at = str_end + 1;
            continue;
        }
        auto value_end = args.find(',', value_at);
        if (value_end == std::string::npos) value_end = args.size();
        row.args.emplace(
            key,
            classify(trimmed(args.substr(value_at, value_end - value_at))));
        at = value_end;
    }
    return row;
}

// Every record of the trace text that is not metadata, in file order.
std::vector<Row> parse_trace(const std::string& text) {
    std::vector<Row> rows;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string body = trimmed(line);
        if (body.empty() || body[0] == '[' || body[0] == ']') continue;
        Row row = parse_row(body);
        if (row.phase == 4) continue;
        rows.push_back(std::move(row));
    }
    return rows;
}

// The fixture: X events, counters of the application and of the system, A
// records and metadata records, with integer, fractional and huge arguments.
// The categories keep their case, so a block that lower-cases one is seen.
std::string make_trace(int n) {
    std::ostringstream text;
    text << "[\n";
    for (int i = 0; i < n; ++i) {
        const int pid = 1 + i % 3;
        const int tid = 10 + i % 2;
        const std::uint64_t ts = 1000000 + static_cast<std::uint64_t>(i) * 997;
        if (i % 20 == 19) {
            text << R"({"id":)" << i << R"(,"name":"meta","cat":"dftracer")"
                 << R"(,"ph":"M","ts":)" << ts
                 << R"(,"args":{"version":"1.0"}})" << "\n";
            continue;
        }
        switch (i % 5) {
            case 0:
            case 1:
                text << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX")"
                     << R"(,"pid":)" << pid << R"(,"tid":)" << tid
                     << R"(,"ph":"X","ts":)" << ts << R"(,"dur":)" << i * 3 % 71
                     << R"(,"args":{"ret":)" << (i * 13) % 4096
                     << R"(,"offset":)" << i * 512
                     << R"(,"hhash":"a1b2c3d4e5f60718")"
                     << R"(,"fhash":"0123456789abcdef"}})" << "\n";
                break;
            case 2:
                text << R"({"id":)" << i << R"(,"name":"app","cat":"App")"
                     << R"(,"pid":)" << pid << R"(,"tid":)" << tid
                     << R"(,"ph":"C","ts":)" << ts
                     << R"(,"args":{"dur":0.25,"user_pct":41.25,"n":)" << i % 7
                     << R"(,"state":"r"}})" << "\n";
                break;
            case 3:
                text << R"({"id":)" << i << R"(,"name":"cpu","cat":"sys")"
                     << R"(,"pid":)" << pid << R"(,"tid":)" << tid
                     << R"(,"ph":"C","ts":)" << ts
                     << R"(,"args":{"user_pct":41.25,"sys_pct":8.5)"
                     << R"(,"mem":123456789,"level":)" << i % 4 << "}}\n";
                break;
            default:
                text << R"({"id":)" << i << R"(,"name":"step","cat":"App")"
                     << R"(,"pid":)" << pid << R"(,"tid":)" << tid
                     << R"(,"ph":"A","ts":)" << ts << R"(,"dur":)" << i % 11
                     << R"(,"args":{"offset":)" << 18446744073709551615ULL
                     << R"(,"delta":-)" << i % 9 << R"(,"count":)" << i * 0.5
                     << "}}\n";
                break;
        }
    }
    text << "]\n";
    return text.str();
}

// A trace of its own in `dir`/`name`, so each one's sidecar index is distinct.
std::string write_trace(const std::string& dir, const std::string& name,
                        std::string& text) {
    text = make_trace(RECORDS);
    const std::string sub = dir + "/" + name;
    fs::create_directories(sub);
    const std::string pfw = sub + "/trace.pfw";
    {
        std::ofstream file(pfw);
        file << text;
    }
    const std::string gz = pfw + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
        pfw, gz, CHECKPOINT_SIZE));
    fs::remove(pfw);
    return gz;
}

IndexerOptions options(const std::string& index_dir, double ratio) {
    IndexerOptions o;
    o.index_dir = index_dir;
    o.checkpoint_size = CHECKPOINT_SIZE;
    o.parallelism = 4;
    agg::AggregationConfig c;
    c.time_interval_us = 100'000;
    c.column_budget_ratio = ratio;
    o.aggregation = c;
    return o;
}

void build(const std::vector<std::string>& files, const std::string& index_dir,
           double ratio) {
    Indexer::open(files, options(index_dir, ratio)).build();
}

// ---- the comparison -------------------------------------------------------

void check_row_core(const agg::ChunkColumns& block, std::size_t r,
                    const Row& row) {
    CHECK(block.ts[r] == row.ts);
    CHECK(block.has_dur[r] == (row.has_dur ? 1 : 0));
    CHECK(block.dur[r] == row.dur);
    CHECK(block.pid[r] == row.pid);
    CHECK(block.tid[r] == row.tid);
    CHECK(block.phase[r] == row.phase);
    REQUIRE(block.cat[r] < block.texts.size());
    REQUIRE(block.name[r] < block.texts.size());
    REQUIRE(block.host[r] < block.texts.size());
    CHECK(block.texts[block.cat[r]] == row.cat);
    CHECK(block.texts[block.name[r]] == row.name);
    CHECK(block.texts[block.host[r]] == row.host);
}

// The column of `path` of the kind `kind`: one arg path holds one column per
// kind its values take.
const agg::ChunkColumn* column_for(const agg::ChunkColumns& block,
                                   std::string_view path,
                                   agg::ColumnKind kind) {
    for (const agg::ChunkColumn& column : block.columns)
        if (column.kind == kind && block.texts[column.path] == path)
            return &column;
    return nullptr;
}

bool is_dropped(const agg::ChunkColumns& block, std::string_view path) {
    for (const std::uint32_t path_index : block.dropped)
        if (block.texts[path_index] == path) return true;
    return false;
}

// The value of every arg of `row` is on row r of `block`: with `strict` every
// one has its column of its kind and the block drops no path at all, else a
// missing column is a path the block lists as dropped.
void check_row_args(const agg::ChunkColumns& block, std::size_t r,
                    const Row& row, bool strict) {
    for (const auto& [path, value] : row.args) {
        const agg::ChunkColumn* column =
            column_for(block, path, value.column_kind());
        if (column == nullptr) {
            if (strict) {
                CHECK_MESSAGE(is_dropped(block, path) == false, "dropped ",
                              path, " of row ", r);
                CHECK_MESSAGE(false, "no column for ", path, " on row ", r);
            } else {
                CHECK_MESSAGE(is_dropped(block, path), "no column for ", path,
                              " on row ", r);
            }
            continue;
        }
        const auto at =
            std::lower_bound(column->rows.begin(), column->rows.end(),
                             static_cast<std::uint64_t>(r));
        REQUIRE(at != column->rows.end());
        REQUIRE_MESSAGE(*at == static_cast<std::uint64_t>(r), path, " on row ",
                        r);
        const auto k = static_cast<std::size_t>(at - column->rows.begin());
        CHECK(column->kind == value.column_kind());
        CHECK(column->bits[k] == value.bits());
    }
}

// Every value of every column of `block` is the one its record held, on the
// same row.
void check_columns_against(const agg::ChunkColumns& block,
                           const std::vector<Row>& rows, std::size_t at) {
    for (const agg::ChunkColumn& column : block.columns) {
        const std::string& path = block.texts[column.path];
        CHECK_FALSE(path.empty());
        CHECK(column.rows.size() == column.bits.size());
        CHECK(column.rows.size() <= block.rows);
        CHECK(std::is_sorted(column.rows.begin(), column.rows.end()));
        for (std::size_t k = 0; k < column.rows.size(); ++k) {
            REQUIRE(at + column.rows[k] < rows.size());
            const Row& row = rows[at + column.rows[k]];
            const auto found = row.args.find(path);
            REQUIRE_MESSAGE(found != row.args.end(), "no arg ", path,
                            " on row ", column.rows[k]);
            CHECK(column.kind == found->second.column_kind());
            CHECK(column.bits[k] == found->second.bits());
        }
    }
}

struct Decoded {
    std::size_t blocks = 0;
    std::size_t rows = 0;
    std::size_t dropped_blocks = 0;
    std::vector<std::uint64_t> chunks;
};

// Reads every block of `file_id` and holds it against the records of `rows`:
// with `strict` every numeric arg of every record has its column, no block
// drops a path, and a block whose records hold args has columns.
Decoded read_and_check(const IndexDatabase& db, int file_id,
                       const std::vector<Row>& rows, bool strict) {
    Decoded out;
    std::size_t at = 0;
    agg::tier::for_each_chunk_columns(
        db, file_id, [&](std::uint64_t chunk, const agg::ChunkColumns& block) {
            ++out.blocks;
            out.chunks.push_back(chunk);
            CHECK(block.chunk == chunk);
            CHECK(block.texts.size() == block.ids.size());
            for (const std::uint32_t path_index : block.dropped)
                CHECK_FALSE(block.texts[path_index].empty());
            if (strict) CHECK(block.dropped.empty());
            check_columns_against(block, rows, at);
            std::size_t args = 0;
            for (std::size_t r = 0; r < block.rows; ++r) {
                REQUIRE(at < rows.size());
                check_row_core(block, r, rows[at]);
                check_row_args(block, r, rows[at], strict);
                args += rows[at].args.size();
                ++at;
                ++out.rows;
            }
            if (strict) {
                CHECK(args > 0);
                CHECK_FALSE(block.columns.empty());
            } else if (args > 0 && !block.dropped.empty()) {
                ++out.dropped_blocks;
            }
            return true;
        });
    CHECK(at == rows.size());
    CHECK(std::is_sorted(out.chunks.begin(), out.chunks.end()));
    CHECK(std::adjacent_find(out.chunks.begin(), out.chunks.end()) ==
          out.chunks.end());
    return out;
}

int file_id_of(const IndexDatabase& db, const std::string& path) {
    const int id =
        db.get_file_info_id(index::store::internal::get_logical_path(path));
    REQUIRE(id >= 0);
    return id;
}

// One column of a block, named by the text of its path and holding its rows
// and values, so two indexes are compared without their block-local ids.
struct ColumnShape {
    std::string path;
    agg::ColumnKind kind = agg::ColumnKind::Int64;
    std::vector<std::uint64_t> rows;
    std::vector<std::uint64_t> bits;

    bool operator==(const ColumnShape&) const = default;
};

std::map<std::uint64_t, std::vector<ColumnShape>> columns_of(
    const IndexDatabase& db, int file_id) {
    std::map<std::uint64_t, std::vector<ColumnShape>> out;
    agg::tier::for_each_chunk_columns(
        db, file_id, [&](std::uint64_t chunk, const agg::ChunkColumns& block) {
            std::vector<ColumnShape> shapes;
            for (const agg::ChunkColumn& column : block.columns)
                shapes.push_back({block.texts[column.path], column.kind,
                                  std::vector<std::uint64_t>(
                                      column.rows.begin(), column.rows.end()),
                                  std::vector<std::uint64_t>(
                                      column.bits.begin(), column.bits.end())});
            out[chunk] = shapes;
            return true;
        });
    return out;
}

}  // namespace

TEST_SUITE("ChunkColumns") {
    TEST_CASE("integers round trip bit exact") {
        roundtrip_column(
            agg::ColumnKind::Int64,
            {static_cast<std::uint64_t>(std::int64_t{-1}),
             static_cast<std::uint64_t>(std::int64_t{0}),
             static_cast<std::uint64_t>(std::int64_t{-9007199254740993LL}),
             static_cast<std::uint64_t>(
                 std::numeric_limits<std::int64_t>::min()),
             static_cast<std::uint64_t>(
                 std::numeric_limits<std::int64_t>::max()),
             static_cast<std::uint64_t>(std::int64_t{7})},
            {0, 1, 2, 3, 4, 5}, 6);
    }

    TEST_CASE("a uint64 above INT64_MAX round trips bit exact") {
        roundtrip_column(agg::ColumnKind::Uint64,
                         {0, std::numeric_limits<std::uint64_t>::max(),
                          std::uint64_t{1} << 63, 42},
                         {0, 1, 2, 3}, 4);
    }

    TEST_CASE("decimals round trip bit exact") {
        const std::vector<double> values = {
            0.1, 12.5, 1e-7, 123456.789, -0.0, 1e300, 5e-324, 3.14159265358979};
        std::vector<std::uint64_t> bits;
        for (const double v : values) bits.push_back(double_bits_of(v));
        const auto column = roundtrip_column(agg::ColumnKind::Double, bits,
                                             {0, 1, 2, 3, 4, 5, 6, 7}, 8);
        for (std::size_t k = 0; k < values.size(); ++k)
            CHECK(double_bits_of(column.double_at(k)) == bits[k]);
    }

    TEST_CASE("values of mixed exponents round trip bit exact") {
        const std::vector<double> values = {0.1,   1,      1000, 0.001, 2.5,
                                            1e-12, 1234.5, 7,    0.5,   1e12};
        std::vector<std::uint64_t> bits;
        for (const double v : values) bits.push_back(double_bits_of(v));
        roundtrip_column(agg::ColumnKind::Double, bits,
                         {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, 10);
    }

    TEST_CASE("NaN, its payload and the infinities round trip bit exact") {
        const std::vector<std::uint64_t> bits = {
            0x7ff8000000000001ULL,
            0xfff0000000000000ULL,
            0x7ff0000000000000ULL,
            double_bits_of(-1.5),
        };
        roundtrip_column(agg::ColumnKind::Double, bits, {0, 1, 2, 3}, 4);
    }

    TEST_CASE("random doubles round trip bit exact") {
        std::uint64_t state = 0x2545f4914f6cdd1dULL;
        const auto next = [&state] {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        };
        std::vector<std::uint64_t> bits;
        std::vector<std::uint64_t> rows;
        for (std::uint64_t k = 0; k < 300; ++k) {
            const std::uint64_t exponent = next() % 0x7fe + 1;
            const std::uint64_t mantissa = next() & 0x000fffffffffffffULL;
            bits.push_back((next() & 1ULL ? 1ULL << 63 : 0) | (exponent << 52) |
                           mantissa);
            rows.push_back(k);
        }
        roundtrip_column(agg::ColumnKind::Double, bits, rows,
                         static_cast<std::size_t>(rows.size()));
    }

    TEST_CASE("a column of one row and a column with gaps round trip") {
        roundtrip_column(agg::ColumnKind::Int64,
                         {static_cast<std::uint64_t>(std::int64_t{-3})}, {4},
                         9);
        roundtrip_column(
            agg::ColumnKind::Double,
            {double_bits_of(0.25), double_bits_of(41.25), double_bits_of(8.5)},
            {0, 5, 8}, 9);
    }

    TEST_CASE("a block with no column holds its core columns") {
        auto block = core_block(7);
        const auto got = encode_then_decode(block);
        check_core_columns(block, got);
        CHECK(got.columns.empty());
        CHECK(got.dropped.empty());
    }

    TEST_CASE("several columns of one path of different kinds round trip") {
        auto block = core_block(4);
        block.columns.push_back(
            column_of(1, agg::ColumnKind::Int64, {0, 2},
                      {static_cast<std::uint64_t>(std::int64_t{-9}),
                       static_cast<std::uint64_t>(std::int64_t{11})}));
        block.columns.push_back(
            column_of(1, agg::ColumnKind::Double, {1, 3},
                      {double_bits_of(0.5), double_bits_of(2.25)}));
        block.columns.push_back(
            column_of(2, agg::ColumnKind::Uint64, {0, 1, 2, 3},
                      {std::numeric_limits<std::uint64_t>::max(), 1, 2, 3}));
        const auto got = encode_then_decode(block);
        check_core_columns(block, got);
        REQUIRE(got.columns.size() == 3);
        for (std::size_t c = 0; c < got.columns.size(); ++c) {
            CHECK(got.columns[c].path == block.columns[c].path);
            CHECK(got.columns[c].kind == block.columns[c].kind);
            CHECK(got.columns[c].rows == block.columns[c].rows);
            CHECK(got.columns[c].bits == block.columns[c].bits);
        }
    }

    TEST_CASE("a small budget drops the columns with the fewest rows first") {
        auto block = core_block(20);
        block.ids.assign({10, 12, 15, 20});
        const std::uint32_t ROWS_16[] = {0, 1, 2,  3,  4,  5,  6,  7,
                                         8, 9, 10, 11, 12, 13, 14, 15};
        const std::uint32_t ROWS_8[] = {0, 2, 4, 6, 8, 10, 12, 14};
        const std::uint32_t ROWS_3_A[] = {1, 5, 9};
        const std::uint32_t ROWS_3_B[] = {2, 6, 10};
        const auto as_rows = [](const std::uint32_t* rows, std::size_t n) {
            return std::vector<std::uint64_t>(rows, rows + n);
        };
        block.columns.push_back(column_of(0, agg::ColumnKind::Int64,
                                          as_rows(ROWS_16, 16), ints(16, 100)));
        block.columns.push_back(column_of(1, agg::ColumnKind::Int64,
                                          as_rows(ROWS_8, 8), ints(8, 100)));
        block.columns.push_back(column_of(2, agg::ColumnKind::Int64,
                                          as_rows(ROWS_3_A, 3), ints(3, 100)));
        block.columns.push_back(column_of(3, agg::ColumnKind::Int64,
                                          as_rows(ROWS_3_B, 3), ints(3, 100)));
        // The two columns of three rows encode to the same bytes, so only the
        // path id of their column decides which one goes first.
        REQUIRE(block.columns[2].rows.size() == block.columns[3].rows.size());

        struct State {
            std::vector<std::uint32_t> kept;
            std::vector<std::uint32_t> dropped;
            bool operator==(const State&) const = default;
        };
        const auto state_at = [&block](std::size_t budget) {
            const auto got = encode_then_decode(block, budget);
            State state;
            for (const agg::ChunkColumn& column : got.columns)
                state.kept.push_back(column.path);
            state.dropped.assign(got.dropped.begin(), got.dropped.end());
            return std::make_pair(state, got);
        };

        dstd::Vector<std::uint8_t> bytes;
        agg::encode_chunk_columns(block, block.columns.size(), bytes,
                                  agg::NO_COLUMN_BUDGET);
        const std::size_t full = bytes.size();
        std::vector<State> states;
        for (std::size_t budget = 0; budget <= full; ++budget) {
            auto [state, got] = state_at(budget);
            for (const std::uint32_t path : state.dropped)
                CHECK(std::find(state.kept.begin(), state.kept.end(), path) ==
                      state.kept.end());
            if (!states.empty() && states.back() == state) continue;
            // Every state holds the core columns whatever it kept.
            check_core_columns(block, got);
            states.push_back(state);
        }
        // One state per column dropped, so the budget takes them one at a time,
        // and one with none left.
        REQUIRE(states.size() == block.columns.size() + 1);
        CHECK(states.front().kept.empty());
        CHECK(states.back().kept == std::vector<std::uint32_t>{0, 1, 2, 3});
        CHECK(states.back().dropped.empty());

        // As the budget grows the columns come back from the one with the most
        // rows to the one with the fewest, and of the two of three rows the one
        // whose path id is larger comes back first: the other drops first.
        const std::vector<std::vector<std::uint32_t>> kept = {
            {}, {0}, {0, 1}, {0, 1, 3}, {0, 1, 2, 3}};
        const std::vector<std::vector<std::uint32_t>> dropped = {
            {2, 3, 1, 0}, {2, 3, 1}, {2, 3}, {2}, {}};
        for (std::size_t k = 0; k < states.size(); ++k) {
            CAPTURE(k);
            CHECK(states[k].kept == kept[k]);
            CHECK(states[k].dropped == dropped[k]);
        }
    }

    TEST_CASE("a malformed block is rejected") {
        auto block = core_block(3);
        block.columns.push_back(
            column_of(1, agg::ColumnKind::Int64, {0, 1},
                      {1, static_cast<std::uint64_t>(std::int64_t{-1})}));
        dstd::Vector<std::uint8_t> bytes;
        agg::encode_chunk_columns(block, block.columns.size(), bytes,
                                  agg::NO_COLUMN_BUDGET);
        const auto truncate = [&](std::size_t size) {
            auto got = agg::decode_chunk_columns(
                dstd::Span<const std::uint8_t>(bytes.data(), size));
            CHECK_FALSE(got.has_value());
        };
        for (std::size_t size = 0; size < bytes.size(); ++size) truncate(size);
        auto bad = bytes;
        bad[0] = 99;
        CHECK_FALSE(agg::decode_chunk_columns(
                        dstd::Span<const std::uint8_t>(bad.data(), bad.size()))
                        .has_value());
    }

    TEST_CASE("a cat, name or host run above the block's ids is rejected") {
        auto block = core_block(4);
        block.columns.push_back(
            column_of(1, agg::ColumnKind::Int64, {0, 1},
                      {1, static_cast<std::uint64_t>(std::int64_t{-1})}));
        dstd::Vector<std::uint8_t> bytes;
        agg::encode_chunk_columns(block, block.columns.size(), bytes,
                                  agg::NO_COLUMN_BUDGET);
        const std::size_t ids = block.ids.size();
        const std::vector<std::uint8_t> values = {2, 3, 4, 0x7f, 0x80, 0xff};
        std::size_t accepted = 0;
        std::size_t rejected = 0;
        for (std::size_t at = 0; at < bytes.size(); ++at) {
            for (const std::uint8_t value : values) {
                if (bytes[at] == value) continue;
                auto patched = bytes;
                patched[at] = value;
                auto got =
                    agg::decode_chunk_columns(dstd::Span<const std::uint8_t>(
                        patched.data(), patched.size()));
                if (!got) {
                    ++rejected;
                    continue;
                }
                ++accepted;
                for (std::size_t r = 0; r < got->rows; ++r) {
                    REQUIRE_MESSAGE(got->cat[r] < ids, "cat of row ", r,
                                    " from byte ", at, " set to ",
                                    static_cast<int>(value));
                    REQUIRE_MESSAGE(got->name[r] < ids, "name of row ", r,
                                    " from byte ", at, " set to ",
                                    static_cast<int>(value));
                    REQUIRE_MESSAGE(got->host[r] < ids, "host of row ", r,
                                    " from byte ", at, " set to ",
                                    static_cast<int>(value));
                }
            }
        }
        CHECK(accepted > 0);
        CHECK(rejected > 0);
    }

    TEST_CASE("an index stores every numeric field of every chunk") {
        dftu_utils_test::TestEnvironment env(60);
        REQUIRE(env.is_valid());
        std::string text;
        const std::string gz = write_trace(env.get_dir(), "trace", text);
        const auto rows = parse_trace(text);
        REQUIRE(rows.size() > 0);
        const std::string index_dir = env.get_dir() + "/idx";
        build({gz}, index_dir, 1000.0);

        const auto index_path =
            trace::internal::determine_index_path(gz, index_dir);
        IndexDatabase db(index_path, index::store::IndexOpenMode::ReadOnly);
        const int fid = file_id_of(db, gz);
        const auto decoded = read_and_check(db, fid, rows, true);
        CHECK(decoded.blocks > 1);
        CHECK(decoded.rows == rows.size());
        CHECK(decoded.dropped_blocks == 0);
        CHECK(db.query_gzip_members(fid).size() >= decoded.blocks);
    }

    TEST_CASE("the default ratio of the fixture drops every numeric column") {
        dftu_utils_test::TestEnvironment env(60);
        REQUIRE(env.is_valid());
        std::string text;
        const std::string gz = write_trace(env.get_dir(), "trace", text);
        const auto rows = parse_trace(text);
        REQUIRE(rows.size() > 0);
        agg::AggregationConfig config;
        config.time_interval_us = 100'000;
        REQUIRE(config.column_budget_ratio == 0.02);
        const std::string index_dir = env.get_dir() + "/idx";
        {
            IndexerOptions o;
            o.index_dir = index_dir;
            o.checkpoint_size = CHECKPOINT_SIZE;
            o.parallelism = 4;
            o.aggregation = config;
            Indexer::open({gz}, o).build();
        }
        const auto index_path =
            trace::internal::determine_index_path(gz, index_dir);
        IndexDatabase db(index_path, index::store::IndexOpenMode::ReadOnly);
        const int fid = file_id_of(db, gz);

        std::size_t at = 0;
        std::size_t blocks = 0;
        std::size_t dropped_blocks = 0;
        agg::tier::for_each_chunk_columns(
            db, fid, [&](std::uint64_t chunk, const agg::ChunkColumns& block) {
                ++blocks;
                const auto begin =
                    static_cast<std::size_t>(chunk) * CHECKPOINT_SIZE;
                REQUIRE(begin < text.size());
                const std::size_t bytes =
                    std::min(CHECKPOINT_SIZE, text.size() - begin);
                const auto budget = static_cast<std::size_t>(
                    static_cast<double>(bytes) * config.column_budget_ratio);
                // The core columns of the chunk's rows already take more than
                // the budget, so the block cannot keep a numeric column.
                dstd::Vector<std::uint8_t> core;
                agg::encode_chunk_columns(block, 0, core,
                                          agg::NO_COLUMN_BUDGET);
                CHECK(core.size() > budget);
                CHECK(block.columns.empty());
                std::vector<std::string> want;
                for (std::size_t r = 0; r < block.rows; ++r) {
                    REQUIRE(at + r < rows.size());
                    check_row_core(block, r, rows[at + r]);
                    check_row_args(block, r, rows[at + r], false);
                    for (const auto& [path, value] : rows[at + r].args)
                        want.push_back(path);
                }
                std::vector<std::string> got;
                for (const std::uint32_t path : block.dropped)
                    got.push_back(block.texts[path]);
                std::sort(want.begin(), want.end());
                want.erase(std::unique(want.begin(), want.end()), want.end());
                std::sort(got.begin(), got.end());
                // Every arg path of the chunk, and no other, is listed once.
                CHECK(got == want);
                CHECK(block.dropped.size() == got.size());
                dropped_blocks += got.empty() ? 0 : 1;
                at += block.rows;
                return true;
            });
        CHECK(at == rows.size());
        CHECK(blocks > 1);
        CHECK(dropped_blocks == blocks);
    }

    TEST_CASE("merge_shard_set copies the column blocks") {
        dftu_utils_test::TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string text_a;
        std::string text_b;
        const std::string a = write_trace(env.get_dir(), "alpha", text_a);
        const std::string b = write_trace(env.get_dir(), "beta", text_b);
        const auto rows_a = parse_trace(text_a);
        const auto rows_b = parse_trace(text_b);
        const std::string ia = trace::internal::determine_index_path(a, "");
        const std::string ib = trace::internal::determine_index_path(b, "");
        build({a}, "", 1000.0);
        build({b}, "", 1000.0);

        std::map<std::uint64_t, std::vector<ColumnShape>> before_a;
        std::map<std::uint64_t, std::vector<ColumnShape>> before_b;
        {
            IndexDatabase db(ia, index::store::IndexOpenMode::ReadOnly);
            before_a = columns_of(db, file_id_of(db, a));
            IndexDatabase db_b(ib, index::store::IndexOpenMode::ReadOnly);
            before_b = columns_of(db_b, file_id_of(db_b, b));
        }
        REQUIRE_FALSE(before_a.empty());
        REQUIRE_FALSE(before_b.empty());
        for (const auto& [chunk, columns] : before_a)
            CHECK_FALSE(columns.empty());

        const std::string root = env.get_dir() + "/set";
        views::write_shard_set(root, {ia, ib});
        const std::string out = env.get_dir() + "/merged";
        CHECK(views::merge_shard_set(root, out) == 2);

        const std::string merged =
            trace::internal::determine_index_path("x", out);
        IndexDatabase db(merged, index::store::IndexOpenMode::ReadOnly);
        const int merged_a = file_id_of(db, a);
        const int merged_b = file_id_of(db, b);
        const auto decoded_a = read_and_check(db, merged_a, rows_a, true);
        const auto decoded_b = read_and_check(db, merged_b, rows_b, true);
        CHECK(decoded_a.blocks > 1);
        CHECK(decoded_b.blocks > 1);
        // The merged blocks hold the columns of the blocks they came from,
        // path text, rows and values included.
        CHECK(columns_of(db, merged_a) == before_a);
        CHECK(columns_of(db, merged_b) == before_b);
    }
}
