#include <dftracer/utils/index/schemas/dft/agg/agg_partial_update.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_serialization.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <algorithm>
#include <string>
#include <utility>

namespace dftracer::utils::index::schemas::dft::agg {

namespace {
// Build ids past this are looked up each time instead of cached.
constexpr std::uint32_t MAX_CACHED_BUILD_ID = 1u << 20;
}  // namespace

std::uint32_t AggregationFold::root_id(dstd::Vector<std::uint32_t>& cache,
                                       std::uint32_t build_id, bool lower) {
    const bool cacheable = build_id < MAX_CACHED_BUILD_ID;
    if (cacheable) {
        if (build_id >= cache.size()) cache.resize(build_id + 1, NO_ID);
        if (cache[build_id] != NO_ID) return cache[build_id];
    }
    std::string_view text = build_id == NO_ID
                                ? std::string_view{}
                                : build_intern_->resolve(build_id);
    std::string storage;
    if (lower) text = trace::internal::to_lower_ascii(text, storage);
    const std::uint32_t id = agg_intern_->intern.get_or_insert(text);
    if (cacheable) cache[build_id] = id;
    return id;
}

std::uint32_t AggregationFold::arg_id(std::string_view key) {
    // Arg names are views into the build intern or the config, which keep
    // their bytes, so the address names the string.
    auto it = arg_ids_.find(key.data());
    if (it != arg_ids_.end() && it->second.length == key.size())
        return it->second.id;
    const std::uint32_t id = agg_intern_->intern.get_or_insert(key);
    arg_ids_[key.data()] = {static_cast<std::uint32_t>(key.size()), id};
    return id;
}

void AggregationFold::add_to_partial(const trace::views::detail::FoldEvent& ev,
                                     const PodAccessor& acc) {
    std::uint32_t at;
    const PartialKind kind = partial_kind(acc);
    const bool has_extra = !config_.extra_group_keys.empty();
    if (!has_extra && last_slot_ != NO_ID && ev.cat_id == last_cat_ &&
        ev.name_id == last_name_ && ev.hhash_id == last_host_ &&
        kind == last_kind_) {
        at = last_slot_;
    } else {
        if (empty_id_ == NO_ID)
            empty_id_ = agg_intern_->intern.get_or_insert("");
        std::uint32_t extra = empty_id_;
        if (has_extra) {
            extra_values(acc, config_, extra_buf_);
            extra = agg_intern_->intern.get_or_insert(extra_buf_);
        }
        const SlotKey key{root_id(cat_ids_, ev.cat_id, true),
                          root_id(name_ids_, ev.name_id, false),
                          root_id(host_ids_, ev.hhash_id, false), extra, kind};
        auto it = slot_of_.find(key);
        if (it == slot_of_.end()) {
            it =
                slot_of_.emplace(key, static_cast<std::uint32_t>(slots_.size()))
                    .first;
            slots_.emplace_back(config_.sketch_accuracy, key);
        }
        at = it->second;
        last_slot_ = at;
        last_cat_ = ev.cat_id;
        last_name_ = ev.name_id;
        last_host_ = ev.hhash_id;
        last_kind_ = kind;
    }
    Slot& slot = slots_[at];
    auto id_of = [this](std::string_view k) { return arg_id(k); };
    if (update_partial(slot.partial, acc, config_, id_of)) slot.used = true;
}

void AggregationFold::close_chunk() {
    for (Slot& slot : slots_) {
        if (!slot.used) continue;
        const std::size_t offset = arena_.size();
        slot.partial.serialize_into(arena_);
        closed_.push_back({chunk_, offset, arena_.size() - offset, slot.key});
        slot.partial.clear();
        slot.used = false;
    }
}

void AggregationFold::merge(trace::views::detail::Fold& other) {
    auto& o = static_cast<AggregationFold&>(other);
    auto& os = o.state_;

    for (auto& [k, m] : os.system_buffer) {
        auto it = state_.system_buffer.find(k);
        if (it == state_.system_buffer.end())
            state_.system_buffer.emplace(k, std::move(m));
        else
            it->second.merge_from(m);
    }
    state_.min_time_bucket =
        std::min(state_.min_time_bucket, os.min_time_bucket);
    state_.max_time_bucket =
        std::max(state_.max_time_bucket, os.max_time_bucket);
    state_.events_processed += os.events_processed;

    if (o.tracker_) {
        if (tracker_)
            tracker_->merge(*o.tracker_);
        else
            tracker_ = std::move(o.tracker_);
    }

    close_chunk();
    o.close_chunk();
    const std::size_t base = arena_.size();
    arena_.insert(arena_.end(), o.arena_.begin(), o.arena_.end());
    for (Closed c : o.closed_) {
        c.offset += base;
        closed_.push_back(c);
    }
    o.arena_.clear();
    o.closed_.clear();

    os.system_buffer.clear();
}

void AggregationFold::write(index::store::IndexWrite& w) {
    namespace agg = index::schemas::dft::agg;
    namespace layout = index::store::layout;
    std::string sys_val_buf;
    for (auto& [k, m] : state_.system_buffer) {
        agg::serialize_system_value_into(sys_val_buf, m);
        tier::merge_system_row(w, k, sys_val_buf);
    }
    if (file_id_ >= 0) {
        close_chunk();
        index::store::records::clear_file(w, layout::Ext::Agg, file_id_);
        for (const Closed& c : closed_)
            index::store::records::put_agg_partial(
                w, file_id_, c.chunk, c.key.cat, c.key.name, c.key.host,
                c.key.extra, static_cast<std::uint8_t>(c.key.kind),
                std::string_view(
                    reinterpret_cast<const char*>(arena_.data()) + c.offset,
                    c.size));
        closed_.clear();
        arena_.clear();
    }
    tier::flush_dictionary(w, *agg_intern_);
    if (file_id_ >= 0) tier::put_file(w, file_id_, config_.params_hash());
}

}  // namespace dftracer::utils::index::schemas::dft::agg
