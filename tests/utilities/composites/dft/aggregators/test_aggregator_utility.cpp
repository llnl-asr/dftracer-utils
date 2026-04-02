#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregator_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <vector>

using namespace dftracer::utils::utilities::composites::dft::aggregators;
using namespace dftracer::utils::coro;
using namespace dft_utils_test;

namespace {

static CoroTask<std::vector<AggregationBatch>> collect_batches(
    AsyncGenerator<AggregationBatch> gen) {
    std::vector<AggregationBatch> batches;
    while (auto batch = co_await gen.next()) {
        batches.push_back(std::move(*batch));
    }
    co_return batches;
}

}  // namespace

TEST_SUITE("AggregatorUtility") {
    TEST_CASE("Collects event profile and system counter batches end-to-end") {
        TestEnvironment env(0);
        REQUIRE(env.is_valid());

        auto trace = fs::path(env.get_dir()) / "mixed_trace.pfw";
        {
            std::ofstream out(trace);
            out << R"({"name":"read","cat":"POSIX","pid":7,"tid":3,"ts":1000,"dur":50,"ph":"X","args":{"ret":64,"bytes":64,"hhash":"event_h","fhash":"event_f"}})"
                << "\n";
            out << R"({"name":"cpu_usage","cat":"PROFILE","pid":7,"tid":3,"ts":1500,"dur":0,"ph":"C","args":{"count":4,"dur_sum":80,"dur_min":10,"dur_max":30,"ret_sum":400,"ret_min":50,"ret_max":150,"bytes_sum":1000,"bytes_min":100,"bytes_max":400,"hhash":"profile_h","fhash":"profile_f"}})"
                << "\n";
            out << R"({"name":"mem_bw","cat":"sys","pid":7,"tid":3,"ts":2500,"dur":0,"ph":"C","args":{"count":2,"dur_sum":40,"dur_min":15,"dur_max":25,"ret_sum":600,"ret_min":250,"ret_max":350,"bytes_sum":1200,"bytes_min":500,"bytes_max":700,"hhash":"system_h","fhash":"system_f"}})"
                << "\n";
        }

        AggregatorInput input;
        input.directory = env.get_dir();
        input.index_dir = env.get_dir();
        input.force_rebuild = true;
        input.event_batch_size = 1;
        input.config.custom_metric_fields = {"bytes"};
        input.config.track_process_parents = false;

        auto batches =
            collect_batches(AggregatorUtility{}.process(input)).get();

        REQUIRE(batches.size() == 3);

        const auto* event_batch = static_cast<const AggregationBatch*>(nullptr);
        const auto* profile_batch =
            static_cast<const AggregationBatch*>(nullptr);
        const auto* system_batch =
            static_cast<const AggregationBatch*>(nullptr);
        for (const auto& batch : batches) {
            if (batch.batch_type == AggregationBatchType::EVENT) {
                event_batch = &batch;
            } else if (batch.batch_type == AggregationBatchType::PROFILE) {
                profile_batch = &batch;
            } else if (batch.batch_type == AggregationBatchType::SYSTEM) {
                system_batch = &batch;
            }
        }

        REQUIRE(event_batch != nullptr);
        REQUIRE(profile_batch != nullptr);
        REQUIRE(system_batch != nullptr);

        CHECK(event_batch->entries.size() == 1);
        CHECK(profile_batch->entries.size() == 1);
        CHECK(system_batch->entries.size() == 1);

        const auto& [event_key, event_metrics] = event_batch->entries.front();
        CHECK(event_key.cat() == "POSIX");
        CHECK(event_key.name() == "read");
        CHECK(event_metrics.count == 1);
        CHECK(event_metrics.duration.total == 50);
        CHECK(event_metrics.size.total == 64);

        const auto& [profile_key, profile_metrics] =
            profile_batch->entries.front();
        CHECK(profile_key.cat() == "PROFILE");
        CHECK(profile_key.name() == "cpu_usage");
        CHECK(profile_metrics.count == 4);
        CHECK(profile_metrics.duration.total == 80);
        CHECK(profile_metrics.size.total == 400);
        REQUIRE(profile_metrics.custom_metrics != nullptr);
        CHECK((*profile_metrics.custom_metrics)["bytes"].total == 1000);

        const auto& [system_key, system_metrics] =
            system_batch->entries.front();
        CHECK(system_key.cat() == "sys");
        CHECK(system_key.name() == "mem_bw");
        CHECK(system_metrics.count == 2);
        CHECK(system_metrics.duration.total == 40);
        CHECK(system_metrics.size.total == 600);
        REQUIRE(system_metrics.custom_metrics != nullptr);
        CHECK((*system_metrics.custom_metrics)["bytes"].total == 1200);

        CHECK(event_batch->total_events_processed == 3);
        CHECK(profile_batch->total_events_processed == 3);
        CHECK(system_batch->total_events_processed == 3);
        CHECK(event_batch->total_files_processed == 1);
        CHECK(profile_batch->total_files_processed == 1);
        CHECK(system_batch->total_files_processed == 1);
    }
}
