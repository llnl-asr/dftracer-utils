// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/domains/plugins/host_ops.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

dftu_op_val str_val(const dstd::String& s) {
    dftu_op_val v{};
    v.str.ptr = s.c_str();
    v.str.len = static_cast<int32_t>(s.size());
    return v;
}

Series run_series(const char* name, const dftu_series* in,
                  const dftu_op_arg& arg) {
    const dftu_op_desc* op = dftu_op_find(name);
    REQUIRE(op != nullptr);
    const dftu_series* cols[1] = {in};
    return Series{dftu_op_run(op, in ? cols : nullptr, in ? 1u : 0u, &arg)};
}

bool run_bool(const char* name, const dftu_op_arg& arg) {
    const dftu_op_desc* op = dftu_op_find(name);
    REQUIRE(op != nullptr);
    int ok = 0;
    dftu_scalar r = dftu_op_run_aggregate(op, nullptr, &arg, &ok);
    REQUIRE(ok == 1);
    return r.value.i != 0;
}

dstd::Vector<dstd::String> rows(const Series& s) {
    dstd::Vector<dstd::String> out;
    for (std::int64_t i = 0; i < s.length(); ++i)
        out.emplace_back(s.string_at(i));
    std::sort(out.begin(), out.end());
    return out;
}

void write_file(dstd::PathView p, const dstd::String& text) {
    const dstd::Path owned(p.str());
    dstd::fs::write_file(dstd::PathView(owned), text).discard();
}

}  // namespace

TEST_SUITE("host_ops") {
    TEST_CASE("register_host_ops is idempotent") {
        domains::plugins::register_host_ops();
        domains::plugins::register_host_ops();
        CHECK(dftu_op_find("dftu.fs.scan_dir") != nullptr);
    }

    TEST_CASE("every host op is registered with its documented signature") {
        struct Expected {
            const char* name;
            const char* signature;
        };
        const Expected expected[] = {
            {"dftu.fs.scan_dir", "(str) -> series"},
            {"dftu.fs.scan_dir_pattern", "(str, str) -> series"},
            {"dftu.file.compress", "(str, str) -> bool"},
            {"dftu.file.decompress", "(str, str) -> bool"},
            {"dftu.text.line_filter", "(series, str) -> series"},
        };
        for (const Expected& e : expected) {
            const dftu_op_desc* op = dftu_op_find(e.name);
            REQUIRE_MESSAGE(op != nullptr, e.name);
            CHECK(dstd::String(dftu_op_signature(op->sig)) == e.signature);
        }
        // The column op that stayed a kernel, next to its inverse.
        CHECK(dftu_op_find("dftu.hex.format64") != nullptr);
        CHECK(dftu_op_find("dftu.hex.parse64") != nullptr);
    }

    TEST_CASE("dftu.fs.scan_dir lists a directory as a String column") {
        dftu_utils_test::ScopedTestDir dir("dftu_host_ops_scan");
        write_file(dir.path() / "a.log", "x");
        write_file(dir.path() / "b.txt", "y");

        const dstd::String root = dstd::String(dir.path().str());
        dftu_op_arg arg{};
        arg.args[0] = str_val(root);
        Series out = run_series("dftu.fs.scan_dir", nullptr, arg);
        REQUIRE(out.handle() != nullptr);
        CHECK(out.type() == TypeId::String);
        dstd::Vector<dstd::String> got = rows(out);
        REQUIRE(got.size() == 2);
        CHECK(got[0] == (dir.path() / "a.log").str());
        CHECK(got[1] == (dir.path() / "b.txt").str());
    }

    TEST_CASE("dftu.fs.scan_dir reports a missing directory as a null column") {
        const dstd::String missing = dstd::String(
            (dstd::fs::current_path().value() / "dftu_host_ops_absent").str());
        dftu_op_arg arg{};
        arg.args[0] = str_val(missing);
        Series out = run_series("dftu.fs.scan_dir", nullptr, arg);
        CHECK(out.handle() == nullptr);
    }

    TEST_CASE("dftu.fs.scan_dir_pattern keeps only matching files") {
        dftu_utils_test::ScopedTestDir dir("dftu_host_ops_pattern");
        write_file(dir.path() / "a.log", "x");
        write_file(dir.path() / "b.txt", "y");
        write_file(dir.path() / "c.log", "z");

        const dstd::String root = dstd::String(dir.path().str());
        const dstd::String pattern = ".log";
        dftu_op_arg arg{};
        arg.args[0] = str_val(root);
        arg.args[1] = str_val(pattern);
        Series out = run_series("dftu.fs.scan_dir_pattern", nullptr, arg);
        REQUIRE(out.handle() != nullptr);
        dstd::Vector<dstd::String> got = rows(out);
        REQUIRE(got.size() == 2);
        CHECK(got[0] == (dir.path() / "a.log").str());
        CHECK(got[1] == (dir.path() / "c.log").str());
    }

    TEST_CASE("dftu.file.compress and dftu.file.decompress round trip") {
        dftu_utils_test::ScopedTestDir dir("dftu_host_ops_gzip");
        const dstd::String text(64 * 1024, 'q');
        const dstd::String src = dstd::String((dir.path() / "in.txt").str());
        const dstd::String gz = dstd::String((dir.path() / "in.txt.gz").str());
        const dstd::String back = dstd::String((dir.path() / "out.txt").str());
        write_file(src, text);

        dftu_op_arg carg{};
        carg.args[0] = str_val(src);
        carg.args[1] = str_val(gz);
        CHECK(run_bool("dftu.file.compress", carg));
        CHECK(dstd::fs::exists(gz).value());

        dftu_op_arg darg{};
        darg.args[0] = str_val(gz);
        darg.args[1] = str_val(back);
        CHECK(run_bool("dftu.file.decompress", darg));

        dstd::String got =
            dstd::fs::read_file(dstd::PathView(back)).value_or(dstd::String());
        CHECK(got == text);
    }

    TEST_CASE("dftu.file.compress reports a missing input as false") {
        dftu_utils_test::ScopedTestDir dir("dftu_host_ops_gzip_missing");
        const dstd::String src =
            dstd::String((dir.path() / "absent.txt").str());
        const dstd::String gz =
            dstd::String((dir.path() / "absent.txt.gz").str());
        dftu_op_arg arg{};
        arg.args[0] = str_val(src);
        arg.args[1] = str_val(gz);
        CHECK_FALSE(run_bool("dftu.file.compress", arg));
    }

    TEST_CASE("dftu.text.line_filter keeps the rows containing the needle") {
        Series lines = Series::strings(
            {"ERROR: disk full", "INFO: ok", "ERROR: timeout", "DEBUG: trace"});
        const dstd::String needle = "ERROR";
        dftu_op_arg arg{};
        arg.args[1] = str_val(needle);
        Series kept = run_series("dftu.text.line_filter", lines.handle(), arg);
        REQUIRE(kept.handle() != nullptr);
        REQUIRE(kept.length() == 2);
        CHECK(kept.string_at(0) == "ERROR: disk full");
        CHECK(kept.string_at(1) == "ERROR: timeout");
    }
}
