// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/trace/table/table_codec.h>
#include <dftracer/utils/trace/writer/trace_writer.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::table;
using namespace dftu_utils_test;
using dataframe::DataFrame;
using dataframe::Series;
using dataframe::TypeId;

namespace {

constexpr double INF = std::numeric_limits<double>::infinity();
constexpr std::int64_t I64_MIN = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t I64_MAX = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t U64_MAX = std::numeric_limits<std::uint64_t>::max();

dstd::Vector<std::uint8_t> bitmap(dstd::Span<const bool> valid) {
    dstd::Vector<std::uint8_t> bits((valid.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < valid.size(); ++i)
        if (valid[i]) bits[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
    return bits;
}

dstd::String field_json(dstd::StringView name, dstd::StringView dtype,
                        dstd::StringView extra = {}) {
    dstd::String s = "{\"name\":\"";
    s += name;
    s += "\",\"dtype\":\"";
    s += dtype;
    s += "\"";
    if (!extra.empty()) {
        s += ",";
        s += extra;
    }
    s += "}";
    return s;
}

dstd::String header_json(std::size_t nrows,
                         dstd::Span<const dstd::String> fields,
                         dstd::StringView version = "1",
                         dstd::StringView format = "dftu.table") {
    dstd::String s = "{\"format\":\"";
    s += format;
    s += "\",\"format_version\":";
    s += version;
    s += ",\"table\":\"t\",\"nrows\":";
    s += dstd::fmt<"{}">(nrows);
    s += ",\"index\":[],\"index_names\":[],\"fields\":[";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) s += ",";
        s += fields[i];
    }
    s += "],\"meta\":{}}";
    return s;
}

struct Sample {
    DataFrame frame;
    dstd::String header;
};

Sample sample_frame() {
    Sample out;
    const std::int64_t i64[] = {I64_MIN, -1, 0, I64_MAX, 7, 8};
    const bool i64_valid[] = {true, true, true, true, false, true};
    const std::uint64_t u64[] = {0, U64_MAX, 1ull << 63, 5, 6, 7};
    const double f64[] = {1.5, std::nan(""), INF, -INF, 0.0, 0.1};
    const bool f64_valid[] = {true, true, true, true, false, true};
    const std::uint8_t flags = 0b001101;
    const bool b_valid[] = {true, true, true, true, true, false};
    const dstd::StringView s[] = {
        "plain", "q\"uote\\", "line\nbreak\ttab", "\xc3\xa9 \xe4\xb8\xad",
        "",      "null"};
    const bool s_valid[] = {true, true, true, true, true, false};
    const dstd::StringView raw[] = {"[1,2]", "[]",   "[\"a\",{\"x\":null}]",
                                    "[3]",   "null", "[4]"};
    const bool raw_valid[] = {true, true, true, true, false, true};

    auto v_i64 = bitmap(i64_valid);
    auto v_f64 = bitmap(f64_valid);
    auto v_b = bitmap(b_valid);
    auto v_s = bitmap(s_valid);
    auto v_raw = bitmap(raw_valid);
    out.frame.names.push_back(dstd::SharedString("i64"));
    out.frame.columns.push_back(
        Series::flat(TypeId::Int64, i64, 6, v_i64.data()));
    out.frame.names.push_back(dstd::SharedString("u64"));
    out.frame.columns.push_back(Series::flat(TypeId::Uint64, u64, 6));
    out.frame.names.push_back(dstd::SharedString("f64"));
    out.frame.columns.push_back(
        Series::flat(TypeId::Float64, f64, 6, v_f64.data()));
    out.frame.names.push_back(dstd::SharedString("b"));
    out.frame.columns.push_back(
        Series::flat(TypeId::Bool, &flags, 6, v_b.data()));
    out.frame.names.push_back(dstd::SharedString("s"));
    out.frame.columns.push_back(Series::strings(s, v_s.data()));
    out.frame.names.push_back(dstd::SharedString("raw"));
    out.frame.columns.push_back(Series::strings(raw, v_raw.data()));

    const dstd::String fields[] = {
        field_json("i64", "Int64"),
        field_json("u64", "UInt64"),
        field_json("f64", "Float64"),
        field_json("b", "boolean"),
        field_json("s", "string"),
        field_json("raw", "object", "\"elem\":\"set\"")};
    out.header = header_json(6, fields);
    return out;
}

const dstd::StringView RAW_COLUMNS[] = {"raw"};

WriteOptions plain_options() {
    WriteOptions o;
    o.raw_columns = RAW_COLUMNS;
    o.compress = false;
    return o;
}

bool same_column(const Series& a, const Series& b) {
    if (a.type() != b.type() || a.length() != b.length()) return false;
    for (std::int64_t i = 0; i < a.length(); ++i) {
        if (a.is_null(i) != b.is_null(i)) return false;
        if (a.is_null(i)) continue;
        switch (a.type()) {
            case TypeId::Int64:
                if (a.values<std::int64_t>()[static_cast<std::size_t>(i)] !=
                    b.values<std::int64_t>()[static_cast<std::size_t>(i)])
                    return false;
                break;
            case TypeId::Uint64:
                if (a.values<std::uint64_t>()[static_cast<std::size_t>(i)] !=
                    b.values<std::uint64_t>()[static_cast<std::size_t>(i)])
                    return false;
                break;
            case TypeId::Float64:
                if (std::bit_cast<std::uint64_t>(
                        a.values<double>()[static_cast<std::size_t>(i)]) !=
                    std::bit_cast<std::uint64_t>(
                        b.values<double>()[static_cast<std::size_t>(i)]))
                    return false;
                break;
            case TypeId::Bool: {
                const auto bit = [i](const Series& s) {
                    return (s.values<std::uint8_t>()[static_cast<std::size_t>(
                                i / 8)] >>
                            (i % 8)) &
                           1;
                };
                if (bit(a) != bit(b)) return false;
                break;
            }
            case TypeId::String:
                if (a.string_at(i) != b.string_at(i)) return false;
                break;
            default:
                return false;
        }
    }
    return true;
}

bool same_frame(const DataFrame& a, const DataFrame& b) {
    if (a.num_columns() != b.num_columns()) return false;
    for (std::size_t c = 0; c < a.num_columns(); ++c) {
        if (dstd::StringView(a.names[c]) != dstd::StringView(b.names[c]))
            return false;
        if (!same_column(a.columns[c], b.columns[c])) return false;
    }
    return true;
}

void write_text(dstd::StringView path, dstd::StringView text) {
    REQUIRE(dstd::fs::write_file(path, text));
}

dstd::String gzip_members(dstd::StringView text, std::size_t member_bytes) {
    fileio::compress::GzipMemberCompressor comp(6);
    dstd::String all;
    for (std::size_t at = 0; at < text.size(); at += member_bytes) {
        const auto piece = text.substr(at, member_bytes);
        auto member = comp.compress_member(piece.data(), piece.size());
        REQUIRE(member.has_value());
        all.append(reinterpret_cast<const char*>(member->data()),
                   member->size());
    }
    return all;
}

void write_gzip_members(dstd::StringView path, dstd::StringView text,
                        std::size_t member_bytes) {
    write_text(path, gzip_members(text, member_bytes));
}

dstd::StringView first_line(dstd::StringView text) {
    return text.substr(0, text.find('\n'));
}

// The header line for a hand-written file.
dstd::String header_line(dstd::StringView args) {
    dstd::String s =
        "{\"name\":\"dftu.table\",\"cat\":\"dftu\",\"pid\":0,\"tid\":0,"
        "\"ts\":0,\"ph\":4,\"args\":";
    s += args;
    s += "}\n";
    return s;
}

common::ErrorCode code_of(const dstd::Error& e) {
    return common::errorcode_of(e);
}

}  // namespace

TEST_SUITE("trace::table codec") {
    TEST_CASE("every column type round trips, plain and gzip") {
        TestEnvironment env(0);
        auto sample = sample_frame();
        for (const bool compress : {false, true}) {
            const dstd::String path =
                env.dir() + (compress ? "/a.pfw.gz" : "/a.pfw");
            auto opts = plain_options();
            opts.compress = compress;
            REQUIRE(write_table(sample.frame, path, sample.header, opts));
            auto back = dftu_utils_test::ok(read_table(path));
            CHECK(same_frame(sample.frame, back.frame));
            // The writer puts nrows first, in a fixed-width slot.
            dstd::String want = sample.header;
            const dstd::StringView nrows = "\"nrows\":6,";
            want.erase(want.find(nrows), nrows.size());
            want.insert(1, "\"nrows\":6                   ,");
            CHECK(back.header_json == want);
        }
    }

    TEST_CASE("null, NaN and infinity stay apart in one float column") {
        TestEnvironment env(0);
        auto sample = sample_frame();
        const dstd::String path = env.dir() + "/f.pfw";
        REQUIRE(
            write_table(sample.frame, path, sample.header, plain_options()));
        auto back = dftu_utils_test::ok(read_table(path));
        const Series& f = back.frame.columns[2];
        CHECK_FALSE(f.is_null(1));
        CHECK(std::isnan(f.values<double>()[1]));
        CHECK(f.values<double>()[2] == INF);
        CHECK(f.values<double>()[3] == -INF);
        CHECK(f.is_null(4));
        CHECK(f.null_count() == 1);
    }

    TEST_CASE("the file lines are DFTracer records with integer ph") {
        TestEnvironment env(0);
        auto sample = sample_frame();
        const dstd::String path = env.dir() + "/l.pfw";
        REQUIRE(
            write_table(sample.frame, path, sample.header, plain_options()));
        const dstd::String text = read_text(path);
        json::Document doc;
        std::size_t line_no = 0;
        std::size_t at = 0;
        while (at < text.size()) {
            const std::size_t end = text.find('\n', at);
            REQUIRE(end != dstd::String::npos);
            auto root = doc.parse(dstd::StringView(text).substr(at, end - at));
            REQUIRE(root.has_value());
            auto ph = root->find("ph");
            REQUIRE(ph.has_value());
            CHECK(ph->as_int().value_or(-1) == (line_no == 0 ? 4 : 3));
            ++line_no;
            at = end + 1;
        }
        CHECK(line_no == 7);
        CHECK(first_line(text).find("\"format\":\"dftu.table\"") !=
              dstd::StringView::npos);
    }

    TEST_CASE("an empty frame round trips with its columns") {
        TestEnvironment env(0);
        DataFrame frame;
        frame.names.push_back(dstd::SharedString("a"));
        frame.columns.push_back(Series::flat(TypeId::Int64, nullptr, 0));
        frame.names.push_back(dstd::SharedString("s"));
        frame.columns.push_back(
            Series::strings(dstd::Span<const dstd::StringView>()));
        const dstd::String fields[] = {field_json("a", "int64"),
                                       field_json("s", "object")};
        const dstd::String header = header_json(0, fields);
        const dstd::String path = env.dir() + "/e.pfw.gz";
        REQUIRE(write_table(frame, path, header));
        auto back = dftu_utils_test::ok(read_table(path));
        CHECK(back.frame.num_columns() == 2);
        CHECK(back.frame.num_rows() == 0);
        CHECK(back.frame.columns[0].type() == TypeId::Int64);
        CHECK(back.frame.columns[1].type() == TypeId::String);
    }

    TEST_CASE("single member, many members and members cut mid line agree") {
        TestEnvironment env(0);
        auto sample = sample_frame();
        const dstd::String plain = env.dir() + "/p.pfw";
        REQUIRE(
            write_table(sample.frame, plain, sample.header, plain_options()));
        const dstd::String text = read_text(plain);
        auto want = dftu_utils_test::ok(read_table(plain));
        for (const std::size_t member : {text.size(), std::size_t{200},
                                         std::size_t{97}, std::size_t{16}}) {
            const dstd::String path = env.dir() + "/m.pfw.gz";
            write_gzip_members(path, text, member);
            auto got = dftu_utils_test::ok(read_table(path));
            CHECK(same_frame(want.frame, got.frame));
            CHECK_FALSE(got.inflated_in_order);
            CHECK(got.members_in_parallel >= 1);
        }
        CHECK(want.members_in_parallel == 0);
    }

    TEST_CASE("a gzip header mark inside a member sends the file in order") {
        TestEnvironment env(0);
        auto sample = sample_frame();
        const dstd::String plain = env.dir() + "/p.pfw";
        REQUIRE(
            write_table(sample.frame, plain, sample.header, plain_options()));
        const dstd::String text = read_text(plain);
        auto want = dftu_utils_test::ok(read_table(plain));
        const dstd::String one = gzip_members(text, text.size());
        // An extra field holding a whole gzip header puts a false member start
        // in the file.
        dstd::String forged = one.substr(0, 3);
        forged += '\x04';
        forged.append(one.data() + 4, 6);
        forged.append("\x0a\x00\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 12);
        forged.append(one.data() + 10, one.size() - 10);
        const dstd::String path = env.dir() + "/forged.pfw.gz";
        write_text(path, forged);
        auto got = dftu_utils_test::ok(read_table(path));
        CHECK(got.inflated_in_order);
        CHECK(same_frame(want.frame, got.frame));
    }

    TEST_CASE("corrupt gzip data is refused") {
        TestEnvironment env(0);
        auto sample = sample_frame();
        const dstd::String plain = env.dir() + "/p.pfw";
        REQUIRE(
            write_table(sample.frame, plain, sample.header, plain_options()));
        const dstd::String good = gzip_members(read_text(plain), 1 << 20);
        const dstd::String path = env.dir() + "/bad.pfw.gz";

        dstd::String crc = good;
        crc[crc.size() - 8] = static_cast<char>(crc[crc.size() - 8] ^ 0x55);
        write_text(path, crc);
        auto bad_crc = read_table(path);
        REQUIRE_FALSE(bad_crc.has_value());
        CHECK(code_of(bad_crc.error()) == common::ErrorCode::Compression);

        write_text(path, good + "trailing garbage");
        auto trailing = read_table(path);
        REQUIRE_FALSE(trailing.has_value());
        CHECK(code_of(trailing.error()) == common::ErrorCode::Compression);

        write_text(path, good.substr(0, good.size() / 2));
        CHECK_FALSE(read_table(path).has_value());
    }

    TEST_CASE("a forged size in a gzip trailer cannot make a huge allocation") {
        TestEnvironment env(0);
        auto sample = sample_frame();
        const dstd::String plain = env.dir() + "/p.pfw";
        REQUIRE(
            write_table(sample.frame, plain, sample.header, plain_options()));
        dstd::String forged = gzip_members(read_text(plain), 1 << 20);
        for (std::size_t i = 1; i <= 4; ++i) forged[forged.size() - i] = '\xff';
        const dstd::String path = env.dir() + "/isize.pfw.gz";
        write_text(path, forged);
        auto r = read_table(path);
        REQUIRE_FALSE(r.has_value());
        CHECK(code_of(r.error()) == common::ErrorCode::Compression);
    }

    TEST_CASE("CRLF line ends read like LF") {
        TestEnvironment env(0);
        auto sample = sample_frame();
        const dstd::String plain = env.dir() + "/p.pfw";
        REQUIRE(
            write_table(sample.frame, plain, sample.header, plain_options()));
        const dstd::String text = read_text(plain);
        dstd::String crlf;
        for (const char c : text) {
            if (c == '\n') crlf += '\r';
            crlf += c;
        }
        const dstd::String path = env.dir() + "/crlf.pfw";
        write_text(path, crlf);
        auto got = dftu_utils_test::ok(read_table(path));
        auto want = dftu_utils_test::ok(read_table(plain));
        CHECK(same_frame(want.frame, got.frame));
    }

    TEST_CASE("a token that is not JSON is never a null") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/tok.pfw";
        const char* const dtypes[] = {"Float64", "Int64",  "UInt64",
                                      "boolean", "string", "object"};
        const char* const tokens[] = {"NaN",  "Infinity", "nan",
                                      "None", "@@",       "nul"};
        for (const char* dtype : dtypes) {
            const dstd::String fields[] = {field_json("x", dtype)};
            for (const char* token : tokens) {
                CAPTURE(dstd::StringView(dtype));
                CAPTURE(dstd::StringView(token));
                dstd::String text = header_line(header_json(1, fields));
                text += "{\"args\":{\"x\":";
                text += token;
                text += "}}\n";
                write_text(path, text);
                CHECK_FALSE(read_table(path).has_value());
            }
        }
        const dstd::String raw_field[] = {
            field_json("x", "object", "\"elem\":\"list\"")};
        dstd::String text = header_line(header_json(1, raw_field));
        text += "{\"args\":{\"x\":NaN}}\n";
        write_text(path, text);
        CHECK_FALSE(read_table(path).has_value());
    }

    TEST_CASE("the string NaN is a string, a float and not an integer") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/nan.pfw";
        const dstd::String ints[] = {field_json("x", "Int64")};
        write_text(path, header_line(header_json(1, ints)) +
                             "{\"args\":{\"x\":\"NaN\"}}\n");
        CHECK_FALSE(read_table(path).has_value());
        const dstd::String strs[] = {field_json("x", "string")};
        write_text(path, header_line(header_json(1, strs)) +
                             "{\"args\":{\"x\":\"NaN\"}}\n");
        auto s = dftu_utils_test::ok(read_table(path));
        CHECK(s.frame.columns[0].null_count() == 0);
        CHECK(s.frame.columns[0].string_at(0) == "NaN");
        const dstd::String floats[] = {field_json("x", "Float64")};
        write_text(path, header_line(header_json(1, floats)) +
                             "{\"args\":{\"x\":\"NaN\"}}\n");
        auto f = dftu_utils_test::ok(read_table(path));
        CHECK(f.frame.columns[0].null_count() == 0);
        CHECK(std::isnan(f.frame.columns[0].values<double>()[0]));
    }

    TEST_CASE("the writer's own members decode in parallel to the same frame") {
        TestEnvironment env(0);
        const std::size_t n = 20000;
        dstd::Vector<std::int64_t> a(n);
        dstd::Vector<double> b(n);
        dstd::Vector<dstd::String> owned(n);
        dstd::Vector<dstd::StringView> views(n);
        for (std::size_t i = 0; i < n; ++i) {
            a[i] = static_cast<std::int64_t>(i) * 37 - 1000;
            b[i] = static_cast<double>(i) / 7.0;
            owned[i] = dstd::fmt<"row {}">(i);
        }
        for (std::size_t i = 0; i < n; ++i) views[i] = owned[i];
        DataFrame frame;
        frame.names.push_back(dstd::SharedString("a"));
        frame.columns.push_back(Series::flat(TypeId::Int64, a.data(),
                                             static_cast<std::int64_t>(n)));
        frame.names.push_back(dstd::SharedString("b"));
        frame.columns.push_back(Series::flat(TypeId::Float64, b.data(),
                                             static_cast<std::int64_t>(n)));
        frame.names.push_back(dstd::SharedString("s"));
        frame.columns.push_back(Series::strings(views));
        const dstd::String fields[] = {field_json("a", "int64"),
                                       field_json("b", "float64"),
                                       field_json("s", "object")};
        const dstd::String header = header_json(n, fields);
        const dstd::String path = env.dir() + "/big.pfw.gz";
        WriteOptions opts;
        opts.member_size = 64 * 1024;
        REQUIRE(write_table(frame, path, header, opts));
        auto one = dftu_utils_test::ok(read_table(path, {.parts = 1}));
        auto many = dftu_utils_test::ok(read_table(path, {.parts = 7}));
        CHECK(many.members_in_parallel > 1);
        CHECK_FALSE(many.inflated_in_order);
        CHECK(same_frame(frame, one.frame));
        CHECK(same_frame(frame, many.frame));
    }

    TEST_CASE("a row needs only args and a header may lack index_names") {
        TestEnvironment env(0);
        const dstd::String fields[] = {field_json("x", "int64"),
                                       field_json("y", "Float64")};
        dstd::String args =
            "{\"format\":\"dftu.table\",\"format_version\":1,\"nrows\":2,"
            "\"index\":[],\"future\":{\"k\":1},\"fields\":[" +
            fields[0] + "," + fields[1] + "]}";
        dstd::String text = header_line(args);
        text += "{\"ph\": 3, \"args\": {\"x\": 1, \"y\": null}}\n";
        text += "{\"args\":{\"y\":\"-Infinity\",\"x\":-2}}";
        const dstd::String path = env.dir() + "/tol.pfw";
        write_text(path, text);
        auto t = dftu_utils_test::ok(read_table(path));
        CHECK(t.frame.num_rows() == 2);
        CHECK(t.frame.columns[0].values<std::int64_t>()[1] == -2);
        CHECK(t.frame.columns[1].is_null(0));
        CHECK(t.frame.columns[1].values<double>()[1] == -INF);
    }

    TEST_CASE("category labels decode as strings, integers or numbers") {
        TestEnvironment env(0);
        const dstd::String fields[] = {
            field_json("cs", "category",
                       "\"categories\":[\"a\",\"b\"],\"ordered\":false"),
            field_json("ci", "category",
                       "\"categories\":[3,4],\"ordered\":true"),
            field_json("cf", "category", "\"categories\":[1.5,2.5]")};
        dstd::String text = header_line(header_json(2, fields));
        text += "{\"args\":{\"cs\":\"b\",\"ci\":3,\"cf\":2.5}}\n";
        text += "{\"args\":{\"cs\":null,\"ci\":4,\"cf\":null}}\n";
        const dstd::String path = env.dir() + "/cat.pfw";
        write_text(path, text);
        auto t = dftu_utils_test::ok(read_table(path));
        CHECK(t.frame.columns[0].type() == TypeId::String);
        CHECK(t.frame.columns[0].string_at(0) == "b");
        CHECK(t.frame.columns[0].is_null(1));
        CHECK(t.frame.columns[1].type() == TypeId::Int64);
        CHECK(t.frame.columns[1].values<std::int64_t>()[1] == 4);
        CHECK(t.frame.columns[2].type() == TypeId::Float64);
        CHECK(t.frame.columns[2].values<double>()[0] == 2.5);
    }

    TEST_CASE("the reader refuses what it cannot read") {
        TestEnvironment env(0);
        const dstd::String fields[] = {field_json("x", "int64")};
        const dstd::String row1 = "{\"args\":{\"x\":1}}\n";
        struct Case {
            const char* name;
            dstd::String text;
            common::ErrorCode code;
        };
        const dstd::Vector<Case> cases = {
            {"unknown format_version",
             header_line(header_json(1, fields, "2")) + row1,
             common::ErrorCode::Parse},
            {"a higher minor than the engine's",
             header_line(header_json(1, fields, "\"1.1\"")) + row1,
             common::ErrorCode::Parse},
            {"another major",
             header_line(header_json(1, fields, "\"2.0\"")) + row1,
             common::ErrorCode::Parse},
            {"a major below the engine's",
             header_line(header_json(1, fields, "\"0.0\"")) + row1,
             common::ErrorCode::Parse},
            {"a version with no minor",
             header_line(header_json(1, fields, "\"1\"")) + row1,
             common::ErrorCode::Parse},
            {"a malformed version",
             header_line(header_json(1, fields, "\"1.x\"")) + row1,
             common::ErrorCode::Parse},
            {"a version with three parts",
             header_line(header_json(1, fields, "\"1.0.0\"")) + row1,
             common::ErrorCode::Parse},
            {"another format",
             header_line(header_json(1, fields, "1", "other")) + row1,
             common::ErrorCode::Parse},
            {"fewer rows than the header says",
             header_line(header_json(3, fields)) + row1 + row1,
             common::ErrorCode::Parse},
            {"more rows than the header says",
             header_line(header_json(1, fields)) + row1 + row1,
             common::ErrorCode::Parse},
            {"a row that misses a field",
             header_line(header_json(1, fields)) + "{\"args\":{\"y\":1}}\n",
             common::ErrorCode::Parse},
            {"a string in an integer column",
             header_line(header_json(1, fields)) + "{\"args\":{\"x\":\"7\"}}\n",
             common::ErrorCode::Parse},
            {"a float in an integer column",
             header_line(header_json(1, fields)) + "{\"args\":{\"x\":1.5}}\n",
             common::ErrorCode::Parse},
            {"null in a numpy integer column",
             header_line(header_json(1, fields)) + "{\"args\":{\"x\":null}}\n",
             common::ErrorCode::Parse},
            {"a row that is not JSON",
             header_line(header_json(1, fields)) + "{\"args\":\n",
             common::ErrorCode::Parse},
            {"a header that is not a table",
             "{\"ph\":4,\"args\":{\"nope\":1}}\n", common::ErrorCode::Parse},
        };
        for (const Case& c : cases) {
            CAPTURE(dstd::StringView(c.name));
            const dstd::String path = env.dir() + "/bad.pfw";
            write_text(path, c.text);
            auto r = read_table(path);
            REQUIRE_FALSE(r.has_value());
            CHECK(code_of(r.error()) == c.code);
            CHECK_FALSE(r.error().message().empty());
        }
        auto missing = read_table(env.dir() + "/missing.pfw");
        REQUIRE_FALSE(missing.has_value());
    }

    TEST_CASE("the reader takes the engine's major with a minor not above it") {
        TestEnvironment env(0);
        const dstd::String fields[] = {field_json("x", "int64")};
        const dstd::String row1 = "{\"args\":{\"x\":1}}\n";
        for (const char* version : {"\"1.0\"", "1"}) {
            CAPTURE(dstd::StringView(version));
            const dstd::String path = env.dir() + "/ok.pfw";
            write_text(path,
                       header_line(header_json(1, fields, version)) + row1);
            auto r = read_table(path);
            REQUIRE(r.has_value());
            CHECK(r->frame.num_rows() == 1);
        }
    }

    TEST_CASE("blank lines between rows are skipped") {
        TestEnvironment env(0);
        const dstd::String fields[] = {field_json("x", "int64")};
        const dstd::String text = header_line(header_json(2, fields)) +
                                  "{\"args\":{\"x\":1}}\n\n  \n"
                                  "{\"args\":{\"x\":2}}\n\n";
        const dstd::String path = env.dir() + "/blank.pfw";
        write_text(path, text);
        auto t = dftu_utils_test::ok(read_table(path));
        CHECK(t.frame.num_rows() == 2);
        CHECK(t.frame.columns[0].values<std::int64_t>()[1] == 2);
    }

    TEST_CASE("a value outside the width of its dtype is refused") {
        TestEnvironment env(0);
        const dstd::String small[] = {field_json("x", "int8")};
        dstd::String text =
            header_line(header_json(1, small)) + "{\"args\":{\"x\":200}}\n";
        const dstd::String path = env.dir() + "/w.pfw";
        write_text(path, text);
        CHECK_FALSE(read_table(path).has_value());
        const dstd::String uns[] = {field_json("x", "uint64")};
        text = header_line(header_json(1, uns)) + "{\"args\":{\"x\":-1}}\n";
        write_text(path, text);
        CHECK_FALSE(read_table(path).has_value());
        text = header_line(header_json(1, uns)) +
               "{\"args\":{\"x\":18446744073709551615}}\n";
        write_text(path, text);
        auto ok_read = dftu_utils_test::ok(read_table(path));
        CHECK(ok_read.frame.columns[0].values<std::uint64_t>()[0] == U64_MAX);
    }

    TEST_CASE("the writer checks the header against the frame") {
        TestEnvironment env(0);
        auto sample = sample_frame();
        const dstd::String path = env.dir() + "/h.pfw";
        const dstd::String fields[] = {field_json("i64", "Int64")};
        auto few = write_table(sample.frame, path, header_json(6, fields),
                               plain_options());
        REQUIRE_FALSE(few.has_value());
        CHECK(code_of(few.error()) == common::ErrorCode::InvalidArgument);
        auto rows = write_table(sample.frame, path, header_json(5, {}),
                                plain_options());
        REQUIRE_FALSE(rows.has_value());
        CHECK(code_of(rows.error()) == common::ErrorCode::InvalidArgument);
        const dstd::StringView unknown[] = {"nope"};
        WriteOptions bad = plain_options();
        bad.raw_columns = unknown;
        auto raw = write_table(sample.frame, path, sample.header, bad);
        REQUIRE_FALSE(raw.has_value());
        CHECK(code_of(raw.error()) == common::ErrorCode::InvalidArgument);
    }

    TEST_CASE("the writer refuses a dtype the column cannot carry") {
        TestEnvironment env(0);
        auto sample = sample_frame();
        const dstd::String path = env.dir() + "/d.pfw";
        const auto with_dtype = [&](std::size_t at, dstd::StringView dtype,
                                    dstd::StringView extra = {}) {
            dstd::String fields[] = {
                field_json("i64", "Int64"),
                field_json("u64", "UInt64"),
                field_json("f64", "Float64"),
                field_json("b", "boolean"),
                field_json("s", "string"),
                field_json("raw", "object", "\"elem\":\"set\"")};
            static const char* const names[] = {"i64", "u64", "f64",
                                                "b",   "s",   "raw"};
            fields[at] = field_json(names[at], dtype, extra);
            return write_table(sample.frame, path, header_json(6, fields),
                               plain_options());
        };
        CHECK_FALSE(with_dtype(0, "float64").has_value());
        CHECK_FALSE(with_dtype(1, "int64").has_value());
        CHECK_FALSE(with_dtype(2, "Int64").has_value());
        CHECK_FALSE(with_dtype(3, "string").has_value());
        CHECK_FALSE(with_dtype(4, "int64").has_value());
        CHECK_FALSE(with_dtype(5, "string").has_value());
        CHECK_FALSE(with_dtype(4, "object", "\"elem\":\"set\"").has_value());
        CHECK_FALSE(with_dtype(0, "int8").has_value());
        CHECK(with_dtype(0, "Int64").has_value());
        // The i64 column holds a null; a numpy int64 dtype has no nulls.
        auto nulls = with_dtype(0, "int64");
        REQUIRE_FALSE(nulls.has_value());
        CHECK(code_of(nulls.error()) == common::ErrorCode::InvalidArgument);
    }

    TEST_CASE("the writer refuses an empty or multi-line raw cell and header") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/r.pfw";
        const dstd::String fields[] = {
            field_json("raw", "object", "\"elem\":\"list\"")};
        const dstd::StringView bad_cells[] = {"", "[1,\n2]"};
        for (const dstd::StringView cell : bad_cells) {
            const dstd::StringView cells[] = {"[1]", cell};
            DataFrame frame;
            frame.names.push_back(dstd::SharedString("raw"));
            frame.columns.push_back(Series::strings(cells));
            auto r = write_table(frame, path, header_json(2, fields),
                                 plain_options());
            REQUIRE_FALSE(r.has_value());
            CHECK(code_of(r.error()) == common::ErrorCode::InvalidArgument);
        }
        auto sample = sample_frame();
        dstd::String header = sample.header;
        header.insert(header.size() - 1, "\n");
        auto r = write_table(sample.frame, path, header, plain_options());
        REQUIRE_FALSE(r.has_value());
        CHECK(code_of(r.error()) == common::ErrorCode::InvalidArgument);
    }
}
