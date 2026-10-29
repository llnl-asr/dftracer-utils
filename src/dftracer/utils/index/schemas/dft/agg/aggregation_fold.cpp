#include <dftracer/utils/index/schemas/dft/agg/agg_partial_update.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_serialization.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace dftracer::utils::index::schemas::dft::agg {

namespace {
// Build ids past this are looked up each time instead of cached.
constexpr std::uint32_t MAX_CACHED_BUILD_ID = 1u << 20;
}  // namespace

std::size_t AggregationFold::column_budget_for(
    const trace::views::detail::ScanUnit& unit) const {
    if (!(config_.column_budget_ratio > 0.0)) return NO_COLUMN_BUDGET;
    const std::size_t bytes =
        unit.end_byte > unit.start_byte ? unit.end_byte - unit.start_byte : 0;
    return static_cast<std::size_t>(static_cast<double>(bytes) *
                                    config_.column_budget_ratio);
}

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
    if (!partial_holds(acc)) return;
    TimeBin* bin = nullptr;
    if (config_.time_interval_us != 0) {
        // The bin of compute_time_bucket, with one division.
        std::uint64_t ts = acc.ts();
        if (config_.use_relative_time) ts -= config_.reference_timestamp;
        bin =
            &bin_of(slot, acc.pid(), acc.tid(), ts / config_.time_interval_us);
    }
    auto id_of = [this](std::string_view k) { return arg_id(k); };
    fold_into_partial(slot.partial, acc, config_, id_of, bin);
    slot.used = true;
}

TimeBin& AggregationFold::bin_of(Slot& slot, std::uint64_t pid,
                                 std::uint64_t tid, std::uint64_t bin) {
    // Events of a thread come mostly in time order, so each thread keeps its
    // newest bin. An event of an older bin looks back a few bins and else
    // adds a second bin of that key, which normalize_bins adds up.
    constexpr std::size_t LOOK_BACK = 8;
    auto& bins = slot.partial.bins;
    slot.partial.bin_interval = config_.time_interval_us;
    std::uint32_t t = slot.last_thread;
    if (t >= slot.threads.size() || slot.threads[t].pid != pid ||
        slot.threads[t].tid != tid) {
        t = 0;
        while (t < slot.threads.size() &&
               (slot.threads[t].pid != pid || slot.threads[t].tid != tid))
            ++t;
        if (t == slot.threads.size()) {
            slot.threads.push_back(
                {pid, tid, static_cast<std::uint32_t>(bins.size())});
            bins.push_back(TimeBin{pid, tid, bin});
            slot.last_thread = t;
            return bins.back();
        }
        slot.last_thread = t;
    }
    ThreadBin& thread = slot.threads[t];
    TimeBin& newest = bins[thread.at];
    if (newest.bin == bin) return newest;
    if (bin > newest.bin) {
        thread.at = static_cast<std::uint32_t>(bins.size());
        bins.push_back(TimeBin{pid, tid, bin});
        return bins.back();
    }
    const std::size_t stop =
        bins.size() > LOOK_BACK ? bins.size() - LOOK_BACK : 0;
    for (std::size_t i = bins.size(); i-- > stop;)
        if (bins[i].bin == bin && bins[i].pid == pid && bins[i].tid == tid)
            return bins[i];
    bins.push_back(TimeBin{pid, tid, bin});
    return bins.back();
}

void AggregationFold::add_to_columns(
    const trace::views::detail::FoldEvent& ev) {
    // The column keeps the category text of the record itself; only the
    // partials group by its lower case.
    columns_.add_row(ev.ts, ev.dur, ev.has_dur, ev.pid, ev.tid,
                     static_cast<std::uint8_t>(ev.phase),
                     root_id(cat_text_ids_, ev.cat_id, false),
                     root_id(name_ids_, ev.name_id, false),
                     root_id(host_ids_, ev.hhash_id, false));
    for (const auto& [key_id, value] : ev.args) {
        const std::uint32_t path = root_id(path_ids_, key_id, false);
        if (const auto* d = std::get_if<double>(&value)) {
            columns_.add_double(path, *d);
        } else if (const auto* i = std::get_if<std::int64_t>(&value)) {
            columns_.add_int64(path, *i);
        } else if (const auto* u = std::get_if<std::uint64_t>(&value)) {
            columns_.add_uint64(path, *u);
        }
    }
}

void AggregationFold::close_columns() {
    if (columns_.empty()) return;
    const std::size_t offset = arena_.size();
    columns_.encode_into(arena_, column_budget_);
    closed_columns_.push_back({chunk_, offset, arena_.size() - offset});
    referenced_.insert(referenced_.end(), columns_.ids().begin(),
                       columns_.ids().end());
    columns_.clear();
}

void AggregationFold::close_chunk() {
    close_columns();
    for (Slot& slot : slots_) {
        if (!slot.used) continue;
        const std::size_t offset = arena_.size();
        slot.partial.normalize_bins();
        slot.partial.serialize_into(arena_);
        closed_.push_back({chunk_, offset, arena_.size() - offset, slot.key});
        referenced_.push_back(slot.key.cat);
        referenced_.push_back(slot.key.name);
        referenced_.push_back(slot.key.host);
        referenced_.push_back(slot.key.extra);
        for (const ArgStat& arg : slot.partial.args)
            referenced_.push_back(arg.id);
        slot.partial.clear();
        slot.threads.clear();
        slot.last_thread = 0;
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
    for (ClosedColumns c : o.closed_columns_) {
        c.offset += base;
        closed_columns_.push_back(c);
    }
    referenced_.insert(referenced_.end(), o.referenced_.begin(),
                       o.referenced_.end());
    o.arena_.clear();
    o.closed_.clear();
    o.closed_columns_.clear();
    o.referenced_.clear();

    os.system_buffer.clear();
}

void AggregationFold::write(index::store::IndexWrite& w) {
    namespace layout = index::store::layout;
    if (file_id_ < 0) {
        throw std::logic_error(
            "an aggregation fold writes the data of one file, and has none");
    }
    close_chunk();
    // The clear covers every kind of the file's aggregation data, its
    // system rows included, so it comes before any of them is put.
    index::store::records::clear_file(w, layout::Ext::Agg, file_id_);
    std::string sys_val_buf;
    for (auto& [k, m] : state_.system_buffer) {
        serialize_system_value_into(sys_val_buf, m);
        index::store::records::put_agg_system_row(w, file_id_, k, sys_val_buf);
    }
    for (const Closed& c : closed_)
        index::store::records::put_agg_partial(
            w, file_id_, c.chunk, c.key.cat, c.key.name, c.key.host,
            c.key.extra, static_cast<std::uint8_t>(c.key.kind),
            std::string_view(
                reinterpret_cast<const char*>(arena_.data()) + c.offset,
                c.size));
    closed_.clear();
    for (const ClosedColumns& c : closed_columns_)
        index::store::records::put_agg_chunk_columns(
            w, file_id_, c.chunk,
            std::string_view(
                reinterpret_cast<const char*>(arena_.data()) + c.offset,
                c.size));
    closed_columns_.clear();
    arena_.clear();
    std::sort(referenced_.begin(), referenced_.end());
    referenced_.erase(std::unique(referenced_.begin(), referenced_.end()),
                      referenced_.end());
    for (const std::uint32_t id : referenced_)
        index::store::records::put_agg_dictionary_entry(
            w, file_id_, id, agg_intern_->intern.resolve(id));
    tier::put_file(w, file_id_, config_.params_hash());
    referenced_.clear();
}

}  // namespace dftracer::utils::index::schemas::dft::agg
