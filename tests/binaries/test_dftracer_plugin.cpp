// SPDX-License-Identifier: MIT
// `dftracer_plugin`: the scaffolds `new` writes and the child process `check`
// runs.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdlib>
using namespace dftracer::utils;

namespace {

dstd::String env_or_probe(const char* var, const dstd::String& name) {
    const char* p = std::getenv(var);
    if (p != nullptr && ::access(p, R_OK) == 0) return p;
    for (const dstd::String& c : {"./" + name, "../" + name, "../../" + name}) {
        if (::access(c.c_str(), R_OK) == 0) return c;
    }
    return "";
}

struct Output {
    int exit_code = -1;
    dstd::String text;
    int signal = 0;
};

// Run the tool and capture stdout and stderr together.
Output run(const dstd::String& binary, const dstd::Vector<dstd::String>& args) {
    int pipefd[2];
    if (::pipe(pipefd) < 0) return {};

    dstd::Vector<const char*> argv;
    argv.push_back(binary.c_str());
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    ::close(pipefd[1]);

    Output out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
        out.text.append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    out.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    out.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    return out;
}

bool has(const dstd::String& haystack, dstd::StringView needle) {
    return haystack.find(needle) != dstd::String::npos;
}

bool write(const dstd::String& path, const dstd::String& text) {
    return dstd::fs::write_file(dstd::PathView(path), text).has_value();
}

dstd::String read(const dstd::String& path) {
    auto got = dstd::fs::read_file(dstd::PathView(path));
    return got ? dstd::String(*got) : dstd::String();
}

dstd::String plugin_tool() {
    return env_or_probe("DFTRACER_UTILS_PLUGIN_PATH", "dftracer_plugin");
}

bool make_dir(const dstd::String& path) {
    return dstd::fs::create_directories(dstd::PathView(path)).has_value();
}

}  // namespace

TEST_SUITE("DFTracerPlugin") {
    TEST_CASE("new writes one project per kind, with the name in it") {
        dstd::String tool = plugin_tool();
        if (tool.empty()) {
            MESSAGE("dftracer_plugin not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(4);
        REQUIRE(env.is_valid());
        const dstd::String root = env.dir() + "/scaffolds";
        REQUIRE(make_dir(root));

        for (const char* kind :
             {"fold", "udf", "aggregate", "format", "sink", "node"}) {
            const dstd::String dir = root + "/" + kind;
            REQUIRE(make_dir(dir));
            const dstd::String name = dstd::String("my_") + kind;
            Output made = run(tool, {"new", kind, name, "--dir", dir});
            CAPTURE(kind);
            REQUIRE(made.exit_code == 0);

            const dstd::String project = dir + "/" + name;
            auto exists = dstd::fs::exists(dstd::PathView(project));
            REQUIRE(exists.has_value());
            REQUIRE(*exists);

            for (const char* file : {"CMakeLists.txt", "plugin.cpp",
                                     "README.md", "tests/basic.test"}) {
                auto is_file = dstd::fs::is_regular_file(
                    dstd::PathView(project + "/" + file));
                REQUIRE(is_file.has_value());
                CHECK_MESSAGE(*is_file, kind, " must write ", file);
            }

            // The name replaced every placeholder, in every file it wrote.
            auto walked = dstd::fs::walk(
                dstd::PathView(project),
                [&](dstd::PathView path, dstd::fs::FileKind file_kind) {
                    if (file_kind != dstd::fs::FileKind::File)
                        return dstd::fs::WalkAction::Continue;
                    const dstd::String body = read(dstd::String(path.str()));
                    CHECK_MESSAGE(!has(body, "@NAME@"), path.str(),
                                  " keeps @NAME@");
                    return dstd::fs::WalkAction::Continue;
                });
            CHECK(walked.has_value());

            const dstd::String cmake = read(project + "/CMakeLists.txt");
            CHECK(has(cmake, "find_package(dftracer_utils"));
            CHECK(has(cmake, name));

            const dstd::String source = read(project + "/plugin.cpp");
            CHECK(has(source, "DFTU_PLUGIN_INFO(\"my_"));

            // A README of 5 to 10 lines, with the name in it.
            const dstd::String readme = read(project + "/README.md");
            std::size_t lines = 0;
            for (std::size_t at = 0; at < readme.size(); ++at)
                if (readme[at] == '\n') ++lines;
            const bool readme_len_ok = lines >= 5 && lines <= 10;
            CHECK_MESSAGE(readme_len_ok, kind, " README has ", lines, " lines");
            const dstd::String named = dstd::String("my_") + kind;
            CHECK(has(readme, dstd::StringView(named)));

            // The golden test it writes comes with its input file.
            const dstd::String golden = read(project + "/tests/basic.test");
            CHECK(has(golden, "query"));
            CHECK(has(golden, "----"));

            // A second run into the same folder refuses.
            Output again = run(tool, {"new", kind, name, "--dir", dir});
            CHECK(again.exit_code != 0);
            CHECK(has(again.text, "exists"));
        }
    }

    TEST_CASE("new refuses a name that is not a C identifier") {
        dstd::String tool = plugin_tool();
        if (tool.empty()) {
            MESSAGE("dftracer_plugin not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(4);
        REQUIRE(env.is_valid());
        for (const char* bad : {"2nope", "no-dash", "has space", ""}) {
            Output made = run(tool, {"new", "udf", bad, "--dir", env.dir()});
            CAPTURE(bad);
            CHECK(made.exit_code != 0);
            CHECK(has(made.text, "identifier"));
        }
        Output unknown =
            run(tool, {"new", "widgets", "ok", "--dir", env.dir()});
        CHECK(unknown.exit_code != 0);
        CHECK(has(unknown.text, "fold"));
    }

    TEST_CASE("check prints every kind list of a real plugin") {
        dstd::String tool = plugin_tool();
        dstd::String plugin = env_or_probe(
            "DFTRACER_UTILS_DURATION_UDFS_PLUGIN_PATH", "duration_udfs.so");
        if (tool.empty() || plugin.empty()) {
            MESSAGE("dftracer_plugin or the duration_udfs plugin not found.");
            return;
        }
        Output checked = run(tool, {"check", plugin});
        CAPTURE(checked.text);
        CHECK(checked.exit_code == 0);
        CHECK(has(checked.text, "name: duration_udfs"));
        CHECK(has(checked.text, "version: 1.0"));
        CHECK(has(checked.text, "abi_version:"));
        CHECK(has(checked.text, "udfs: to_ms, is_slow"));
        CHECK(has(checked.text, "file_formats: (none)"));
        CHECK(has(checked.text, "sinks: (none)"));
        CHECK(has(checked.text, "plugin unloaded"));
        CHECK(has(checked.text, "no leaks"));
    }

    TEST_CASE("check reports a leak and fails") {
        dstd::String tool = plugin_tool();
        dstd::String plugin =
            env_or_probe("DFTRACER_UTILS_LEAKY_PLUGIN_PATH", "leaky_plugin.so");
        if (tool.empty() || plugin.empty()) {
            MESSAGE("dftracer_plugin or leaky_plugin not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(4);
        REQUIRE(env.is_valid());
        REQUIRE(make_dir(env.dir() + "/data"));
        REQUIRE(make_dir(env.dir() + "/tests"));
        REQUIRE(write(env.dir() + "/data/sample.leaky", "1\n2\n"));
        REQUIRE(write(env.dir() + "/tests/basic.test",
                      "input ../data/sample.leaky\n"
                      "query\n"
                      "select id\n"
                      "----\n"
                      "id\n"
                      "1\n"));

        Output checked =
            run(tool, {"check", plugin, "--tests", env.dir() + "/tests"});
        CAPTURE(checked.text);
        CHECK(checked.exit_code != 0);
        CHECK(has(checked.text, "name: leaky"));
        CHECK(has(checked.text, "file_formats: leaky"));
        CHECK(has(checked.text, "PASS"));
        CHECK(has(checked.text, "leak"));
        CHECK(has(checked.text, "exit check"));
        CHECK(has(checked.text, "check: failed"));
    }

    TEST_CASE("check reports a crash by signal and fails") {
        dstd::String tool = plugin_tool();
        dstd::String plugin =
            env_or_probe("DFTRACER_UTILS_CRASH_PLUGIN_PATH", "crash_plugin.so");
        if (tool.empty() || plugin.empty()) {
            MESSAGE("dftracer_plugin or crash_plugin not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(4);
        REQUIRE(env.is_valid());
        REQUIRE(make_dir(env.dir() + "/data"));
        REQUIRE(make_dir(env.dir() + "/tests"));
        REQUIRE(write(env.dir() + "/data/sample.crash", "1\n"));
        REQUIRE(write(env.dir() + "/tests/basic.test",
                      "input ../data/sample.crash\n"
                      "query\n"
                      "select id\n"
                      "----\n"
                      "id\n"
                      "1\n"));

        Output checked =
            run(tool, {"check", plugin, "--tests", env.dir() + "/tests"});
        CAPTURE(checked.text);
        // The tool itself returns; only the child died.
        CHECK(checked.signal == 0);
        CHECK(checked.exit_code != 0);
        CHECK(has(checked.text, "died from signal"));
        CHECK(has(checked.text, "SIGABRT"));
    }

    TEST_CASE("a golden test fails with a diff, and --record fixes it") {
        dstd::String tool = plugin_tool();
        dstd::String plugin = env_or_probe(
            "DFTRACER_UTILS_DURATION_UDFS_PLUGIN_PATH", "duration_udfs.so");
        if (tool.empty() || plugin.empty()) {
            MESSAGE("dftracer_plugin or the duration_udfs plugin not found.");
            return;
        }
        dftu_utils_test::TestEnvironment env(50);
        REQUIRE(env.is_valid());
        dstd::String trace = env.create_dft_test_gzip_file(50);
        REQUIRE(!trace.empty());
        REQUIRE(make_dir(env.dir() + "/data"));
        REQUIRE(make_dir(env.dir() + "/tests"));
        REQUIRE(dstd::fs::rename(trace, env.dir() + "/data/trace.pfw.gz")
                    .has_value());

        const dstd::String golden = env.dir() + "/tests/basic.test";
        REQUIRE(write(golden,
                      "# a wrong expectation, recorded below\n"
                      "input ../data/trace.pfw.gz\n"
                      "query\n"
                      "select name\n"
                      "----\n"
                      "nope\n"));

        Output failed =
            run(tool, {"check", plugin, "--tests", env.dir() + "/tests"});
        CAPTURE(failed.text);
        CHECK(failed.exit_code != 0);
        CHECK(has(failed.text, "FAIL"));
        CHECK(has(failed.text, "- nope"));
        CHECK(has(failed.text, "+ "));
        CHECK(has(failed.text, "golden: 0 passed, 1 failed"));

        Output recorded = run(tool, {"check", plugin, "--tests",
                                     env.dir() + "/tests", "--record"});
        CAPTURE(recorded.text);
        CHECK(recorded.exit_code == 0);
        CHECK(has(recorded.text, "RECORDED"));
        const dstd::String rewritten = read(golden);
        CHECK(has(rewritten, "# a wrong expectation, recorded below"));
        CHECK(has(rewritten, "input ../data/trace.pfw.gz"));
        CHECK(!has(rewritten, "nope"));

        Output passed =
            run(tool, {"check", plugin, "--tests", env.dir() + "/tests"});
        CAPTURE(passed.text);
        CHECK(passed.exit_code == 0);
        CHECK(has(passed.text, "PASS"));
        CHECK(has(passed.text, "golden: 1 passed, 0 failed"));
    }

    TEST_CASE("--record leaves a block whose query failed alone and fails") {
        dstd::String tool = plugin_tool();
        dstd::String plugin = env_or_probe(
            "DFTRACER_UTILS_DURATION_UDFS_PLUGIN_PATH", "duration_udfs.so");
        if (tool.empty() || plugin.empty()) {
            MESSAGE("dftracer_plugin or the duration_udfs plugin not found.");
            return;
        }
        dftu_utils_test::TestEnvironment env(4);
        REQUIRE(env.is_valid());
        dstd::String trace = env.create_dft_test_gzip_file(4);
        REQUIRE(!trace.empty());
        REQUIRE(make_dir(env.dir() + "/data"));
        REQUIRE(make_dir(env.dir() + "/tests"));
        REQUIRE(dstd::fs::rename(trace, env.dir() + "/data/trace.pfw.gz")
                    .has_value());

        const dstd::String golden = env.dir() + "/tests/basic.test";
        REQUIRE(write(golden,
                      "# a query that fails, then one that runs\n"
                      "input ../data/trace.pfw.gz\n"
                      "query\n"
                      "select name | nonsense 2\n"
                      "----\n"
                      "kept\n"
                      "\n"
                      "query\n"
                      "select name | take 1\n"
                      "----\n"
                      "wrong\n"));

        Output recorded = run(tool, {"check", plugin, "--tests",
                                     env.dir() + "/tests", "--record"});
        CAPTURE(recorded.text);
        CHECK(recorded.exit_code != 0);
        CHECK(has(recorded.text, "FAIL"));
        CHECK(has(recorded.text, "RECORDED"));

        const dstd::String rewritten = read(golden);
        CAPTURE(rewritten);
        // The failed block kept its expected output and names the error; the
        // run that worked was recorded.
        CHECK(has(rewritten, "kept"));
        CHECK(!has(rewritten, "wrong"));
        CHECK(has(rewritten, "select name | nonsense 2"));
    }

    TEST_CASE("a check writes no .dftindex beside the input trace") {
        dstd::String tool = plugin_tool();
        dstd::String plugin = env_or_probe(
            "DFTRACER_UTILS_DURATION_UDFS_PLUGIN_PATH", "duration_udfs.so");
        if (tool.empty() || plugin.empty()) {
            MESSAGE("dftracer_plugin or the duration_udfs plugin not found.");
            return;
        }
        dftu_utils_test::TestEnvironment env(4);
        REQUIRE(env.is_valid());
        dstd::String trace = env.create_dft_test_gzip_file(4);
        REQUIRE(!trace.empty());
        REQUIRE(make_dir(env.dir() + "/data"));
        REQUIRE(make_dir(env.dir() + "/tests"));
        REQUIRE(dstd::fs::rename(trace, env.dir() + "/data/trace.pfw.gz")
                    .has_value());
        REQUIRE(write(env.dir() + "/tests/basic.test",
                      "input ../data/trace.pfw.gz\n"
                      "query error duql parse error\n"
                      "select name | nonsense 2\n"
                      "----\n"));

        Output checked =
            run(tool, {"check", plugin, "--tests", env.dir() + "/tests"});
        CAPTURE(checked.text);
        CHECK(checked.exit_code == 0);
        CHECK(has(checked.text, "PASS"));
        // The index the query built went to a temporary folder, so the user's
        // trace folder holds nothing new.
        auto index =
            dstd::fs::exists(dstd::PathView(env.dir() + "/data/.dftindex"));
        REQUIRE(index.has_value());
        CHECK(!*index);
    }
}
