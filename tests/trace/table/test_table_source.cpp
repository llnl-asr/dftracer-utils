// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/gzip/gzip_indexer.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/table/table_source.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::table;
using dataframe::col;
using dataframe::DataFrame;
using dftu_utils_test::TestEnvironment;

namespace {

dstd::String header_args(std::size_t nrows, dstd::StringView version = "1",
                         dstd::StringView dtype_i = "int64",
                         dstd::StringView extra = {}) {
    dstd::String s = "{\"format\":\"dftu.table\",\"format_version\":";
    s += version;
    s += ",\"table\":\"t\",\"nrows\":";
    s += dstd::fmt<"{}">(nrows);
    s += ",\"index\":[],\"index_names\":[],\"fields\":[{\"name\":\"i\",";
    s += "\"dtype\":\"";
    s += dtype_i;
    s += "\"},{\"name\":\"f\",\"dtype\":\"float64\"},{\"name\":\"s\",";
    s += "\"dtype\":\"object\"}";
    s += extra;
    s += "],\"meta\":{}}";
    return s;
}

dstd::String header_line(dstd::StringView args) {
    dstd::String s =
        "{\"name\":\"dftu.table\",\"cat\":\"dftu\",\"pid\":0,\"tid\":0,"
        "\"ts\":0,\"ph\":4,\"args\":";
    s += args;
    s += "}\n";
    return s;
}

dstd::String row(std::int64_t i, dstd::StringView f, dstd::StringView s) {
    const dstd::String n = dstd::fmt<"{}">(i);
    dstd::String out = "{\"name\":\"t\",\"cat\":\"dftu\",\"ts\":";
    out += n;
    out += ",\"ph\":3,\"pid\":0,\"tid\":0,\"args\":{\"i\":";
    out += n;
    out += ",\"f\":";
    out += f;
    out += ",\"s\":";
    out += s;
    out += "}}\n";
    return out;
}

// Rows with the values that trip a prune: NaN, null, an integer where a float
// goes, a multi-byte string.
dstd::String edge_rows() {
    dstd::String t;
    t += row(1, "1.5", "\"a\"");
    t += row(2, "\"NaN\"", "\"\xc3\xa9t\xc3\xa9\"");
    t += row(3, "null", "null");
    t += row(4, "7", "\"\xe4\xb8\xad\xe6\x96\x87\"");
    t += row(5, "\"Infinity\"", "\"a\"");
    t += row(6, "-2.25", "\"zz\"");
    return t;
}

dstd::String table_text(dstd::StringView rows, std::size_t nrows) {
    return header_line(header_args(nrows)) + dstd::String(rows);
}

dstd::String gz(dstd::StringView text, std::size_t member_bytes) {
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

// Members that end at line ends, as the table writer cuts them.
dstd::String gz_lines(dstd::StringView text, std::size_t member_bytes) {
    fileio::compress::GzipMemberCompressor comp(6);
    dstd::String all;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = std::min(text.size(), at + member_bytes);
        const std::size_t nl = text.find('\n', end > 0 ? end - 1 : 0);
        end = nl == dstd::StringView::npos ? text.size() : nl + 1;
        const auto piece = text.substr(at, end - at);
        auto member = comp.compress_member(piece.data(), piece.size());
        REQUIRE(member.has_value());
        all.append(reinterpret_cast<const char*>(member->data()),
                   member->size());
        at = end;
    }
    return all;
}

void put(dstd::StringView path, dstd::StringView text) {
    REQUIRE(dstd::fs::write_file(path, text).has_value());
}

dstd::Result<dstd::Optional<dstd::SharedPtr<const dataframe::Source>>> open(
    dstd::Span<const dstd::String> paths) {
    dstd::Vector<TableInput> inputs;
    for (const dstd::String& p : paths) inputs.push_back(TableInput{p, {}});
    return open_table_inputs(
        dstd::Span<const TableInput>(inputs.data(), inputs.size()));
}

dstd::SharedPtr<const dataframe::Source> open_one(dstd::StringView path) {
    const dstd::String p(path);
    auto opened = open(dstd::Span<const dstd::String>(&p, 1));
    const dstd::String why =
        opened ? dstd::String() : dstd::String(opened.error().message());
    INFO(why);
    REQUIRE(opened.has_value());
    REQUIRE(opened->has_value());
    return **opened;
}

DataFrame collect(dataframe::LazyFrame lf) {
    auto done = default_runtime().block_on(lf.collect());
    const dstd::String why =
        done ? dstd::String() : dstd::String(done.error().message());
    INFO(why);
    REQUIRE(done.has_value());
    return std::move(*done);
}

dstd::Vector<std::int64_t> ints(const DataFrame& df, std::size_t column) {
    dstd::Vector<std::int64_t> out;
    const auto c = df.columns[column].materialize();
    for (std::int64_t i = 0; i < c.length(); ++i)
        out.push_back(c.values<std::int64_t>()[i]);
    return out;
}

}  // namespace

TEST_SUITE("trace::table source") {
    TEST_CASE("a table is detected by line 1, a trace is not") {
        TestEnvironment env(0);
        const dstd::String table = env.dir() + "/a.pfw";
        const dstd::String trace = env.dir() + "/b.pfw";
        put(table, table_text(edge_rows(), 6));
        put(trace,
            "[\n{\"name\":\"n\",\"cat\":\"c\",\"pid\":1,\"tid\":1,\"ts\":1,"
            "\"dur\":1,\"ph\":\"X\"}\n");
        const dstd::String one[] = {table};
        auto opened = open(one);
        REQUIRE(opened.has_value());
        CHECK(opened->has_value());
        const dstd::String two[] = {trace};
        auto none = open(two);
        REQUIRE(none.has_value());
        CHECK_FALSE(none->has_value());
        const dstd::String gone[] = {env.dir() + "/missing.pfw"};
        auto missing = open(gone);
        REQUIRE(missing.has_value());
        CHECK_FALSE(missing->has_value());
    }

    TEST_CASE("a gzip table is detected from its first member only") {
        TestEnvironment env(0);
        dstd::String text = table_text(edge_rows(), 6);
        const dstd::String path = env.dir() + "/a.pfw.gz";
        put(path, gz(text, 64));
        auto source = open_one(path);
        CHECK(table_headers(*source).size() == 1);
        CHECK(table_paths(*source)[0] == path);
    }

    TEST_CASE("a line 1 that claims the format and is wrong is an error") {
        TestEnvironment env(0);
        const dstd::String bad_version = env.dir() + "/v.pfw";
        put(bad_version, header_line(header_args(0, "2")));
        const dstd::String not_phase_four = env.dir() + "/p.pfw";
        put(not_phase_four, "{\"name\":\"dftu.table\",\"ph\":3,\"args\":" +
                                header_args(0) + "}\n");
        const dstd::String cut = env.dir() + "/c.pfw";
        put(cut, "{\"name\":\"dftu.table\",\"ph\":4,\"args\":{\"format\":");
        const dstd::String no_fields = env.dir() + "/n.pfw";
        put(no_fields,
            "{\"name\":\"dftu.table\",\"ph\":4,\"args\":{\"format\":"
            "\"dftu.table\",\"format_version\":1}}\n");
        for (const dstd::String* p :
             {&bad_version, &not_phase_four, &cut, &no_fields}) {
            auto opened = open(dstd::Span<const dstd::String>(p, 1));
            CHECK_FALSE(opened.has_value());
        }
    }

    TEST_CASE("tables and traces together raise and name one of each") {
        TestEnvironment env(0);
        const dstd::String table = env.dir() + "/a.pfw";
        const dstd::String trace = env.dir() + "/b.pfw";
        put(table, table_text(edge_rows(), 6));
        put(trace, "[\n{\"name\":\"n\",\"ph\":\"X\",\"ts\":1}\n");
        const dstd::String both[] = {table, trace};
        auto opened = open(both);
        REQUIRE_FALSE(opened.has_value());
        const dstd::String message(opened.error().message());
        CHECK(message.find(table) != dstd::String::npos);
        CHECK(message.find(trace) != dstd::String::npos);
    }

    TEST_CASE("parts must agree on their fields before any row is read") {
        TestEnvironment env(0);
        const dstd::String a = env.dir() + "/a.pfw";
        const dstd::String b = env.dir() + "/b.pfw";
        put(a, table_text(edge_rows(), 6));
        put(b, header_line(header_args(0, "1", "float64")));
        const dstd::String both[] = {a, b};
        CHECK_FALSE(open(both).has_value());
        const dstd::String extra = env.dir() + "/c.pfw";
        put(extra,
            header_line(header_args(0, "1", "int64",
                                    ",{\"name\":\"z\",\"dtype\":\"int64\"}")));
        const dstd::String more[] = {a, extra};
        CHECK_FALSE(open(more).has_value());
    }

    TEST_CASE("parts read in part-number order, folders in list order") {
        TestEnvironment env(0);
        REQUIRE(dstd::fs::create_directories(env.dir() + "/x").has_value());
        REQUIRE(dstd::fs::create_directories(env.dir() + "/y").has_value());
        dstd::Vector<dstd::String> paths;
        for (const char* name : {"x/part-10.pfw", "x/part-2.pfw",
                                 "y/part-1.pfw", "x/part-1.pfw"}) {
            const dstd::String p = env.dir() + "/" + name;
            put(p, table_text(row(1, "1.0", "\"a\""), 1));
            paths.push_back(p);
        }
        auto opened = open(paths);
        REQUIRE(opened.has_value());
        const auto got = table_paths(***opened);
        REQUIRE(got.size() == 4);
        CHECK(got[0] == env.dir() + "/x/part-1.pfw");
        CHECK(got[1] == env.dir() + "/x/part-2.pfw");
        CHECK(got[2] == env.dir() + "/x/part-10.pfw");
        CHECK(got[3] == env.dir() + "/y/part-1.pfw");
    }

    TEST_CASE("a filter reads in batches and keeps the full predicate") {
        TestEnvironment env(0);
        const dstd::String text = table_text(edge_rows(), 6);
        const dstd::String plain = env.dir() + "/a.pfw";
        const dstd::String zipped = env.dir() + "/a.pfw.gz";
        put(plain, text);
        put(zipped, gz(text, 200));
        for (const dstd::String& path : {plain, zipped}) {
            auto source = open_one(path);
            reset_read_counters();
            DataFrame all = collect(dataframe::LazyFrame::scan(source));
            CHECK(all.num_rows() == 6);
            CHECK(read_counters().direct_files == 1);
            reset_read_counters();
            DataFrame some = collect(dataframe::LazyFrame::scan(source).filter(
                col(0) > std::int64_t{3}));
            CHECK(ints(some, 0) == dstd::Vector<std::int64_t>{4, 5, 6});
            CHECK(read_counters().streamed_files == 1);
            // NaN and null never pass a comparison; a float literal on an
            // integer column compares as a number.
            DataFrame floats = collect(
                dataframe::LazyFrame::scan(source).filter(col(1) > 1.0));
            CHECK(ints(floats, 0) == dstd::Vector<std::int64_t>{1, 4, 5});
            DataFrame ints_only =
                collect(dataframe::LazyFrame::scan(source).filter(
                    (col(0) >= 2.0) & (col(0) <= 2.0)));
            CHECK(ints(ints_only, 0) == dstd::Vector<std::int64_t>{2});
        }
    }

    TEST_CASE("a projection decodes only the columns it names") {
        TestEnvironment env(0);
        // Column s of this row is no string, so a read of it fails.
        dstd::String text = header_line(header_args(2));
        text += row(1, "1.0", "\"a\"");
        text += row(2, "2.0", "5");
        const dstd::String path = env.dir() + "/a.pfw";
        put(path, text);
        auto source = open_one(path);
        DataFrame picked = collect(dataframe::LazyFrame::scan(source).select(
            dstd::Vector<dstd::String>{"f", "i"}));
        CHECK(picked.num_rows() == 2);
        CHECK(dstd::StringView(picked.names[0]) == "f");
        auto whole = default_runtime().block_on(
            dataframe::LazyFrame::scan(source).collect());
        CHECK_FALSE(whole.has_value());
    }

    TEST_CASE("a header that counts the wrong rows is an error") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/a.pfw";
        put(path, table_text(edge_rows(), 5));
        auto source = open_one(path);
        auto done = default_runtime().block_on(
            dataframe::LazyFrame::scan(source).collect());
        CHECK_FALSE(done.has_value());
    }

    TEST_CASE("an index prunes a float literal on large integers exactly") {
        TestEnvironment env(0);
        constexpr std::int64_t TWO_53 = std::int64_t{1} << 53;
        constexpr std::int64_t FIRST = TWO_53 - 30000;
        constexpr std::size_t ROWS = 60000;
        // Members end at line ends, as the table writer cuts them.
        fileio::compress::GzipMemberCompressor comp(6);
        dstd::String zipped;
        dstd::String member = header_line(header_args(ROWS));
        auto flush = [&] {
            auto packed = comp.compress_member(member.data(), member.size());
            REQUIRE(packed.has_value());
            zipped.append(reinterpret_cast<const char*>(packed->data()),
                          packed->size());
            member.clear();
        };
        for (std::size_t k = 0; k < ROWS; ++k) {
            member += row(FIRST + static_cast<std::int64_t>(k), "1.5", "\"a\"");
            if (member.size() >= 32768) flush();
        }
        if (!member.empty()) flush();
        const dstd::String indexed = env.dir() + "/idx.pfw.gz";
        const dstd::String plain_dir = env.dir() + "/plain";
        REQUIRE(dstd::fs::create_directories(plain_dir).has_value());
        const dstd::String plain = plain_dir + "/idx.pfw.gz";
        put(indexed, zipped);
        put(plain, zipped);
        const dstd::String index_dir = env.dir() + "/index";
        REQUIRE(dftu_utils_test::build_index(indexed, index_dir, 32768));
        const dstd::String index_path = dstd::String(
            trace::internal::determine_index_path(indexed, index_dir));

        const dstd::Vector<TableInput> with_index{
            TableInput{indexed, index_path}};
        auto indexed_open = open_table_inputs(
            dstd::Span<const TableInput>(with_index.data(), with_index.size()));
        REQUIRE(indexed_open.has_value());
        REQUIRE(indexed_open->has_value());
        const auto indexed_source = **indexed_open;
        const auto plain_source = open_one(plain);

        const double two53 = 9007199254740992.0;
        struct Case {
            const char* label;
            dataframe::Expr expr;
            bool selective;
        };
        const Case cases[] = {
            {"== 2^53", (col(0) >= two53) & (col(0) <= two53), true},
            {"> 2^53", col(0) > two53, true},
            {">= 2^53", col(0) >= two53, true},
            {"< 2^53", col(0) < two53, true},
            {"<= 2^53", col(0) <= two53, true},
            {"> 2^53 + 25000", col(0) > 9007199254765992.0, true},
            {"< 2^53 - 25000", col(0) < 9007199254715992.0, true},
            {"below every row", col(0) < 9007199254000000.0, true},
        };
        for (const Case& c : cases) {
            INFO(c.label);
            reset_read_counters();
            const DataFrame got = collect(
                dataframe::LazyFrame::scan(indexed_source).filter(c.expr));
            const ReadCounters used = read_counters();
            const DataFrame want = collect(
                dataframe::LazyFrame::scan(plain_source).filter(c.expr));
            CHECK(used.indexed_files == 1);
            REQUIRE(got.columns.size() == 3);
            REQUIRE(want.columns.size() == 3);
            CHECK(ints(got, 0) == ints(want, 0));
            if (c.selective) CHECK(used.chunks_skipped > 0);
        }
    }

    TEST_CASE("an index built before the rows-complete fact existed scans") {
        TestEnvironment env(0);
        dstd::String text;
        for (std::int64_t k = 0; k < 3000; ++k) text += row(k, "1.5", "\"a\"");
        const dstd::String path = env.dir() + "/old.pfw.gz";
        put(path, gz_lines(table_text(text, 3000), 4096));
        const dstd::String index_dir = env.dir() + "/index";
        REQUIRE(dftu_utils_test::build_index(path, index_dir, 4096));
        const dstd::String index_path = dstd::String(
            trace::internal::determine_index_path(path, index_dir));

        // Writes the members and metadata of the file as the Indexer does, with
        // or without the fact that its rows read clean.
        auto index_with = [&](bool complete) {
            auto arts = default_runtime().block_on(
                trace::index::gzip::build_gzip_index_artifacts(
                    dstd::CStringView(path), 4096, {}));
            REQUIRE(arts.has_value());
            arts->table_rows_complete = complete;
            auto opened = trace::index::Index::open(index_path);
            REQUIRE(opened.has_value());
            auto build =
                opened->build(fileio::get_logical_path(path),
                              trace::index::SourceStat{
                                  static_cast<std::uint64_t>(
                                      fileio::get_file_modification_time(path)),
                                  fileio::file_size_bytes(path),
                                  fileio::calculate_file_hash(path)});
            REQUIRE(build.has_value());
            trace::index::gzip::persist_gzip_index_artifacts(*build, *arts);
            auto committed =
                default_runtime().block_on(std::move(*build).commit());
            REQUIRE(committed.has_value());
        };
        auto read = [&] {
            const dstd::Vector<TableInput> inputs{TableInput{path, index_path}};
            auto source = open_table_inputs(
                dstd::Span<const TableInput>(inputs.data(), inputs.size()));
            REQUIRE(source.has_value());
            REQUIRE(source->has_value());
            reset_read_counters();
            const DataFrame got =
                collect(dataframe::LazyFrame::scan(**source).filter(
                    col(0) > std::int64_t{2990}));
            CHECK(got.num_rows() == 9);
            return read_counters();
        };
        CHECK(read().indexed_files == 1);
        index_with(false);
        CHECK(read().indexed_files == 0);
        index_with(true);
        CHECK(read().indexed_files == 1);
    }
}
