// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/binaries/cli_args.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

namespace {

struct Options {
    dstd::StringView output = "default.txt";
    dstd::Vector<dstd::StringView> names;
    dstd::Vector<dstd::StringView> inputs;
    bool force = false;
    int count = 3;
    double ratio = 0.5;
    std::size_t chunk = 0;
    std::size_t batch = 0;
    double seconds = 0;
    double millis = 0;
};

binaries::ParseOutcome run(Options& o,
                           const dstd::Vector<dstd::StringView>& args,
                           bool require_input = false) {
    binaries::ArgParser p("tool", "1.2.3", "A test tool.");
    auto& in = p.positional(o.inputs, "PATH", "Files or folders");
    if (require_input) in.at_least_one();
    p.option(o.output, {"-o", "--output"}, "Output file");
    p.flag(o.force, {"-f", "--force"}, "Force");
    p.option(o.names, {"--name"}, "Names");
    p.option(o.count, {"--count"}, "Count");
    p.option(o.ratio, {"--ratio"}, "Ratio");
    p.bytes(o.chunk, {"--chunk"}, "Chunk size", "64KB");
    p.bytes_with_unit(o.batch, {"--batch"}, "Batch size", "4MB");
    p.seconds(o.seconds, {"--timeout"}, "Timeout", "0");
    p.duration(o.millis, 1e3, {"--interval"}, "Interval", "5000");
    return p.parse(args);
}

bool inside(dstd::StringView view, dstd::StringView whole) {
    return view.data() >= whole.data() &&
           view.data() + view.size() <= whole.data() + whole.size();
}

}  // namespace

TEST_CASE("defaults come from the bound variables") {
    Options o;
    auto r = run(o, {});
    CHECK_FALSE(r.exit_code);
    CHECK(o.output == "default.txt");
    CHECK(o.count == 3);
    CHECK(o.chunk == 64 * 1024);
    CHECK(o.batch == 4 * 1024 * 1024);
    CHECK(o.millis == doctest::Approx(5000));
}

TEST_CASE("a path with a space stays whole") {
    Options o;
    auto r = run(o, {"/tmp/my traces/a b.pfw.gz", "-o", "out dir/x y"});
    CHECK_FALSE(r.exit_code);
    REQUIRE(o.inputs.size() == 1);
    CHECK(o.inputs[0] == "/tmp/my traces/a b.pfw.gz");
    CHECK(o.output == "out dir/x y");
}

TEST_CASE("files and folders mix in one list") {
    Options o;
    auto r = run(o, {"a.pfw.gz", "dir", "-f", "b.pfw.gz"});
    CHECK_FALSE(r.exit_code);
    CHECK(o.force);
    CHECK(o.inputs.size() == 3);
}

TEST_CASE("a missing input is an error when required") {
    Options o;
    auto r = run(o, {"-f"}, /*require_input=*/true);
    REQUIRE(r.exit_code);
    CHECK(*r.exit_code == 1);
    CHECK(r.err.find("PATH") != dstd::String::npos);
}

TEST_CASE("an unknown flag is an error") {
    Options o;
    auto r = run(o, {"--bogus"});
    REQUIRE(r.exit_code);
    CHECK(*r.exit_code == 1);
    CHECK(r.err.find("--bogus") != dstd::String::npos);
}

TEST_CASE("a missing value is an error") {
    Options o;
    auto r = run(o, {"--count"});
    REQUIRE(r.exit_code);
    CHECK(r.err.find("--count") != dstd::String::npos);
}

TEST_CASE("bad numbers, sizes and durations are errors") {
    Options o;
    auto number = run(o, {"--count", "abc"});
    REQUIRE(number.exit_code);
    CHECK(number.err.find("--count") != dstd::String::npos);

    auto ratio = run(o, {"--ratio", "1x"});
    CHECK(ratio.exit_code);

    auto size = run(o, {"--chunk", "12parsecs"});
    REQUIRE(size.exit_code);
    CHECK(size.err.find("--chunk") != dstd::String::npos);

    auto bare = run(o, {"--batch", "4096"});
    REQUIRE(bare.exit_code);
    CHECK(bare.err.find("unit") != dstd::String::npos);

    auto duration = run(o, {"--timeout", "soon"});
    REQUIRE(duration.exit_code);
    CHECK(duration.err.find("--timeout") != dstd::String::npos);
}

TEST_CASE("numbers, sizes and durations parse") {
    Options o;
    auto r =
        run(o, {"--count", "-7", "--ratio", "0.25", "--chunk", "2MB", "--batch",
                "512KB", "--timeout", "2m", "--interval", "5s"});
    CHECK_FALSE(r.exit_code);
    CHECK(o.count == -7);
    CHECK(o.ratio == doctest::Approx(0.25));
    CHECK(o.chunk == 2u * 1024 * 1024);
    CHECK(o.batch == 512u * 1024);
    CHECK(o.seconds == doctest::Approx(120));
    CHECK(o.millis == doctest::Approx(5000));
}

TEST_CASE("a list option takes repeated and following values") {
    Options repeated;
    CHECK_FALSE(run(repeated, {"--name", "a", "--name", "b"}).exit_code);
    CHECK(repeated.names.size() == 2);

    Options following;
    CHECK_FALSE(run(following, {"--name", "a", "b", "c", "-f"}).exit_code);
    REQUIRE(following.names.size() == 3);
    CHECK(following.names[2] == "c");
    CHECK(following.force);
}

TEST_CASE("--x=value works") {
    Options o;
    CHECK_FALSE(run(o, {"--output=a b.txt"}).exit_code);
    CHECK(o.output == "a b.txt");
}

TEST_CASE("help and version end the run with code 0") {
    Options o;
    auto help = run(o, {"--help"});
    REQUIRE(help.exit_code);
    CHECK(*help.exit_code == 0);
    CHECK(help.out.find("Usage: tool") != dstd::String::npos);
    CHECK(help.out.find("--output") != dstd::String::npos);
    CHECK(help.out.find("A test tool.") != dstd::String::npos);

    auto version = run(o, {"-v"});
    REQUIRE(version.exit_code);
    CHECK(*version.exit_code == 0);
    CHECK(version.out == "1.2.3\n");
}

TEST_CASE("parsed text points into argv and is never copied") {
    dstd::String out_arg = "--output=somewhere/file.txt";
    dstd::String name_a = "alpha";
    dstd::String name_b = "beta";
    dstd::String path_a = "in/one.pfw.gz";
    dstd::String path_b = "in/two.pfw.gz";
    dstd::Vector<dstd::StringView> args = {out_arg, "--name", name_a, name_b,
                                           "-f",    path_a,   path_b};
    Options o;
    auto r = run(o, args);
    REQUIRE_FALSE(r.exit_code);
    CHECK(inside(o.output, out_arg));
    REQUIRE(o.names.size() == 2);
    CHECK(inside(o.names[0], name_a));
    CHECK(inside(o.names[1], name_b));
    REQUIRE(o.inputs.size() == 2);
    CHECK(inside(o.inputs[0], path_a));
    CHECK(inside(o.inputs[1], path_b));
}
