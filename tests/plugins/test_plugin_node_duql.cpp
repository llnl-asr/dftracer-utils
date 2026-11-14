// SPDX-License-Identifier: MIT
// A duql `call` stage that names a registered plan node: the pipeline runs the
// node table to table, so the node's input is the columns the plan carries at
// that point (after a scan, after one select, after several) and its output is
// the schema the node declares.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdint>
#include <string>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;

#ifndef TOY_NODE_PLUGIN_PATH
#error "TOY_NODE_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

constexpr int EVENTS = 3;

dstd::String event(int i) {
    return R"({"ph":"X","name":"op)" + dstd::to_string(i % 3) +
           R"(","cat":"POSIX","pid":1,"tid":1,"ts":)" +
           dstd::to_string(1000 + 100 * i) + R"(,"dur":)" +
           dstd::to_string(10 + i) + R"(,"args":{}})";
}

ViewFile file_of(dftu_utils_test::TestEnvironment& env, int rows) {
    const dstd::String dir = env.dir() + "/f";
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw = dir + "/trace.pfw";
    {
        dftu_utils_test::FileOut o(pfw);
        for (int i = 0; i < rows; ++i) o << event(i) << "\n";
    }
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    dftu_utils_test::ok(
        dftu_utils_test::ok(
            trace::index::Indexer::open({dstd::String(dstd::StringView(gz))}))
            .build());
    ViewFile f;
    f.file_path = gz;
    f.index_path = trace::internal::determine_index_path(gz, "");
    return f;
}

dstd::Optional<plugins::host::Plugins> load(const char* path) {
    auto loaded = plugins::host::Plugins::Builder().add(path).build();
    if (!loaded) {
        MESSAGE("cannot load ", path, ": ", loaded.error().c_str());
        return dstd::nullopt;
    }
    return dstd::Optional<plugins::host::Plugins>(std::move(*loaded));
}

// "toy_node.dup" is also a node; a `call` naming it must reach this op.
dftu_dataframe* toy_dup(const dftu_dataframe* df, std::int64_t n) {
    return dftu_dataframe_head(df, n);
}

struct DupOp {
    DupOp() {
        const dftu_op_desc op{"toy_node.dup",
                              DFTU_OP_SIG(FRAME, FRAME, I64, NONE),
                              reinterpret_cast<dftu_op_generic_fn>(&toy_dup)};
        REQUIRE(dftu_op_register(&op) == 0);
    }
    ~DupOp() { dftu_op_unregister("toy_node.dup"); }
};

// The frame a pipeline gives, or the message it fails with, at build time or
// while it runs.
struct Ran {
    dstd::Optional<dataframe::DataFrame> frame;
    dstd::String message;
};

Ran run(const View& v) {
    Ran out;
    if (auto kept = v.status(); !kept.has_value()) {
        out.message = kept.error().c_str();
        return out;
    }
    auto got = default_runtime().submit(v.collect()).get();
    if (!got.has_value()) {
        out.message = got.error().c_str();
        return out;
    }
    for (dataframe::Series& c : got->columns) c = c.materialize();
    out.frame = std::move(*got);
    return out;
}

// The frame's column names, comma separated, in order.
dstd::String joined_names(const dataframe::DataFrame& df) {
    dstd::String out;
    for (const dstd::SharedString& n : df.names) {
        if (!out.empty()) out += ",";
        out += dstd::String(n.view());
    }
    return out;
}

// The Int64 values of column `name`, comma separated, in row order.
dstd::String ints(const dataframe::DataFrame& df, dstd::StringView name) {
    dstd::String out;
    for (std::size_t i = 0; i < df.names.size(); ++i) {
        if (dstd::StringView(df.names[i]) != name) continue;
        const dataframe::Series& s = df.columns[i];
        for (std::int64_t r = 0; r < s.length(); ++r) {
            if (!out.empty()) out += ",";
            out += dstd::to_string(s.data<std::int64_t>()[r]);
        }
    }
    return out;
}

bool says(const dstd::String& message, const char* text) {
    return dstd::StringView(message).find(text) != dstd::StringView::npos;
}

}  // namespace

TEST_SUITE("View duql plugin node") {
    TEST_CASE("a call names a node right after the scan") {
        auto plugins = load(TOY_NODE_PLUGIN_PATH);
        if (!plugins) return;

        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const View v = View::from_files({file_of(env, EVENTS)});

        Ran ran = run(v.duql("call toy_node.pass()"));
        REQUIRE(ran.frame.has_value());
        // The node's input is the columns the plan carries: the scan's own,
        // not a projection a stage narrowed to.
        CHECK(ran.frame->num_rows() == EVENTS);
        CHECK(ran.frame->names.size() > 2);
        CHECK(ints(*ran.frame, "dur") == "10,11,12");
    }

    TEST_CASE("a call names a node after one select") {
        auto plugins = load(TOY_NODE_PLUGIN_PATH);
        if (!plugins) return;

        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const View v = View::from_files({file_of(env, EVENTS)});

        Ran ran = run(v.duql("select name, dur | call toy_node.pass()"));
        REQUIRE(ran.frame.has_value());
        CHECK(joined_names(*ran.frame) == "name,dur");
        CHECK(ints(*ran.frame, "dur") == "10,11,12");
    }

    TEST_CASE("a call names a node after two selects") {
        auto plugins = load(TOY_NODE_PLUGIN_PATH);
        if (!plugins) return;

        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const View v = View::from_files({file_of(env, EVENTS)});

        Ran ran = run(v.duql(
            "select name, dur | select name, dur | call toy_node.pass()"));
        INFO(ran.message.c_str());
        REQUIRE(ran.frame.has_value());
        CHECK(joined_names(*ran.frame) == "name,dur");
        CHECK(ints(*ran.frame, "dur") == "10,11,12");
    }

    TEST_CASE("a call with an argument is refused: a node has no signature") {
        auto plugins = load(TOY_NODE_PLUGIN_PATH);
        if (!plugins) return;

        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const View v = View::from_files({file_of(env, EVENTS)});

        const Ran ran = run(v.duql("select name, dur | call toy_node.pass(1)"));
        CHECK_FALSE(ran.frame.has_value());
        CHECK(says(ran.message, "takes no arguments"));
        CHECK(says(ran.message, "toy_node.pass"));
    }

    TEST_CASE("a call naming no node and no op is refused") {
        auto plugins = load(TOY_NODE_PLUGIN_PATH);
        if (!plugins) return;

        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const View v = View::from_files({file_of(env, EVENTS)});

        const Ran ran = run(v.duql("call toy_node.nope()"));
        CHECK_FALSE(ran.frame.has_value());
        CHECK(says(ran.message, "toy_node.nope"));
        CHECK(says(ran.message, "Unknown function"));
    }

    TEST_CASE("an op of the same name wins over the node") {
        auto plugins = load(TOY_NODE_PLUGIN_PATH);
        if (!plugins) return;
        DupOp dup;

        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const View v = View::from_files({file_of(env, EVENTS)});

        Ran ran = run(v.duql("call toy_node.dup(1)"));
        INFO(ran.message.c_str());
        REQUIRE(ran.frame.has_value());
        CHECK(ran.frame->num_rows() == 1);

        // The op's own signature is what an argument-less call meets, not the
        // node's "takes no arguments".
        const Ran bare = run(v.duql("call toy_node.dup()"));
        CHECK_FALSE(bare.frame.has_value());
        CHECK(says(bare.message, "too few arguments"));
        CHECK_FALSE(says(bare.message, "takes no arguments"));
    }
}
