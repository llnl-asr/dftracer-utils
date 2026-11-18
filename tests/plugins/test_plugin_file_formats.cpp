// SPDX-License-Identifier: MIT
// File formats (dftu.svc.file_formats, dftu_file_format_vt): a plugin's format
// claims a file, opens it, declares its columns, and hands the engine one
// cursor per chunk. Drives the real dlopen path through Plugins::Builder
// against toy_format_plugin, then reads the rows through LazyFrame::scan_file
// and through the C ABI.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/file_format_registry.h>
#include <dftracer/utils/dataframe/internal/sink_plan.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dlfcn.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <cstdio>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

#ifndef TOY_FORMAT_PLUGIN_PATH
#error "TOY_FORMAT_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef TOY_FORMAT_TIE_PLUGIN_PATH
#error "TOY_FORMAT_TIE_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

// Six rows, so both chunks of the toy format hold some.
const char TOY_ROWS[] =
    "TOY1\n"
    "1 alpha\n"
    "2 beta\n"
    "3 gamma\n"
    "4 delta\n"
    "5 epsilon\n"
    "6 zeta\n";

dstd::String write_temp(dstd::CStringView name, dstd::CStringView text) {
    std::FILE* f = std::fopen(name.c_str(), "wb");
    REQUIRE(f != nullptr);
    const std::size_t n = std::fwrite(text.c_str(), 1, text.size(), f);
    std::fclose(f);
    REQUIRE(n == text.size());
    return dstd::String(name);
}

dstd::Optional<plugins::host::Plugins> load(dstd::StringView path) {
    auto result =
        plugins::host::Plugins::Builder().add(dstd::String(path)).build();
    REQUIRE(result.has_value());
    return std::move(result).value();
}

dstd::Optional<plugins::host::Plugins> load_both() {
    auto result = plugins::host::Plugins::Builder()
                      .add(TOY_FORMAT_PLUGIN_PATH)
                      .add(TOY_FORMAT_TIE_PLUGIN_PATH)
                      .build();
    REQUIRE(result.has_value());
    return std::move(result).value();
}

dstd::Result<DataFrame> run(const LazyFrame& lf) {
    return default_runtime().submit(lf.collect()).get();
}

// A collected frame of several chunks holds CHUNKED columns, so every read
// materializes first.
Series flat_column(const DataFrame& df, dstd::CStringView name) {
    Series column = df.column(name);
    REQUIRE(column.valid());
    if (!column.is_flat()) column = column.materialize();
    return column;
}

dstd::Vector<std::int64_t> i64_column(const DataFrame& df,
                                      dstd::CStringView name) {
    const Series column = flat_column(df, name);
    REQUIRE(column.type() == TypeId::Int64);
    const std::int64_t* data = column.data<std::int64_t>();
    REQUIRE(data != nullptr);
    return dstd::Vector<std::int64_t>(data, data + column.length());
}

dstd::Vector<dstd::String> str_column(const DataFrame& df,
                                      dstd::CStringView name) {
    const Series column = flat_column(df, name);
    REQUIRE(column.type() == TypeId::String);
    dstd::Vector<dstd::String> out;
    for (std::int64_t i = 0; i < column.length(); ++i) {
        std::int64_t len = 0;
        const char* at = dftu_series_string_at(column.handle(), i, &len);
        REQUIRE(at != nullptr);
        out.emplace_back(at, static_cast<std::size_t>(len));
    }
    return out;
}

// The test keeps its own reference to each toy plugin for the whole run, so the
// library stays mapped after ~Plugins drops the host's reference and the
// counters below still see what the plugin wrote while it ran.
template <class Getter>
Getter plugin_symbol(dstd::CStringView path, dstd::CStringView symbol) {
    static dstd::Vector<void*> handles;
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    REQUIRE(handle != nullptr);
    handles.push_back(handle);
    auto getter = reinterpret_cast<Getter>(dlsym(handle, symbol.c_str()));
    REQUIRE(getter != nullptr);
    return getter;
}

const std::int64_t* toy_stats() {
    static std::int64_t values[5] = {0, 0, 0, 0, 0};
    plugin_symbol<void (*)(std::int64_t*)>(TOY_FORMAT_PLUGIN_PATH,
                                           "toy_format_stats")(values);
    return values;
}

const std::int64_t* toy_sink_stats() {
    static std::int64_t values[6] = {0, 0, 0, 0, 0, 0};
    plugin_symbol<void (*)(std::int64_t*)>(TOY_SINK_PLUGIN_PATH,
                                           "toy_sink_stats")(values);
    return values;
}

std::int64_t toy_sink_row_count() {
    return plugin_symbol<std::int64_t (*)()>(TOY_SINK_PLUGIN_PATH,
                                             "toy_sink_row_count")();
}

const std::int64_t* toy_sink_rows() {
    return plugin_symbol<const std::int64_t* (*)()>(TOY_SINK_PLUGIN_PATH,
                                                    "toy_sink_rows")();
}

dstd::StringView toy_sink_option(dstd::CStringView symbol) {
    return plugin_symbol<const char* (*)()>(TOY_SINK_PLUGIN_PATH, symbol)();
}

void remove_file(dstd::CStringView name) { std::remove(name.c_str()); }

}  // namespace

TEST_CASE("scan_file reads every chunk of a file its plugin's format claims") {
    dstd::Optional<plugins::host::Plugins> plugins =
        load(TOY_FORMAT_PLUGIN_PATH);
    const dstd::String path = write_temp("toy_rows.toy", TOY_ROWS);

    DataFrame df = dftu_utils_test::ok(run(LazyFrame::scan_file(path)));
    CHECK(df.num_rows() == 6);
    CHECK(i64_column(df, "id") == dstd::Vector<std::int64_t>{1, 2, 3, 4, 5, 6});
    CHECK(str_column(df, "name") ==
          dstd::Vector<dstd::String>{"alpha", "beta", "gamma", "delta",
                                     "epsilon", "zeta"});
    remove_file(path.c_str());
}

TEST_CASE("a projection reaches the format and drops the other column") {
    dstd::Optional<plugins::host::Plugins> plugins =
        load(TOY_FORMAT_PLUGIN_PATH);
    const dstd::String path = write_temp("toy_projection.toy", TOY_ROWS);

    DataFrame df =
        dftu_utils_test::ok(run(LazyFrame::scan_file(path).select({"name"})));
    CHECK(df.num_columns() == 1);
    CHECK(df.names[0] == "name");
    CHECK(df.num_rows() == 6);
    remove_file(path.c_str());
}

TEST_CASE("a filter the format absorbs gives the same rows as the engine") {
    dstd::Optional<plugins::host::Plugins> plugins =
        load(TOY_FORMAT_PLUGIN_PATH);
    const dstd::String path = write_temp("toy_filter.toy", TOY_ROWS);

    const Expr predicate = col(0) > std::int64_t{3};
    DataFrame pushed =
        dftu_utils_test::ok(run(LazyFrame::scan_file(path).filter(predicate)));
    CHECK(i64_column(pushed, "id") == dstd::Vector<std::int64_t>{4, 5, 6});
    remove_file(path.c_str());
}

TEST_CASE("a file no format claims is an error naming the path") {
    dstd::Optional<plugins::host::Plugins> plugins =
        load(TOY_FORMAT_PLUGIN_PATH);
    const dstd::String path = write_temp("toy_unknown.bin", "NOPE\n1 x\n");

    LazyFrame lf = LazyFrame::scan_file(path);
    REQUIRE(!lf.status().has_value());
    const dstd::String message = lf.status().error().c_str();
    CHECK(message.find("no format claims") != dstd::String::npos);
    CHECK(message.find(path.c_str()) != dstd::String::npos);
    CHECK(!run(lf).has_value());
    remove_file(path.c_str());
}

TEST_CASE("two formats claiming one file is an error naming both") {
    dstd::Optional<plugins::host::Plugins> plugins = load_both();
    const dstd::String path = write_temp("toy_tie.toy", TOY_ROWS);

    LazyFrame lf = LazyFrame::scan_file(path);
    REQUIRE(!lf.status().has_value());
    const dstd::String message = lf.status().error().c_str();
    CHECK(message.find("toy_format.rows") != dstd::String::npos);
    CHECK(message.find("toy_format_tie.rows") != dstd::String::npos);
    CHECK(!run(lf).has_value());
    remove_file(path.c_str());
}

TEST_CASE("a format's open error reaches the caller with its message") {
    dstd::Optional<plugins::host::Plugins> plugins =
        load(TOY_FORMAT_PLUGIN_PATH);
    const dstd::String path = write_temp("toy_fail.toy", "FAIL\n1 x\n");

    LazyFrame lf = LazyFrame::scan_file(path);
    REQUIRE(!lf.status().has_value());
    const dstd::String message = lf.status().error().c_str();
    CHECK(message.find("first line says FAIL") != dstd::String::npos);
    remove_file(path.c_str());
}

TEST_CASE("a format's scan error reaches the caller with its message") {
    dstd::Optional<plugins::host::Plugins> plugins =
        load(TOY_FORMAT_PLUGIN_PATH);
    const dstd::String path =
        write_temp("toy_scan_fail.toy", "TOY1\n1 SCANFAIL\n2 ok\n");

    dstd::Result<DataFrame> collected = run(LazyFrame::scan_file(path));
    REQUIRE(!collected.has_value());
    const dstd::String message = collected.error().c_str();
    CHECK(message.find("SCANFAIL") != dstd::String::npos);
    remove_file(path.c_str());
}

TEST_CASE("a plan outlives the plugin set that registered its format") {
    const dstd::String path = write_temp("toy_unload.toy", TOY_ROWS);
    LazyFrame lf = LazyFrame::scan_file(path + ".missing");
    {
        dstd::Optional<plugins::host::Plugins> plugins =
            load(TOY_FORMAT_PLUGIN_PATH);
        lf = LazyFrame::scan_file(path);
        REQUIRE(lf.status().has_value());
    }  // ~Plugins unregisters the format and drops its library reference.
    // The plan holds the open file, so a fresh scan no longer finds a format
    // while this plan still reads its rows.
    DataFrame df = dftu_utils_test::ok(run(lf));
    CHECK(i64_column(df, "id") == dstd::Vector<std::int64_t>{1, 2, 3, 4, 5, 6});
    CHECK(!LazyFrame::scan_file(path).status().has_value());
    remove_file(path.c_str());
}

TEST_CASE("the C ABI scans a file through its registered format") {
    dstd::Optional<plugins::host::Plugins> plugins =
        load(TOY_FORMAT_PLUGIN_PATH);
    const dstd::String path = write_temp("toy_c_abi.toy", TOY_ROWS);

    dftu_lazyframe* lf = nullptr;
    REQUIRE(dftu_lazyframe_scan_file(path.c_str(), &lf) == 0);
    REQUIRE(lf != nullptr);
    dftu_dataframe* out = dftu_lazyframe_collect(lf, -1);
    REQUIRE(out != nullptr);
    CHECK(dftu_dataframe_num_rows(out) == 6);
    dftu_dataframe_free(out);
    dftu_lazyframe_free(lf);

    CHECK(dftu_lazyframe_scan_file(nullptr, &lf) != 0);
    CHECK(dftu_lazyframe_scan_file(path.c_str(), nullptr) != 0);
    remove_file(path.c_str());
}

TEST_CASE("register_format refuses a reserved and a duplicate name") {
    dstd::Optional<plugins::host::Plugins> plugins =
        load(TOY_FORMAT_PLUGIN_PATH);
    void* handle = dlopen(TOY_FORMAT_PLUGIN_PATH, RTLD_NOW | RTLD_LOCAL);
    REQUIRE(handle != nullptr);
    auto gate_ok =
        reinterpret_cast<int (*)()>(dlsym(handle, "toy_format_gate_ok"));
    REQUIRE(gate_ok != nullptr);
    CHECK(gate_ok() == 1);
    dlclose(handle);
}

TEST_CASE("a limit crossing two chunks returns exactly that many rows") {
    dstd::Optional<plugins::host::Plugins> plugins =
        load(TOY_FORMAT_PLUGIN_PATH);
    const dstd::String path = write_temp("toy_limit.toy", TOY_ROWS);

    // Three rows in each chunk, so four rows need both chunks.
    DataFrame df = dftu_utils_test::ok(run(LazyFrame::scan_file(path).head(4)));
    CHECK(df.num_rows() == 4);
    CHECK(i64_column(df, "id") == dstd::Vector<std::int64_t>{1, 2, 3, 4});
    remove_file(path.c_str());
}

TEST_CASE("the format with the highest score opens the file") {
    // The plugin registers a second format on the same magic that scores 10
    // and whose open() is a trap, so this reads rows only if 90 won.
    dstd::Optional<plugins::host::Plugins> plugins =
        load(TOY_FORMAT_PLUGIN_PATH);
    const dstd::String path = write_temp("toy_highest.toy", TOY_ROWS);

    DataFrame df = dftu_utils_test::ok(run(LazyFrame::scan_file(path)));
    CHECK(df.num_rows() == 6);
    remove_file(path.c_str());
}

TEST_CASE("a chunk that votes its filters differently fails the scan") {
    const dstd::String path =
        write_temp("toy_votes.toy", "TOYV\n1 a\n2 b\n3 c\n4 d\n");
    dstd::Optional<plugins::host::Plugins> plugins =
        load(TOY_FORMAT_PLUGIN_PATH);

    dstd::Result<DataFrame> collected =
        run(LazyFrame::scan_file(path).filter(col(0) > std::int64_t{0}));
    REQUIRE(!collected.has_value());
    const dstd::String message = collected.error().c_str();
    CHECK(message.find("must report the same votes") != dstd::String::npos);
    remove_file(path.c_str());
}

TEST_CASE("every cursor a refused chunk scan opened is destroyed") {
    const dstd::String path =
        write_temp("toy_votes_stats.toy", "TOYV\n1 a\n2 b\n3 c\n4 d\n");
    dstd::Optional<plugins::host::Plugins> plugins =
        load(TOY_FORMAT_PLUGIN_PATH);
    const std::int64_t* base = toy_stats();
    const std::int64_t made_before = base[0];
    const std::int64_t killed_before = base[1];
    const std::int64_t opens_before = base[2];
    dstd::Result<DataFrame> refused =
        run(LazyFrame::scan_file(path).filter(col(0) > std::int64_t{0}));
    CHECK(!refused.has_value());
    // Chunk 0's cursor and the refused chunk 1's cursor were both made, and
    // the host destroyed each before releasing its own host handle.
    CHECK(toy_stats()[0] - made_before == 2);
    CHECK(toy_stats()[1] - killed_before == 2);
    CHECK(toy_stats()[2] - opens_before == 1);
    remove_file(path.c_str());
}

TEST_CASE("close and destroy run once per open and once per unload") {
    const dstd::String path = write_temp("toy_counts.toy", TOY_ROWS);
    const std::int64_t destroyed_before = toy_stats()[4];
    {
        dstd::Optional<plugins::host::Plugins> plugins =
            load(TOY_FORMAT_PLUGIN_PATH);
        const std::int64_t opens_before = toy_stats()[2];
        const std::int64_t closes_before = toy_stats()[3];
        const std::int64_t made_before = toy_stats()[0];
        const std::int64_t killed_before = toy_stats()[1];
        {
            DataFrame df = dftu_utils_test::ok(run(LazyFrame::scan_file(path)));
            CHECK(df.num_rows() == 6);
        }
        CHECK(toy_stats()[2] - opens_before == 1);   // opened once
        CHECK(toy_stats()[3] - closes_before == 1);  // and closed once
        CHECK(toy_stats()[0] - made_before == toy_stats()[1] - killed_before);
    }
    // The plugin registers two formats, so unloading destroys two states.
    CHECK(toy_stats()[4] - destroyed_before == 2);
    remove_file(path.c_str());
}

TEST_CASE("a sink writer is released on finalize and on abort") {
    dstd::Optional<plugins::host::Plugins> plugins = load(TOY_SINK_PLUGIN_PATH);
    dstd::SharedPtr<dataframe::SinkFactory> sink =
        dataframe::find_sink("toy_sink.out");
    REQUIRE(sink != nullptr);

    dstd::Vector<dstd::String> names;
    names.emplace_back("id");
    dstd::Vector<TypeId> types;
    types.push_back(TypeId::Int64);
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options;
    options.emplace_back("path", "out.toy");

    std::int64_t base[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 6; ++i) base[i] = toy_sink_stats()[i];

    std::int64_t ids[2] = {1, 2};
    DataFrame batch;
    batch.names.emplace_back("id");
    batch.columns.push_back(Series::flat_i64(ids, 2));

    auto first = sink->bind(names, types, options);
    REQUIRE(first.has_value());
    dstd::UniquePtr<dataframe::SinkWriter> writer = std::move(*first);
    CHECK(writer->write(batch).has_value());
    REQUIRE(writer->finalize().has_value());
    // A second finalize is the caller's error, never a call with no handle.
    CHECK(!writer->finalize().has_value());
    writer.reset();

    auto second = sink->bind(names, types, options);
    REQUIRE(second.has_value());
    dstd::UniquePtr<dataframe::SinkWriter> aborted = std::move(*second);
    aborted->abort();
    aborted.reset();

    const std::int64_t* now = toy_sink_stats();
    CHECK(now[0] - base[0] == 2);  // two writers built
    CHECK(now[1] - base[1] == 2);  // and both destroyed
    CHECK(now[2] - base[2] == 1);  // one frame written
    CHECK(now[3] - base[3] == 1);  // one finalized
    CHECK(now[4] - base[4] == 1);  // one aborted
    CHECK(now[5] == 1);            // the bound column count arrived
    CHECK(dstd::StringView(toy_sink_option("toy_sink_option_key")) == "path");
    CHECK(dstd::StringView(toy_sink_option("toy_sink_option_value")) ==
          "out.toy");
}

TEST_CASE("a sink without write is refused and named") {
    auto built =
        plugins::host::Plugins::Builder().add(TOY_BAD_SINK_PLUGIN_PATH).build();
    REQUIRE(!built.has_value());
    const dstd::String message = built.error().c_str();
    CHECK(message.find("toy_bad_sink.out") != dstd::String::npos);
    CHECK(message.find("write") != dstd::String::npos);
}

TEST_CASE("LazyFrame::sink writes every row in order to the plugin sink") {
    dstd::Optional<plugins::host::Plugins> plugins = load(TOY_SINK_PLUGIN_PATH);
    std::int64_t ids[3] = {7, 8, 9};
    DataFrame frame;
    frame.names.emplace_back("id");
    frame.columns.push_back(Series::flat_i64(ids, 3));

    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options;
    options.emplace_back("path", "sink.csv");
    const std::int64_t written_before = toy_sink_stats()[2];
    const std::int64_t aborted_before = toy_sink_stats()[4];

    const std::int64_t rows_before = toy_sink_row_count();
    LazyFrame lf = frame.lazy();
    const SinkSummary summary = dftu_utils_test::ok(
        default_runtime().submit(lf.sink("toy_sink.out", options)).get());
    CHECK(summary.rows == 3);
    CHECK(summary.batches == 1);
    CHECK(toy_sink_stats()[2] - written_before == 1);  // one frame written
    CHECK(toy_sink_stats()[4] == aborted_before);      // and none aborted
    CHECK(toy_sink_stats()[5] == 1);                   // the plan's columns
    CHECK(toy_sink_row_count() - rows_before == 3);
    const std::int64_t* rows = toy_sink_rows() + rows_before;
    CHECK(rows[0] == 7);
    CHECK(rows[1] == 8);
    CHECK(rows[2] == 9);
    CHECK(dstd::StringView(toy_sink_option("toy_sink_option_key")) == "path");
    CHECK(dstd::StringView(toy_sink_option("toy_sink_option_value")) ==
          "sink.csv");
}

TEST_CASE("a sink whose write fails aborts the writer once") {
    dstd::Optional<plugins::host::Plugins> plugins = load(TOY_SINK_PLUGIN_PATH);
    const std::int64_t made_before = toy_sink_stats()[0];
    const std::int64_t aborted_before = toy_sink_stats()[4];
    const std::int64_t finalized_before = toy_sink_stats()[3];

    std::int64_t ids[2] = {1, 2};
    DataFrame frame;
    frame.names.emplace_back("id");
    frame.columns.push_back(Series::flat_i64(ids, 2));
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options;
    options.emplace_back("fail", "0");

    LazyFrame lf = frame.lazy();
    const auto failed =
        default_runtime().submit(lf.sink("toy_sink.out", options)).get();
    REQUIRE(!failed.has_value());
    CHECK(toy_sink_stats()[0] - made_before == 1);     // one writer built
    CHECK(toy_sink_stats()[4] - aborted_before == 1);  // aborted once
    CHECK(toy_sink_stats()[3] == finalized_before);    // never finalized
}

TEST_CASE("an unknown sink name names the registered sinks") {
    dstd::Optional<plugins::host::Plugins> plugins = load(TOY_SINK_PLUGIN_PATH);
    DataFrame frame;
    frame.names.emplace_back("id");
    const std::int64_t ids[1] = {1};
    frame.columns.push_back(Series::flat_i64(ids, 1));

    LazyFrame lf = frame.lazy();
    const auto failed = default_runtime().submit(lf.sink("no_such.sink")).get();
    REQUIRE(!failed.has_value());
    const dstd::String message = failed.error().c_str();
    CHECK(message.find("no sink named 'no_such.sink'") != dstd::String::npos);
    CHECK(message.find("toy_sink.out") != dstd::String::npos);
}

TEST_CASE("the C ABI sink writes the same rows and reports the same totals") {
    dstd::Optional<plugins::host::Plugins> plugins = load(TOY_SINK_PLUGIN_PATH);
    const std::int64_t ids[2] = {4, 5};
    const char* names[1] = {"id"};
    dftu_series* columns[1] = {nullptr};
    columns[0] = dftu_series_new_flat(DFTU_TYPE_INT64, ids, 2, nullptr);
    REQUIRE(columns[0] != nullptr);
    dftu_dataframe* source = dftu_dataframe_new(names, columns, 1);
    REQUIRE(source != nullptr);
    dftu_lazyframe* lf = dftu_dataframe_lazy(source);
    REQUIRE(lf != nullptr);

    const char* keys[1] = {"path"};
    const char* values[1] = {"abi.csv"};
    std::int64_t rows = -1;
    std::int64_t batches = -1;
    CHECK(dftu_lazyframe_sink(lf, "toy_sink.out", keys, values, 1, &rows,
                              &batches) == 0);
    CHECK(rows == 2);
    CHECK(batches == 1);
    const std::int64_t* written = toy_sink_rows() + toy_sink_row_count() - 2;
    CHECK(written[0] == 4);
    CHECK(written[1] == 5);

    // A missing name fails and writes nothing through the out pointers.
    std::int64_t untouched_rows = -1;
    CHECK(dftu_lazyframe_sink(lf, "no_such.sink", nullptr, nullptr, 0,
                              &untouched_rows, &batches) != 0);
    CHECK(untouched_rows == -1);
    CHECK(dftu_lazyframe_sink(nullptr, "toy_sink.out", nullptr, nullptr, 0,
                              &rows, &batches) != 0);
    CHECK(dftu_lazyframe_sink(lf, nullptr, nullptr, nullptr, 0, &rows,
                              &batches) != 0);

    dftu_lazyframe_free(lf);
    dftu_dataframe_free(source);
}

namespace {

// A plan of one good frame, then a failure: the second batch never comes.
class FailingSource final : public Source {
   public:
    class FailingCursor final : public Cursor {
       public:
        coro::CoroTask<MorselResult> next(std::int64_t) override {
            if (served_) {
                co_return common::make_error<"scripted source: batch 1 failed">(
                    common::ErrorCode::Io);
            }
            served_ = true;
            std::int64_t value = 1;
            Morsel morsel;
            morsel.rows = 1;
            morsel.batch_index = 0;
            morsel.columns.push_back(Series::flat_i64(&value, 1));
            co_return morsel;
        }

       private:
        bool served_ = false;
    };

    Schema schema() const override {
        Schema s;
        s.fields.push_back(
            Field{dstd::String("id"), scalar(TypeId::Int64), false});
        return s;
    }

    dstd::Result<ScanResult> scan(const ScanRequest& request) const override {
        ScanResult r;
        r.filters.assign(request.filters.size(), Pushed::No);
        r.cursor = dstd::make_unique<FailingCursor>();
        return r;
    }
};

}  // namespace

TEST_CASE("a stream that fails after one good frame aborts the sink once") {
    dstd::Optional<plugins::host::Plugins> plugins = load(TOY_SINK_PLUGIN_PATH);
    const std::int64_t written_before = toy_sink_stats()[2];
    const std::int64_t finalized_before = toy_sink_stats()[3];
    const std::int64_t aborted_before = toy_sink_stats()[4];

    LazyFrame failing = LazyFrame::scan(dstd::make_shared<FailingSource>());
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options;
    options.emplace_back("path", "bad.csv");
    const auto got =
        default_runtime().submit(failing.sink("toy_sink.out", options)).get();
    REQUIRE(!got.has_value());
    CHECK(dstd::StringView(got.error().c_str()).find("batch 1 failed") !=
          dstd::StringView::npos);
    CHECK(toy_sink_stats()[2] - written_before == 1);  // the good frame
    CHECK(toy_sink_stats()[4] - aborted_before == 1);  // aborted once
    CHECK(toy_sink_stats()[3] == finalized_before);    // never finalized
}

TEST_CASE("a second run of one sink plan writes and counts again") {
    dstd::Optional<plugins::host::Plugins> plugins = load(TOY_SINK_PLUGIN_PATH);
    std::int64_t ids[4] = {1, 2, 3, 4};
    DataFrame frame;
    frame.names.emplace_back("id");
    frame.columns.push_back(Series::flat_i64(ids, 4));

    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options;
    options.emplace_back("path", "twice.csv");
    auto counts = dstd::make_shared<SinkSummary>();
    auto wrapped = sink_plan(frame.lazy(), "toy_sink.out", options, 2, counts);
    REQUIRE(wrapped.has_value());

    const std::int64_t rows_before = toy_sink_row_count();
    for (int pass = 0; pass < 2; ++pass) {
        DataFrame totals = dftu_utils_test::ok(run(*wrapped));
        CHECK(totals.num_rows() == 1);
        // The second run replaces the totals; it does not add to them.
        CHECK(counts->rows == 4);
        CHECK(counts->batches == 2);
    }
    // The sink saw both runs, over two binds.
    CHECK(toy_sink_row_count() - rows_before == 8);
}
