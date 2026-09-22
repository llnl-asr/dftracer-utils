#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/utilities/host_ops.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

namespace {

namespace sfs = std::filesystem;

dftu_op_val str_val(const std::string& s) {
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

std::vector<std::string> rows(const Series& s) {
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < s.length(); ++i)
        out.emplace_back(s.string_at(i));
    std::sort(out.begin(), out.end());
    return out;
}

void write_file(const sfs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

using dftu_utils_test::ScopedTestDir;

}  // namespace

TEST_SUITE("host_ops") {
    TEST_CASE("register_host_ops is idempotent") {
        dftracer::utils::utilities::register_host_ops();
        dftracer::utils::utilities::register_host_ops();
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
            CHECK(std::string(dftu_op_signature(op->sig)) == e.signature);
        }
        // The column op that stayed a kernel, next to its inverse.
        CHECK(dftu_op_find("dftu.hex.format64") != nullptr);
        CHECK(dftu_op_find("dftu.hex.parse64") != nullptr);
    }

    TEST_CASE("dftu.fs.scan_dir lists a directory as a String column") {
        ScopedTestDir dir("dftu_host_ops_scan");
        write_file(dir.path() / "a.log", "x");
        write_file(dir.path() / "b.txt", "y");

        const std::string root = dir.path().string();
        dftu_op_arg arg{};
        arg.args[0] = str_val(root);
        Series out = run_series("dftu.fs.scan_dir", nullptr, arg);
        REQUIRE(out.handle() != nullptr);
        CHECK(out.type() == TypeId::String);
        std::vector<std::string> got = rows(out);
        REQUIRE(got.size() == 2);
        CHECK(got[0] == (dir.path() / "a.log").string());
        CHECK(got[1] == (dir.path() / "b.txt").string());
    }

    TEST_CASE("dftu.fs.scan_dir reports a missing directory as a null column") {
        const std::string missing =
            (sfs::current_path() / "dftu_host_ops_absent").string();
        dftu_op_arg arg{};
        arg.args[0] = str_val(missing);
        Series out = run_series("dftu.fs.scan_dir", nullptr, arg);
        CHECK(out.handle() == nullptr);
    }

    TEST_CASE("dftu.fs.scan_dir_pattern keeps only matching files") {
        ScopedTestDir dir("dftu_host_ops_pattern");
        write_file(dir.path() / "a.log", "x");
        write_file(dir.path() / "b.txt", "y");
        write_file(dir.path() / "c.log", "z");

        const std::string root = dir.path().string();
        const std::string pattern = ".log";
        dftu_op_arg arg{};
        arg.args[0] = str_val(root);
        arg.args[1] = str_val(pattern);
        Series out = run_series("dftu.fs.scan_dir_pattern", nullptr, arg);
        REQUIRE(out.handle() != nullptr);
        std::vector<std::string> got = rows(out);
        REQUIRE(got.size() == 2);
        CHECK(got[0] == (dir.path() / "a.log").string());
        CHECK(got[1] == (dir.path() / "c.log").string());
    }

    TEST_CASE("dftu.file.compress and dftu.file.decompress round trip") {
        ScopedTestDir dir("dftu_host_ops_gzip");
        const std::string text(64 * 1024, 'q');
        const std::string src = (dir.path() / "in.txt").string();
        const std::string gz = (dir.path() / "in.txt.gz").string();
        const std::string back = (dir.path() / "out.txt").string();
        write_file(src, text);

        dftu_op_arg carg{};
        carg.args[0] = str_val(src);
        carg.args[1] = str_val(gz);
        CHECK(run_bool("dftu.file.compress", carg));
        CHECK(sfs::exists(gz));

        dftu_op_arg darg{};
        darg.args[0] = str_val(gz);
        darg.args[1] = str_val(back);
        CHECK(run_bool("dftu.file.decompress", darg));

        std::ifstream f(back, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
        CHECK(got == text);
    }

    TEST_CASE("dftu.file.compress reports a missing input as false") {
        ScopedTestDir dir("dftu_host_ops_gzip_missing");
        const std::string src = (dir.path() / "absent.txt").string();
        const std::string gz = (dir.path() / "absent.txt.gz").string();
        dftu_op_arg arg{};
        arg.args[0] = str_val(src);
        arg.args[1] = str_val(gz);
        CHECK_FALSE(run_bool("dftu.file.compress", arg));
    }

    TEST_CASE("dftu.text.line_filter keeps the rows containing the needle") {
        Series lines = Series::strings(
            {"ERROR: disk full", "INFO: ok", "ERROR: timeout", "DEBUG: trace"});
        const std::string needle = "ERROR";
        dftu_op_arg arg{};
        arg.args[1] = str_val(needle);
        Series kept = run_series("dftu.text.line_filter", lines.handle(), arg);
        REQUIRE(kept.handle() != nullptr);
        REQUIRE(kept.length() == 2);
        CHECK(kept.string_at(0) == "ERROR: disk full");
        CHECK(kept.string_at(1) == "ERROR: timeout");
    }
}
