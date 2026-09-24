#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace {

constexpr int RECORDS = 3000;

// Records of the sv_log schema, in time order: `ts_ms` is 1000 + i (absent
// for every 100th record), `took` is (i % 7) * 0.5 seconds, `worker` cycles
// w0..w2, `status` is "n/a" for every 11th record and an integer otherwise,
// and `tags` is an array written with and without spaces.
std::string write_log(TestEnvironment& env) {
    const std::string plain = env.get_dir() + "/log.ndjson";
    {
        std::ofstream out(plain);
        for (int i = 0; i < RECORDS; ++i) {
            out << R"({"op":")" << (i % 2 ? "read" : "write") << '"';
            if (i % 100 != 99) out << R"(,"ts_ms":)" << 1000 + i;
            out << R"(,"took":)" << (i % 7) * 0.5 << R"(,"worker":"w)" << i % 3
                << '"';
            if (i % 11 == 0)
                out << R"(,"status":"n/a")";
            else
                out << R"(,"status":)" << (i % 2 ? 200 : 404);
            if (i % 4 == 0)
                out << R"(,"tags":[ "a" , "b" ])";
            else if (i % 4 == 1)
                out << R"(,"tags":["a","b"])";
            else if (i % 4 == 2)
                out << R"(,"tags":[1,2])";
            else
                out << R"(,"tags":[1, 2.0])";
            out << "}\n";
        }
    }
    const std::string gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    fs::remove(plain);
    dftracer::utils::index::Indexer::open({gz}).build();
    return gz;
}

void register_log_schema() {
    dftracer::utils::index::register_schema(
        "id: sv_log\n"
        "fields:\n"
        "  op: {type: string}\n"
        "  ts_ms: {type: int, role: time, unit: ms, optional: true}\n"
        "  took: {type: float, role: duration, unit: s}\n"
        "  worker: {type: string, role: entity}\n"
        "  status: {type: int, optional: true}\n"
        "  tags: {type: json, optional: true}\n"
        "  host: {type: string, path: meta.host, optional: true}\n",
        "test_schema_view");
}

View view_of(const std::string& gz) {
    return View::from_file(gz, determine_index_path(gz, ""));
}

std::size_t rows(const View& v) { return v.collect().get().num_rows(); }

}  // namespace

TEST_SUITE("SchemaView") {
    TEST_CASE("declared types fix the column types") {
        register_log_schema();
        TestEnvironment env(10);
        const auto gz = write_log(env);
        std::map<std::string, std::string> types;
        for (const auto& c : view_of(gz).column_info()) types[c.name] = c.type;
        CHECK(types["status"] == "int64");
        CHECK(types["took"] == "float64");
        CHECK(types["tags"] == "string");

        const auto df = view_of(gz).select({"status"}).collect().get();
        REQUIRE(df.num_rows() == RECORDS);
        CHECK(df.columns[0].type() == dataframe::TypeId::Int64);
        CHECK(df.columns[0].null_count() == (RECORDS + 10) / 11);
    }

    TEST_CASE("a json field reads and compares as canonical text") {
        register_log_schema();
        TestEnvironment env(10);
        const auto gz = write_log(env);
        const auto df = view_of(gz).select({"tags"}).collect().get();
        REQUIRE(df.num_rows() == RECORDS);
        std::set<std::string> texts;
        for (std::int64_t i = 0; i < 4; ++i)
            texts.insert(std::string(df.columns[0].string_at(i)));
        CHECK(texts == std::set<std::string>{R"(["a","b"])", "[1,2]"});
        // Spacing, and 2.0 against 2, do not change the value.
        CHECK(rows(view_of(gz).query(R"(tags == '["a", "b"]')")) ==
              RECORDS / 2);
        CHECK(rows(view_of(gz).query(R"(tags == "[1, 2]")")) == RECORDS / 2);
        CHECK(rows(view_of(gz).query(R"(tags != "[1,2]")")) == RECORDS / 2);
    }

    TEST_CASE("time operations read the schema's roles") {
        register_log_schema();
        TestEnvironment env(10);
        const auto gz = write_log(env);
        std::size_t timed = 0, in_range = 0;
        for (int i = 0; i < RECORDS; ++i) {
            if (i % 100 == 99) continue;
            ++timed;
            in_range += 1000 + i >= 2000 && 1000 + i < 2500;
        }
        // ts_ms 2000..2499 is 2,000,000..2,499,999 microseconds.
        CHECK(rows(view_of(gz).time_range(2000000, 2500000)) == in_range);

        struct Null : ExportSink {
            void write(std::string_view) override {}
        } sink;
        const auto stats =
            view_of(gz).time_range(2000000, 2500000).sink_json(sink).get();
        CHECK(stats.events_matched == in_range);
        CHECK(stats.chunks_skipped > 0);

        const auto buckets = view_of(gz)
                                 .time_bucket(1000000)
                                 .agg({{AggOp::Count, "", "n"}})
                                 .collect()
                                 .get();
        // ts_ms 1000..3999 falls in the 1 s buckets starting at 1, 2 and 3 s.
        std::map<std::int64_t, std::int64_t> per_bucket;
        const auto& b = buckets.columns[buckets.column_index("time_bucket")];
        const auto& n = buckets.columns[buckets.column_index("n")];
        for (std::int64_t i = 0; i < n.length(); ++i)
            per_bucket[std::stoll(std::string(b.string_at(i)))] +=
                n.values<std::int64_t>()[i];
        CHECK(per_bucket ==
              std::map<std::int64_t, std::int64_t>{
                  {1000000, 990}, {2000000, 990}, {3000000, 990}});

        // Busy time is the union of [ts, ts + took) over the timed records.
        std::int64_t busy = 0, end = 0;
        for (int i = 0; i < RECORDS; ++i) {
            if (i % 100 == 99) continue;
            const std::int64_t t = (1000 + i) * 1000;
            const std::int64_t e = t + (i % 7) * 500000;
            if (e <= end) continue;
            busy += e - std::max(t, end);
            end = e;
        }
        const auto occ =
            view_of(gz).agg({{AggOp::Busy, "", "busy"}}).collect().get();
        const auto& bc = occ.columns[occ.column_index("busy")];
        CHECK(bc.values<double>()[0] ==
              doctest::Approx(static_cast<double>(busy)));

        const auto tree = run(
            view_of(gz).call_tree({"pid", "tid"}, "ts", "dur", "op").collect());
        CHECK(tree.num_rows() == timed);
        std::set<std::int64_t> lanes;
        const auto& pid = tree.columns[tree.column_index("pid")];
        for (const std::int64_t v : pid.values<std::int64_t>()) lanes.insert(v);
        CHECK(lanes.size() == 3);
        std::int64_t max_dur = 0;
        const auto& dur = tree.columns[tree.column_index("dur")];
        for (const std::int64_t v : dur.values<std::int64_t>())
            max_dur = std::max(max_dur, v);
        CHECK(max_dur == 3000000);
    }

    TEST_CASE("the schema tree shows observed paths and declared fields") {
        register_log_schema();
        TestEnvironment env(10);
        const auto gz = write_log(env);
        std::map<std::string, SchemaLeaf> tree;
        for (auto& leaf : view_of(gz).schema_tree())
            tree[leaf.path] = std::move(leaf);
        REQUIRE(tree.count("status"));
        CHECK(tree["status"].type == "mixed");
        CHECK(tree["status"].declared_type == "int");
        CHECK(tree["status"].count == RECORDS);
        CHECK(tree["ts_ms"].count == RECORDS - RECORDS / 100);
        CHECK(tree["tags.0"].type == "mixed");
        CHECK(tree["tags"].declared_type == "json");
        CHECK_FALSE(tree["tags"].count.has_value());
        CHECK(tree["meta.host"].field == "host");
        CHECK(tree["meta.host"].count == 0);
        CHECK(tree["meta.host"].type.empty());
        CHECK(tree["op"].field == "op");
        CHECK(schema_tree_json({tree["meta.host"]}) ==
              R"([{"path":"meta.host","type":null,"count":0,"field":"host",)"
              R"("declared_type":"string"}])");
    }
}
