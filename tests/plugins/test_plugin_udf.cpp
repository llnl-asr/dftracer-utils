// SPDX-License-Identifier: MIT
// Scalar UDFs a plugin registers from its factory: duql calls them by name in
// select, derive and where, over whole batches. The host masks null rows unless
// the UDF is null aware, refuses a name that a built-in or another UDF holds,
// reports an exec failure to the caller, stops a slow exec on cancel, and keeps
// the plugin library loaded while a plan holds the call.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/udf.h>
#include <dftracer/utils/dataframe/udf_registry.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dlfcn.h>
#include <doctest/doctest.h>
#include <span_list.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <chrono>
#include <cstdint>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;

#ifndef UDF_PLUGIN_PATH
#error "UDF_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef UDF_CLASH_PLUGIN_PATH
#error "UDF_CLASH_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

constexpr int ROWS = 12;

dstd::String event(int ts, dstd::CStringView args, int dur) {
    return R"({"ph":1,"name":"op","cat":"POSIX","pid":1,"tid":1,"ts":)" +
           dstd::to_string(ts) + R"(,"dur":)" + dstd::to_string(dur) +
           R"(,"args":{)" + args.c_str() + "}}";
}

// Row i has ts 1000 + 100 i, dur 10 + i, and x = 3 i except in every fourth
// row, which has no x.
dstd::Vector<dstd::String> lines_of(int rows) {
    dstd::Vector<dstd::String> lines;
    for (int i = 0; i < rows; ++i)
        lines.push_back(event(
            1000 + 100 * i,
            i % 4 == 3 ? dstd::String() : R"("x":)" + dstd::to_string(3 * i),
            10 + i));
    return lines;
}

View view_of_lines(dftu_utils_test::TestEnvironment& env,
                   dstd::Span<const dstd::String> lines,
                   dstd::CStringView name) {
    const dstd::String pfw = env.dir() + "/" + name.c_str() + ".pfw";
    {
        dftu_utils_test::FileOut o(pfw);
        for (const auto& l : lines) o << l << "\n";
    }
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    dftu_utils_test::ok(
        dftu_utils_test::ok(
            trace::index::Indexer::open({dstd::String(dstd::StringView(gz))}))
            .build());
    return View::from_file(dstd::String(gz),
                           dstd::String(dstd::String(
                               trace::internal::determine_index_path(gz, ""))));
}

dstd::Result<dataframe::DataFrame> collect(const View& v) {
    return default_runtime().submit(v.collect()).get();
}

// The text of each cell of column `name`: a number, or `null`.
dstd::Vector<dstd::String> cells(const dataframe::DataFrame& f,
                                 dstd::CStringView name) {
    std::size_t c = 0;
    while (c < f.names.size() && f.names[c] != name) ++c;
    REQUIRE(c < f.names.size());
    const dataframe::Series col = f.columns[c].materialize();
    dstd::Vector<dstd::String> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r) {
        if (col.is_null(r)) {
            out.emplace_back("null");
            continue;
        }
        switch (col.type()) {
            case dataframe::TypeId::Int64:
                out.push_back(dstd::to_string(col.data<std::int64_t>()[r]));
                break;
            case dataframe::TypeId::Uint64:
                out.push_back(dstd::to_string(col.data<std::uint64_t>()[r]));
                break;
            case dataframe::TypeId::Float64:
                out.push_back(dstd::to_string(col.data<double>()[r]));
                break;
            case dataframe::TypeId::Bool:
                out.emplace_back((col.data<std::uint8_t>()[r >> 3] >> (r & 7)) &
                                         1
                                     ? "true"
                                     : "false");
                break;
            default:
                out.emplace_back(col.string_at(r));
        }
    }
    return out;
}

dstd::Vector<dstd::String> run(const View& v, dstd::CStringView name,
                               dstd::StringView query) {
    const View q = v.duql(query);
    auto got = collect(q);
    INFO(query);
    INFO(
        (got.has_value() ? dstd::String{} : dstd::String(got.error().c_str())));
    REQUIRE(got.has_value());
    return cells(*got, name);
}

// The failure message of the query, from building the view or running it.
dstd::String failure(const View& v, dstd::StringView query) {
    const View q = v.duql(query);
    if (const auto kept = q.status(); !kept.has_value())
        return kept.error().c_str();
    const auto got = collect(q);
    if (!got.has_value()) return got.error().c_str();
    return {};
}

bool has(dstd::StringView text, dstd::StringView word) {
    return text.find(word) != dstd::String::npos;
}

dstd::Optional<plugins::host::Plugins> load(dstd::StringView path) {
    auto built =
        plugins::host::Plugins::builder().add(dstd::String(path)).build();
    INFO((built.has_value() ? dstd::String{}
                            : dstd::String(built.error().message())));
    REQUIRE(built.has_value());
    return std::move(*built);
}

bool library_mapped() {
    void* h = dlopen(UDF_PLUGIN_PATH, RTLD_NOW | RTLD_NOLOAD);
    if (!h) return false;
    dlclose(h);
    return true;
}

std::int64_t counter(dstd::CStringView symbol) {
    void* h = dlopen(UDF_PLUGIN_PATH, RTLD_NOW | RTLD_NOLOAD);
    REQUIRE(h != nullptr);
    auto* fn = reinterpret_cast<std::int64_t (*)()>(dlsym(h, symbol.c_str()));
    REQUIRE(fn != nullptr);
    const std::int64_t value = fn();
    dlclose(h);
    return value;
}

dstd::Vector<dstd::String> expected(
    int rows, const dstd::Function<dstd::String(int)>& cell) {
    dstd::Vector<dstd::String> out;
    for (int i = 0; i < rows; ++i) out.push_back(cell(i));
    return out;
}

// An in-process UDF: x + 1, null where x is null (the host masks it).
template <bool Clash>
class PlusOneT {
   public:
    static constexpr const char* NAME = Clash ? "abs" : "cxx_plus1";

    static dstd::Result<dstd::UniquePtr<PlusOneT>> bind(
        dataframe::UdfBind& bind) {
        if (bind.type(0) != dataframe::TypeId::Int64 &&
            bind.type(0) != dataframe::TypeId::Uint64)
            return common::make_error<"cxx_plus1 takes an integer">(
                common::ErrorCode::InvalidArgument);
        bind.result(dataframe::TypeId::Int64);
        return dstd::make_unique<PlusOneT>();
    }

    dstd::Result<dataframe::Column> exec(dataframe::UdfBatch& batch) const {
        const dataframe::ColumnView arg = batch.arg(0);
        dstd::Vector<std::int64_t> out;
        if (arg.type() == dataframe::TypeId::Uint64) {
            const dstd::Span<const std::uint64_t> in =
                arg.values<std::uint64_t>();
            for (std::uint64_t i = 0; i < batch.rows(); ++i)
                out.push_back(
                    static_cast<std::int64_t>(in[static_cast<std::size_t>(i)]) +
                    1);
        } else {
            const dstd::Span<const std::int64_t> in =
                arg.values<std::int64_t>();
            for (std::uint64_t i = 0; i < batch.rows(); ++i)
                out.push_back(in[static_cast<std::size_t>(i)] + 1);
        }
        return dataframe::Column::from_int64(std::move(out));
    }
};

using PlusOne = PlusOneT<false>;
using PlusOneClash = PlusOneT<true>;

dstd::Vector<std::int64_t> eval_i64(const dataframe::Expr& e,
                                    const dataframe::Series& in) {
    auto got = dataframe::eval(
        e, dftu_utils_test::span_of<const dftracer::utils::dataframe::Series*>(
               {&in}));
    REQUIRE(got.has_value());
    dstd::Vector<std::int64_t> out;
    for (std::int64_t i = 0; i < got->length(); ++i)
        out.push_back(got->data<std::int64_t>()[i]);
    return out;
}

}  // namespace

TEST_CASE("a numeric UDF over two columns equals the built-in expression") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "add");
    auto set = load(UDF_PLUGIN_PATH);

    CHECK(run(v, "s", "derive s = uadd(dur, ts) | select s") ==
          run(v, "s", "derive s = dur + ts | select s"));
    CHECK(run(v, "s", "derive s = uadd(dur, ts) * 2 | select s") ==
          run(v, "s", "derive s = (dur + ts) * 2 | select s"));
    CHECK(run(v, "s", "select s = uadd(dur, 5)") ==
          expected(ROWS, [](int i) { return dstd::to_string(10 + i + 5); }));
}

TEST_CASE("bind picks the result type from the argument types") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "types");
    auto set = load(UDF_PLUGIN_PATH);

    auto ints = collect(v.duql("derive s = uadd(dur, 1) | select s"));
    REQUIRE(ints.has_value());
    CHECK(ints->columns[0].type() == dataframe::TypeId::Int64);
    auto floats = collect(v.duql("derive s = uadd(dur, 0.5) | select s"));
    REQUIRE(floats.has_value());
    CHECK(floats->columns[0].type() == dataframe::TypeId::Float64);
    CHECK(cells(*floats, "s")[0] == dstd::to_string(10.5));
}

TEST_CASE("a Bool UDF is a filter and a derived column") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "pred");
    auto set = load(UDF_PLUGIN_PATH);

    // As the first stage and after another stage.
    CHECK(run(v, "dur", "where uover(dur, 15) | select dur") ==
          run(v, "dur", "where dur > 15 | select dur"));
    CHECK(run(v, "dur", "derive d = dur | where uover(d, 15) | select dur") ==
          run(v, "dur", "where dur > 15 | select dur"));
    CHECK(run(v, "dur", "where uover(dur, 12) and dur < 18 | select dur") ==
          run(v, "dur", "where dur > 12 and dur < 18 | select dur"));
    CHECK(run(v, "dur", "where not uover(dur, 15) | select dur") ==
          run(v, "dur", "where dur <= 15 | select dur"));
    CHECK(run(v, "b", "derive b = uover(dur, 15) | select b") ==
          expected(ROWS, [](int i) { return i + 10 > 15 ? "true" : "false"; }));
    CHECK(run(v, "n", "where uover(dur, 15) | agg { n = count() }") ==
          dstd::Vector<dstd::String>{dstd::to_string(6)});
}

TEST_CASE("bind sees constant arguments and refuses one that is a column") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "consts");
    auto set = load(UDF_PLUGIN_PATH);

    const dstd::String why = failure(v, "where uover(dur, ts) | select dur");
    CHECK(has(why, "constant"));
    CHECK(has(why, "uover"));
    CHECK(failure(v, "where uover(dur, 15) | select dur").empty());
}

TEST_CASE("the host masks null rows unless the UDF is null aware") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "nulls");
    auto set = load(UDF_PLUGIN_PATH);

    CHECK(run(v, "y", "derive y = uplus1(x) | select y") ==
          expected(ROWS, [](int i) {
              return i % 4 == 3 ? dstd::String("null")
                                : dstd::to_string(3 * i + 1);
          }));
    CHECK(run(v, "y", "derive y = unull(x) | select y") ==
          expected(ROWS, [](int i) {
              return i % 4 == 3 ? dstd::String("-1") : dstd::to_string(3 * i);
          }));
    CHECK(run(v, "y", "derive y = uvol(x) + 0 | select y") ==
          expected(ROWS, [](int i) {
              return i % 4 == 3 ? dstd::String("null")
                                : dstd::to_string(3 * i + 1);
          }));
}

TEST_CASE("a name that a built-in or another UDF holds fails the load") {
    auto clash =
        plugins::host::Plugins::builder().add(UDF_CLASH_PLUGIN_PATH).build();
    REQUIRE_FALSE(clash.has_value());
    CHECK(has(dstd::String(clash.error().message()), "abs"));
    CHECK(has(dstd::String(clash.error().message()), "built-in"));

    auto first = load(UDF_PLUGIN_PATH);
    auto again = plugins::host::Plugins::builder().add(UDF_PLUGIN_PATH).build();
    REQUIRE_FALSE(again.has_value());
    CHECK(has(dstd::String(again.error().message()), "uadd"));
    CHECK(has(dstd::String(again.error().message()), "already"));
    // The refused load removed nothing of the first.
    CHECK(dataframe::find_udf("uadd") != nullptr);
}

TEST_CASE("an argument count outside min and max fails with a message") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "arity");
    auto set = load(UDF_PLUGIN_PATH);

    const dstd::String few = failure(v, "derive s = uadd(dur) | select s");
    CHECK(has(few, "uadd"));
    CHECK(has(few, "argument"));
    const dstd::String many =
        failure(v, "derive s = uadd(dur, ts, 1) | select s");
    CHECK(has(many, "uadd"));
    CHECK(has(many, "argument"));
}

TEST_CASE("an exec failure reaches the caller with its message") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "fail");
    auto set = load(UDF_PLUGIN_PATH);

    const dstd::String why = failure(v, "derive y = ufail(dur) | select y");
    CHECK(has(why, "ufail"));
    CHECK(has(why, "refused on purpose"));
}

TEST_CASE("a slow UDF stops within one step after a deadline") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(400), "slow");
    auto set = load(UDF_PLUGIN_PATH);

    // 400 rows in steps of 4 rows at 10 ms each take a second.
    const std::int64_t before = counter("udf_plugin_slow_steps");
    const auto start = dstd::steady_now();
    auto task =
        coro::with_timeout(v.duql("derive y = uslow(dur) | select y").collect(),
                           std::chrono::milliseconds(100));
    auto got = default_runtime().submit(std::move(task)).get();
    const auto elapsed = dstd::steady_now() - start;

    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().condition() == dstd::Condition::Timeout);
    CHECK(elapsed < std::chrono::milliseconds(800));
    const std::int64_t steps = counter("udf_plugin_slow_steps") - before;
    CHECK(steps < 100);

    // Nothing runs on after the error came back.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(counter("udf_plugin_slow_steps") - before == steps);
}

TEST_CASE("unloading the plugin removes the UDF and a plan keeps the library") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "lifetime");

    {
        auto set = load(UDF_PLUGIN_PATH);
        REQUIRE(dataframe::find_udf("uplus1") != nullptr);
        REQUIRE(library_mapped());
    }
    CHECK(dataframe::find_udf("uplus1") == nullptr);
    CHECK_FALSE(library_mapped());
    CHECK(has(failure(v, "derive y = uplus1(dur) | select y"), "uplus1"));

    dstd::Optional<plugins::host::Plugins> set = load(UDF_PLUGIN_PATH);
    dstd::Optional<View> plan = v.duql("derive y = uplus1(dur) | select y");
    REQUIRE(plan->status().has_value());
    set.reset();
    CHECK(dataframe::find_udf("uplus1") == nullptr);
    CHECK(library_mapped());
    {
        auto got = collect(*plan);
        REQUIRE(got.has_value());
        CHECK(cells(*got, "y") == expected(ROWS, [](int i) {
                  return dstd::to_string(10 + i + 1);
              }));
    }
    CHECK(library_mapped());

    plan.reset();
    for (int i = 0; i < 2000 && library_mapped(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK_FALSE(library_mapped());
}

TEST_CASE("a UDF is a join key function and a Bool UDF a join key") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    dstd::Vector<dstd::String> lines;
    for (int i = 0; i < 6; ++i)
        lines.push_back(R"({"type":"ev","i":)" + dstd::to_string(i) +
                        R"(,"k":)" + dstd::to_string(i % 3) + "}");
    for (int k = 1; k <= 3; ++k)
        lines.push_back(R"({"type":"run","rid":)" + dstd::to_string(k) +
                        R"(,"app":"app)" + dstd::to_string(k) + R"("})");
    for (const bool big : {false, true})
        lines.push_back(R"({"type":"cls","big":)" +
                        dstd::String(big ? "true" : "false") + R"(,"label":")" +
                        (big ? "large" : "small") + R"("})");
    const View v = view_of_lines(env, lines, "join");
    auto set = load(UDF_PLUGIN_PATH);

    const dstd::String runs =
        "let runs = where type == \"run\" | select rid, app; ";
    CHECK(run(v, "app",
              runs + "where type == \"ev\" | lookup runs on uadd(k, 1) == rid "
                     "| select app") ==
          run(v, "app",
              runs + "where type == \"ev\" | lookup runs on k + 1 == rid | "
                     "select app"));
    const dstd::String classes =
        "let cls = where type == \"cls\" | select big, label; ";
    CHECK(run(v, "label",
              classes + "where type == \"ev\" | lookup cls on uover(i, 2) == "
                        "big | select label") ==
          run(v, "label",
              classes + "where type == \"ev\" | lookup cls on (i > 2) == big | "
                        "select label"));
    // The predicate also filters the joined rows.
    CHECK(run(v, "i",
              runs + "where type == \"ev\" | lookup runs on k + 1 == rid | "
                     "where uover(i, 2) | select i") ==
          expected(3, [](int i) { return dstd::to_string(i + 3); }));
}

TEST_CASE("an exec that returns the wrong row count or type is an error") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "wrong");
    auto set = load(UDF_PLUGIN_PATH);

    const dstd::String rows = failure(v, "derive y = ubadrows(dur) | select y");
    CHECK(has(rows, "ubadrows"));
    CHECK(has(rows, "rows"));
    const dstd::String type = failure(v, "derive y = ubadtype(dur) | select y");
    CHECK(has(type, "ubadtype"));
    CHECK(has(type, "rows of"));
}

TEST_CASE("free_bound runs once for every successful bind") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "frees");
    auto set = load(UDF_PLUGIN_PATH);
    const std::int64_t binds = counter("udf_plugin_binds");
    const std::int64_t frees = counter("udf_plugin_frees");
    {
        run(v, "y", "derive y = uplus1(dur) | select y");
        run(v, "y", "derive y = uplus1(uplus1(dur)) | select y");
        failure(v, "derive y = uadd(name, dur) | select y");
        failure(v, "derive y = ubadrows(dur) | select y");
    }
    CHECK(counter("udf_plugin_binds") - binds ==
          counter("udf_plugin_frees") - frees);
    CHECK(counter("udf_plugin_binds") - binds >= 3);
}

TEST_CASE("a String UDF takes and gives strings") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "strings");
    auto set = load(UDF_PLUGIN_PATH);

    CHECK(run(v, "s", "derive s = ustr(name) | select s") ==
          expected(ROWS, [](int) { return dstd::String("op!"); }));
    CHECK(has(failure(v, "derive s = ustr(dur) | select s"), "string"));
}

TEST_CASE("unregistering while a plan runs lets the plan finish") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(60), "running");
    dstd::Optional<plugins::host::Plugins> set = load(UDF_PLUGIN_PATH);
    const std::int64_t before = counter("udf_plugin_slow_steps");

    auto task = default_runtime().submit(
        v.duql("derive y = uslow(dur) | select y").collect());
    for (int i = 0; i < 2000 && counter("udf_plugin_slow_steps") == before; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(counter("udf_plugin_slow_steps") > before);

    set.reset();
    CHECK(dataframe::find_udf("uslow") == nullptr);
    CHECK_FALSE(dataframe::udf("uslow").has_value());
    auto got = task.get();
    REQUIRE(got.has_value());
    CHECK(cells(*got, "y") ==
          expected(60, [](int i) { return dstd::to_string(10 + i); }));
}

namespace {

coro::CoroTask<dstd::Result<dataframe::Series>> eval_cancellable(
    dataframe::Expr e, const dataframe::Series* in) {
    co_await coro::yield();
    const coro::CancelView cancel = co_await coro::this_coro::cancel_view;
    const dataframe::UdfCancelScope scope(cancel);
    auto got = dataframe::eval(
        e, dftu_utils_test::span_of<const dftracer::utils::dataframe::Series*>(
               {in}));
    if (!got && got.error().condition() == dstd::Condition::Cancelled)
        co_await coro::stop_here();
    co_return got;
}

}  // namespace

TEST_CASE("a cancel reaches a UDF running in the parallel windows of eval") {
    // The plugin needs 17500 steps of 10 ms to finish. Valgrind runs the
    // timer and the windows so slowly that a cancel at 100 ms lands after
    // about 14 s and 2300 steps, so the bounds there only ask for a stop well
    // before the work is done.
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    constexpr auto MAX_CANCEL_TIME = std::chrono::milliseconds(60000);
    constexpr std::int64_t MAX_STEPS_AFTER_CANCEL = 8000;
#else
    constexpr auto MAX_CANCEL_TIME = std::chrono::milliseconds(2000);
    constexpr std::int64_t MAX_STEPS_AFTER_CANCEL = 300;
#endif
    struct SerialAfter {
        ~SerialAfter() { dataframe::set_parallel_backend(nullptr, nullptr); }
    } serial_after;
    dataframe::install_runtime_parallel_backend();
    auto set = load(UDF_PLUGIN_PATH);
    dstd::Vector<std::int64_t> xs(70000, 3);
    const auto series = dataframe::Series::flat_i64(xs.data(), 70000);
    auto call = dataframe::udf("uslow");
    REQUIRE(call.has_value());

    const std::int64_t before = counter("udf_plugin_slow_steps");
    const auto start = dstd::steady_now();
    auto task = coro::with_timeout(
        eval_cancellable((*call)(dataframe::col(0)), &series),
        std::chrono::milliseconds(100));
    auto got = default_runtime().submit(std::move(task)).get();
    const auto elapsed = dstd::steady_now() - start;

    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().condition() == dstd::Condition::Timeout);
    CHECK(elapsed < MAX_CANCEL_TIME);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const std::int64_t steps = counter("udf_plugin_slow_steps") - before;
    CHECK(steps < MAX_STEPS_AFTER_CANCEL);
}

#ifdef DURATION_UDFS_PLUGIN_PATH
TEST_CASE("the duration_udfs example registers two UDFs through the SDK") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "example");
    auto set = load(DURATION_UDFS_PLUGIN_PATH);

    CHECK(run(v, "ms", "derive ms = to_ms(dur) | select ms") ==
          expected(ROWS,
                   [](int i) { return dstd::to_string((10 + i) / 1000.0); }));
    CHECK(run(v, "dur", "where is_slow(dur, 15) | select dur") ==
          run(v, "dur", "where dur > 15 | select dur"));
    CHECK(has(failure(v, "where is_slow(dur, ts) | select dur"), "constant"));
    CHECK(has(failure(v, "where is_slow(dur) | select dur"), "argument"));
}
#endif

TEST_CASE("make_udf<T> serves duql, expressions and the op registry") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "inproc");
    auto reg = dataframe::make_udf<PlusOne>();
    REQUIRE(reg.has_value());
    CHECK(reg->name() == "cxx_plus1");

    const auto want =
        expected(ROWS, [](int i) { return dstd::to_string(10 + i + 1); });
    CHECK(run(v, "y", "derive y = cxx_plus1(dur) | select y") == want);

    const std::int64_t xs[] = {1, 2, 3};
    const auto series = dataframe::Series::flat_i64(xs, 3);
    auto call = dataframe::udf("cxx_plus1");
    REQUIRE(call.has_value());
    CHECK(eval_i64((*call)(dataframe::col(0)), series) ==
          dstd::Vector<std::int64_t>{2, 3, 4});
    CHECK(eval_i64((*call)(dataframe::col(0))*dataframe::lit(std::int64_t{2}),
                   series) == dstd::Vector<std::int64_t>{4, 6, 8});
    CHECK(dftu_op_find("cxx_plus1") != nullptr);
}

TEST_CASE("make_udf<T> reads a Uint64 argument as uint64") {
    // The trace view gives `dur` as a Uint64 column, and bind accepts one, so
    // exec must read it as uint64 rather than as an empty int64 span.
    auto reg = dataframe::make_udf<PlusOne>();
    REQUIRE(reg.has_value());

    const std::uint64_t big = 5'000'000'000ULL;
    const std::uint64_t wide[] = {1, big, 3};
    const auto uints =
        dataframe::Series::flat(dataframe::TypeId::Uint64, wide, 3);
    REQUIRE(uints.type() == dataframe::TypeId::Uint64);
    auto call = dataframe::udf("cxx_plus1");
    REQUIRE(call.has_value());
    dstd::Vector<std::int64_t> want;
    want.push_back(2);
    want.push_back(static_cast<std::int64_t>(big + 1));
    want.push_back(4);
    CHECK(eval_i64((*call)(dataframe::col(0)), uints) == want);

    // An Int64 column still reads as int64.
    const std::int64_t narrow[] = {1, 2, 3};
    const auto ints = dataframe::Series::flat_i64(narrow, 3);
    CHECK(eval_i64((*call)(dataframe::col(0)), ints) ==
          dstd::Vector<std::int64_t>{2, 3, 4});
}

TEST_CASE("udf(name) fails for an unknown name") {
    auto call = dataframe::udf("no_such_udf");
    REQUIRE_FALSE(call.has_value());
    CHECK(has(call.error().c_str(), "no_such_udf"));
}

TEST_CASE("a dead owning handle removes the name and a plan keeps the UDF") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of_lines(env, lines_of(ROWS), "scoped");
    dstd::Optional<dataframe::Udf> reg;
    {
        auto made = dataframe::make_udf<PlusOne>();
        REQUIRE(made.has_value());
        reg.emplace(std::move(*made));
    }
    const std::int64_t xs[] = {5};
    const auto series = dataframe::Series::flat_i64(xs, 1);
    auto call = dataframe::udf("cxx_plus1");
    REQUIRE(call.has_value());
    const dataframe::Expr held = (*call)(dataframe::col(0));
    const View plan = v.duql("derive y = cxx_plus1(dur) | select y");
    REQUIRE(plan.status().has_value());

    CHECK_FALSE(call->is_owning());
    {
        auto looked = dataframe::udf("cxx_plus1");
        REQUIRE(looked.has_value());
    }
    CHECK(dataframe::find_udf("cxx_plus1") != nullptr);

    reg.reset();
    CHECK(dataframe::find_udf("cxx_plus1") == nullptr);
    CHECK(dftu_op_find("cxx_plus1") == nullptr);
    CHECK_FALSE(dataframe::udf("cxx_plus1").has_value());
    CHECK(has(failure(v, "derive y = cxx_plus1(dur) | select y"), "cxx_plus1"));

    CHECK(eval_i64(held, series) == dstd::Vector<std::int64_t>{6});
    auto got = collect(plan);
    REQUIRE(got.has_value());
    CHECK(cells(*got, "y") ==
          expected(ROWS, [](int i) { return dstd::to_string(10 + i + 1); }));
}

TEST_CASE("make_udf<T> refuses a clash and a moved handle keeps one name") {
    auto first = dataframe::make_udf<PlusOne>();
    REQUIRE(first.has_value());
    auto again = dataframe::make_udf<PlusOne>();
    REQUIRE_FALSE(again.has_value());
    CHECK(has(again.error().c_str(), "cxx_plus1"));
    auto builtin = dataframe::make_udf<PlusOneClash>();
    REQUIRE_FALSE(builtin.has_value());

    {
        dataframe::Udf moved(std::move(*first));
        CHECK(first->name().empty());
        CHECK(moved.name() == "cxx_plus1");
        CHECK(dataframe::find_udf("cxx_plus1") != nullptr);
    }
    CHECK(dataframe::find_udf("cxx_plus1") == nullptr);
}

TEST_CASE("a plugin UDF is found through the op registry and unloads with it") {
    {
        auto set = load(UDF_PLUGIN_PATH);
        CHECK(dftu_op_find("uplus1") != nullptr);
        auto call = dataframe::udf("uplus1");
        CHECK(call.has_value());
    }
    CHECK(dftu_op_find("uplus1") == nullptr);
}
