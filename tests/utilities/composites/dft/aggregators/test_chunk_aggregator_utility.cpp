#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/chunk_aggregator_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>

using namespace dftracer::utils::utilities::composites::dft::aggregators;
using namespace dft_utils_test;

namespace {

std::string write_trace_file(const std::string& name, const std::string& body) {
    auto path = make_unique_test_path(name);
    std::ofstream out(path);
    out << body;
    out.close();
    return path.string();
}

std::size_t file_size(const std::string& path) {
    return static_cast<std::size_t>(fs::file_size(path));
}

}  // namespace

TEST_SUITE("ChunkAggregatorUtility") {
    TEST_CASE(
        "Separates event profile and system aggregations with custom metrics") {
        const std::string trace = write_trace_file(
            "chunk_aggregator_mixed_trace.pfw",
            R"({"name":"read","cat":"POSIX","pid":7,"tid":3,"ts":1000,"dur":50,"ph":"X","args":{"ret":64,"bytes":64,"hhash":"event_h","fhash":"event_f"}})"
            "\n"
            R"({"name":"cpu_usage","cat":"PROFILE","pid":7,"tid":3,"ts":1500,"dur":0,"ph":"C","args":{"count":4,"dur_sum":80,"dur_min":10,"dur_max":30,"ret_sum":400,"ret_min":50,"ret_max":150,"bytes_sum":1000,"bytes_min":100,"bytes_max":400,"hhash":"profile_h","fhash":"profile_f"}})"
            "\n"
            R"({"name":"mem_bw","cat":"sys","pid":7,"tid":3,"ts":2500,"dur":0,"ph":"C","args":{"count":2,"dur_sum":40,"dur_min":15,"dur_max":25,"ret_sum":600,"ret_min":250,"ret_max":350,"bytes_sum":1200,"bytes_min":500,"bytes_max":700,"hhash":"system_h","fhash":"system_f"}})"
            "\n");

        AggregationConfig config;
        config.custom_metric_fields = {"bytes"};

        ChunkAggregatorInput input;
        input.file_path = trace;
        input.start_byte = 0;
        input.end_byte = file_size(trace);
        input.checkpoint_size = 32 * 1024 * 1024;
        input.chunk_index = 0;
        input.config = config;

        auto output = ChunkAggregatorUtility{}.process(input).get();

        REQUIRE(output.success);
        CHECK(output.events_processed == 3);

        REQUIRE(output.aggregations.size() == 1);
        const auto& [event_key, event_metrics] = *output.aggregations.begin();
        CHECK(event_key.cat() == "POSIX");
        CHECK(event_key.name() == "read");
        CHECK(event_metrics.count == 1);
        CHECK(event_metrics.duration.total == 50);
        CHECK(event_metrics.size.total == 64);
        REQUIRE(event_metrics.custom_metrics != nullptr);
        CHECK(event_metrics.custom_metrics->count("bytes") == 1);
        CHECK((*event_metrics.custom_metrics)["bytes"].total == 64);

        REQUIRE(output.profile_aggregations.size() == 1);
        const auto& [profile_key, profile_metrics] =
            *output.profile_aggregations.begin();
        CHECK(profile_key.cat() == "PROFILE");
        CHECK(profile_key.name() == "cpu_usage");
        CHECK(profile_metrics.count == 4);
        CHECK(profile_metrics.duration.total == 80);
        CHECK(profile_metrics.duration.min == 10);
        CHECK(profile_metrics.duration.max == 30);
        CHECK(profile_metrics.duration.mean == doctest::Approx(20.0));
        CHECK(profile_metrics.size.total == 400);
        CHECK(profile_metrics.size.mean == doctest::Approx(100.0));
        REQUIRE(profile_metrics.custom_metrics != nullptr);
        CHECK((*profile_metrics.custom_metrics)["bytes"].total == 1000);
        CHECK((*profile_metrics.custom_metrics)["bytes"].min == 100);
        CHECK((*profile_metrics.custom_metrics)["bytes"].max == 400);
        CHECK((*profile_metrics.custom_metrics)["bytes"].mean ==
              doctest::Approx(250.0));

        REQUIRE(output.system_aggregations.size() == 1);
        const auto& [system_key, system_metrics] =
            *output.system_aggregations.begin();
        CHECK(system_key.cat() == "sys");
        CHECK(system_key.name() == "mem_bw");
        CHECK(system_metrics.count == 2);
        CHECK(system_metrics.duration.total == 40);
        CHECK(system_metrics.duration.mean == doctest::Approx(20.0));
        CHECK(system_metrics.size.total == 600);
        CHECK(system_metrics.size.mean == doctest::Approx(300.0));
        REQUIRE(system_metrics.custom_metrics != nullptr);
        CHECK((*system_metrics.custom_metrics)["bytes"].total == 1200);
        CHECK((*system_metrics.custom_metrics)["bytes"].min == 500);
        CHECK((*system_metrics.custom_metrics)["bytes"].max == 700);
        CHECK((*system_metrics.custom_metrics)["bytes"].mean ==
              doctest::Approx(600.0));

        fs::remove(trace);
    }

    TEST_CASE("Counter events honor query filters and byte-range completion") {
        const std::string trace = write_trace_file(
            "chunk_aggregator_query_trace.pfw",
            R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":20,"ph":"X","args":{"ret":8,"hhash":"h1"}})"
            "\n"
            R"({"name":"cpu_usage","cat":"PROFILE","pid":1,"tid":1,"ts":2000,"dur":0,"ph":"C","args":{"count":3,"dur_sum":60,"dur_min":10,"dur_max":30,"hhash":"h2"}})"
            "\n");

        std::ifstream in(trace, std::ios::binary | std::ios::ate);
        REQUIRE(in.is_open());
        const auto end = static_cast<std::size_t>(in.tellg());
        in.close();

        AggregationConfig config;
        ChunkAggregatorInput input;
        input.file_path = trace;
        input.start_byte = 0;
        input.end_byte = end - 5;
        input.checkpoint_size = 32 * 1024 * 1024;
        input.chunk_index = 1;
        input.config = config;
        input.query = dftracer::utils::utilities::common::query::parse_or_throw(
            R"(cat == "PROFILE")");

        auto output = ChunkAggregatorUtility{}.process(input).get();

        REQUIRE(output.success);
        CHECK(output.events_processed == 1);
        CHECK(output.aggregations.empty());
        REQUIRE(output.profile_aggregations.size() == 1);
        CHECK(output.system_aggregations.empty());
        CHECK(output.profile_aggregations.begin()->second.count == 3);

        fs::remove(trace);
    }
}
