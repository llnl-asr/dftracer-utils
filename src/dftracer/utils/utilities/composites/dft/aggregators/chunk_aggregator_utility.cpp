#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_logic.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/chunk_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>

#include <string_view>

namespace dftracer::utils::utilities::composites::dft::aggregators {

using dftracer::utils::utilities::composites::dft::DFTracerEvent;

coro::CoroTask<ChunkAggregationOutput> ChunkAggregatorUtility::process(
    const ChunkAggregatorInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("aggregate chunk");
    ChunkAggregationOutput output;
    output.chunk_index = input.chunk_index;
    output.events_processed = 0;
    output.bytes_processed = input.end_byte - input.start_byte;
    output.file_path = input.file_path;
    output.success = false;

    if (input.chunk_index % 100 == 0) {
        DFTRACER_UTILS_LOG_INFO("Starting chunk %d: %s [bytes %zu-%zu]",
                                input.chunk_index, input.file_path.c_str(),
                                input.start_byte, input.end_byte);
    }

    using dftracer::utils::utilities::reader::ReadConfig;
    using dftracer::utils::utilities::reader::TraceReader;
    using dftracer::utils::utilities::reader::TraceReaderConfig;

    TraceReaderConfig reader_cfg;
    reader_cfg.file_path = input.file_path;
    if (!input.index_path.empty()) {
        reader_cfg.index_dir =
            input.index_path.substr(0, input.index_path.rfind('/'));
    }
    reader_cfg.checkpoint_size = input.checkpoint_size;
    TraceReader trace_reader(reader_cfg);

    ReadConfig rc;
    rc.start_byte = input.start_byte;
    rc.end_byte = input.end_byte;
    rc.buffer_size = input.batch_size;
    if (input.query) {
        rc.query = input.query->source();
    }

    auto json_gen = trace_reader.read_json(rc);

    AggregationMap local_aggregations;
    AggregationMap local_profiles;
    AggregationMap local_system;

    std::shared_ptr<AssociationTracker> local_tracker;
    if (input.config.track_process_parents ||
        !input.config.boundary_events.empty()) {
        local_tracker = std::make_shared<AssociationTracker>();
    }

    while (auto opt = co_await json_gen.next()) {
        DFTracerEvent ev;
        if (!DFTracerEvent::parse_ondemand(*opt->parser, ev)) continue;
        if (ev.is_metadata()) continue;

        if (local_tracker) {
            local_tracker->extract_from_event(ev.name, ev.pid, ev.ts, ev.dur,
                                              ev.args, input.config);
        }
        auto key = build_aggregation_key(ev, input.config);
        if (ev.is_system()) {
            update_aggregation_entry(ev, input.config, local_system, key);
        } else if (ev.is_profile()) {
            update_aggregation_entry(ev, input.config, local_profiles, key);
        } else {
            update_aggregation_entry(ev, input.config, local_aggregations, key);
        }
        output.events_processed++;
    }

    if (local_tracker) {
        local_tracker->finalize();
        output.local_tracker = std::move(local_tracker);
    }
    output.aggregations = std::move(local_aggregations);
    output.profile_aggregations = std::move(local_profiles);
    output.system_aggregations = std::move(local_system);
    output.success = true;

    co_return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
