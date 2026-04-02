#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator_utility.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::composites::dft::aggregators;

namespace {

AggregationKey make_key(std::string_view cat, std::string_view name,
                        std::uint64_t time_bucket = 0) {
    auto& intern = aggregation_intern();
    AggregationKey key;
    key.cat_id = intern.get_or_insert(cat);
    key.name_id = intern.get_or_insert(name);
    key.pid = 1;
    key.tid = 2;
    key.time_bucket = time_bucket;
    return key;
}

AggregationMetrics make_metrics(std::uint64_t count, std::uint64_t dur_total) {
    AggregationMetrics metrics;
    for (std::uint64_t i = 0; i < count; ++i) {
        metrics.update_duration(dur_total / count);
    }
    return metrics;
}

}  // namespace

TEST_SUITE("EventAggregatorUtility") {
    TEST_CASE("Merges event profile and system maps independently") {
        ChunkAggregationOutput first;
        first.success = true;
        first.file_path = "/tmp/a.pfw";
        first.events_processed = 2;
        first.bytes_processed = 128;
        first.aggregations.emplace(make_key("POSIX", "read"),
                                   make_metrics(2, 40));
        first.profile_aggregations.emplace(make_key("PROFILE", "cpu"),
                                           make_metrics(3, 90));

        ChunkAggregationOutput second;
        second.success = true;
        second.file_path = "/tmp/b.pfw";
        second.events_processed = 1;
        second.bytes_processed = 64;
        second.system_aggregations.emplace(make_key("sys", "mem"),
                                           make_metrics(4, 120));
        second.profile_aggregations.emplace(make_key("PROFILE", "cpu"),
                                            make_metrics(1, 30));

        EventAggregatorUtility utility;
        utility.merge_chunk(std::move(first));
        utility.merge_chunk(std::move(second));
        auto output = utility.finalize();

        CHECK(output.total_events_processed == 3);
        CHECK(output.total_files_processed == 2);
        CHECK(output.total_bytes_processed == 192);

        REQUIRE(output.aggregations.size() == 1);
        CHECK(output.aggregations.begin()->second.count == 2);

        REQUIRE(output.profile_aggregations.size() == 1);
        CHECK(output.profile_aggregations.begin()->second.count == 4);
        CHECK(output.profile_aggregations.begin()->second.duration.total ==
              120);

        REQUIRE(output.system_aggregations.size() == 1);
        CHECK(output.system_aggregations.begin()->second.count == 4);
    }
}
