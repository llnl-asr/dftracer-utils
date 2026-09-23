#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/trace/aggregators/aggregation_serialization.h>
#include <dftracer/utils/trace/aggregators/event_aggregator.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <string>

using namespace dftracer::utils::trace::aggregators;

namespace {

dftracer::utils::StringIntern& test_intern() {
    static auto table = make_intern_table();
    return table->intern;
}

AggregationKey make_key(std::string_view cat, std::string_view name,
                        std::uint64_t time_bucket = 0) {
    auto& intern = test_intern();
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

TEST_SUITE("EventAggregator") {
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

        EventAggregator utility;
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
        CHECK(output.profile_aggregations.begin()->second.duration.total() ==
              120);

        REQUIRE(output.system_aggregations.size() == 1);
        CHECK(output.system_aggregations.begin()->second.count == 4);
    }

    TEST_CASE("Two indexes open at once keep their own string ids") {
        auto root = dftu_utils_test::make_unique_test_path(
            "dftu_intern_two_index_test");
        fs::remove_all(root);
        const auto path_a = (root / "a").string();
        const auto path_b = (root / "b").string();

        // Both dictionaries start at id 0, so index B's ids collide with A's.
        // They only resolve correctly if each index owns its own table.
        std::string key_a, key_b;
        {
            auto db = EventAggregator::open_with_merge_operator(path_a);
            EventAggregator agg(db, 0);
            auto& intern = agg.intern();
            AggregationKey k;
            k.cat_id = intern.get_or_insert("POSIX");
            k.name_id = intern.get_or_insert("read");
            key_a = serialize_agg_key(0, AggMapType::EVENT, k, intern);
            auto batch = db->begin_batch();
            flush_intern_dictionary(*db, batch, *agg.intern_table());
            db->commit_batch(batch);
        }
        {
            auto db = EventAggregator::open_with_merge_operator(path_b);
            EventAggregator agg(db, 0);
            auto& intern = agg.intern();
            AggregationKey k;
            k.cat_id = intern.get_or_insert("MPI");
            k.name_id = intern.get_or_insert("send");
            key_b = serialize_agg_key(0, AggMapType::EVENT, k, intern);
            auto batch = db->begin_batch();
            flush_intern_dictionary(*db, batch, *agg.intern_table());
            db->commit_batch(batch);
        }

        auto db_a = EventAggregator::open_with_merge_operator(path_a);
        EventAggregator agg_a(db_a, 0);
        auto db_b = EventAggregator::open_with_merge_operator(path_b);
        EventAggregator agg_b(db_b, 0);

        AggKeyView view_a, view_b;
        REQUIRE(parse_agg_key_view(key_a, agg_a.intern(), view_a));
        REQUIRE(parse_agg_key_view(key_b, agg_b.intern(), view_b));
        CHECK(view_a.cat == "POSIX");
        CHECK(view_a.name == "read");
        CHECK(view_b.cat == "MPI");
        CHECK(view_b.name == "send");

        fs::remove_all(root);
    }
}
