#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_logic.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_map.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/trace/comparator/comparison_aggregation.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <simdjson.h>

#include <string_view>
#include <utility>

namespace dftracer::utils::trace::comparator {

namespace {

using dftracer::utils::index::schemas::dft::agg::AggregationMap;
using dftracer::utils::trace::DFTracerEvent;

// The three aggregation tiers a session accumulates, one per map_batches slot.
struct Tiers {
    AggregationMap event;
    AggregationMap profile;
    AggregationMap system;
};

// Fold a source map into a destination map key-by-key; mirrors
// EventAggregator::merge_chunk so the DDSketches combine identically.
void merge_into(AggregationMap& dst, AggregationMap& src) {
    for (auto& [key, metrics] : src) {
        auto it = dst.find(key);
        if (it == dst.end())
            dst.emplace(key, std::move(metrics));
        else
            it->second.merge_from(metrics);
    }
}

}  // namespace

coro::CoroTask<dftracer::utils::index::schemas::dft::agg::EventAggregatorOutput>
run_comparison_aggregation(
    CoroScope& /*ctx*/, const std::vector<std::string>& input_files,
    const dftracer::utils::index::schemas::dft::agg::AggregationConfig&
        agg_config,
    const std::optional<query::Query>& query, const std::string& index_dir,
    std::size_t checkpoint_size, bool /*force_rebuild*/,
    std::size_t executor_threads) {
    std::vector<views::ViewFile> files;
    files.reserve(input_files.size());
    for (const auto& f : input_files) {
        views::ViewFile vf;
        vf.file_path = f;
        vf.index_path = trace::internal::determine_index_path(f, index_dir);
        vf.checkpoint_size = checkpoint_size;
        files.push_back(std::move(vf));
    }
    views::View view = views::View::from_files(std::move(files));
    if (query) view = view.filter(*query);

    // One shared intern (thread-safe get_or_insert) so the per-slot maps use
    // the same string ids and merge key-for-key.
    auto intern_table = std::make_shared<
        dftracer::utils::index::schemas::dft::agg::AggInternTable>();
    dftracer::utils::StringIntern& intern = intern_table->intern;
    const dftracer::utils::index::schemas::dft::agg::AggregationConfig& config =
        agg_config;

    // Mirrors ChunkAggregator's per-event core so results merge identically.
    auto fold = [&config, &intern](Tiers& acc,
                                   const std::vector<std::string_view>& lines) {
        simdjson::dom::parser parser;
        for (std::string_view line : lines) {
            auto result = parser.parse(line.data(), line.size());
            if (result.error()) continue;
            auto root = result.value_unsafe();
            if (!root.is_object()) continue;
            json::JsonValue json(root);
            DFTracerEvent ev;
            if (!DFTracerEvent::parse(json, ev)) continue;
            if (ev.is_metadata()) continue;
            auto key = dftracer::utils::index::schemas::dft::agg::
                build_aggregation_key(ev, config, intern);
            if (ev.is_system())
                dftracer::utils::index::schemas::dft::agg::
                    update_aggregation_entry(ev, config, acc.system, key,
                                             intern);
            else if (ev.is_profile())
                dftracer::utils::index::schemas::dft::agg::
                    update_aggregation_entry(ev, config, acc.profile, key,
                                             intern);
            else
                dftracer::utils::index::schemas::dft::agg::
                    update_aggregation_entry(ev, config, acc.event, key,
                                             intern);
        }
    };
    auto combine = [](Tiers&& a, Tiers&& b) -> Tiers {
        merge_into(a.event, b.event);
        merge_into(a.profile, b.profile);
        merge_into(a.system, b.system);
        return std::move(a);
    };

    auto res = co_await view.map_batches<Tiers>(
        std::move(fold), std::move(combine), executor_threads);

    dftracer::utils::index::schemas::dft::agg::EventAggregatorOutput out;
    out.intern = intern_table;
    out.aggregations = std::move(res.value.event);
    out.profile_aggregations = std::move(res.value.profile);
    out.system_aggregations = std::move(res.value.system);
    out.total_events_processed = res.stats.events_matched;
    out.total_files_processed = input_files.size();
    out.total_bytes_processed = 0;
    out.success = true;
    co_return out;
}

}  // namespace dftracer::utils::trace::comparator
