#include <dftracer/utils/index/schemas/dft/agg/agg_partial_update.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace dftracer::utils::index::schemas::dft::agg {

namespace {
constexpr std::uint32_t MAX_CACHED_BUILD_ID = 1u << 20;

std::string_view bytes_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
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
        std::uint64_t ts = acc.ts();
        if (config_.use_relative_time) ts -= config_.reference_timestamp;
        bin =
            &bin_of(slot, acc.pid(), acc.tid(), ts / config_.time_interval_us);
    }
    fold_into_partial(slot.partial, acc, config_, bin);
    slot.used = true;
}

TimeBin& AggregationFold::bin_of(Slot& slot, std::uint64_t pid,
                                 std::uint64_t tid, std::uint64_t bin) {
    // Events of a thread come mostly in time order, so each thread keeps its
    // newest bin. An older event looks back a few bins, else adds a second bin
    // of that key, which normalize_bins adds up.
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
    // The column keeps the record's category case. Only the partials group by
    // lower case.
    columns_.add_row(ev.ts, ev.dur, ev.has_dur, ev.pid, ev.tid,
                     static_cast<std::uint8_t>(ev.phase),
                     root_id(cat_text_ids_, ev.cat_id, false),
                     root_id(name_ids_, ev.name_id, false),
                     root_id(host_ids_, ev.hhash_id, false),
                     root_id(fhash_ids_, ev.fhash_id, false));
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
    scratch_.clear();
    columns_.encode_into(scratch_);
    ClosedColumns closed;
    closed.chunk = chunk_;
    closed.value.assign(scratch_.begin(), scratch_.end());
    closed_bytes_ += closed.value.size();
    closed_columns_.push_back(std::move(closed));
    referenced_.insert(referenced_.end(), columns_.ids().begin(),
                       columns_.ids().end());
    columns_.clear();
}

void AggregationFold::close_chunk() {
    close_columns();
    for (Slot& slot : slots_) {
        if (!slot.used) continue;
        slot.partial.normalize_bins();
        scratch_.clear();
        slot.partial.serialize_into(scratch_);
        Closed closed;
        closed.chunk = chunk_;
        closed.key = slot.key;
        closed.value.assign(scratch_.begin(), scratch_.end());
        closed_bytes_ += closed.value.size();
        closed_.push_back(std::move(closed));
        referenced_.push_back(slot.key.cat);
        referenced_.push_back(slot.key.name);
        referenced_.push_back(slot.key.host);
        referenced_.push_back(slot.key.extra);
        slot.partial.clear();
        slot.threads.clear();
        slot.last_thread = 0;
        slot.used = false;
    }
    if (through_ == nullptr) return;
    namespace fa = constants::file_admission;
    const std::size_t doublings = std::min(
        through_runs_ / fa::SPILL_RUNS_PER_DOUBLING, fa::MAX_SPILL_DOUBLINGS);
    if (closed_bytes_ >= g_agg_run_bytes.load(std::memory_order_relaxed)
                             << doublings)
        drain_due_ = true;
}

coro::CoroTask<void> AggregationFold::drain() {
    drain_due_ = false;
    auto held = co_await index::claim(*through_);
    if (!held) co_return;
    put_closed(held->build());
    auto staged = co_await held->stage();
    if (!staged) co_return;
    ++through_runs_;
#if DFTRACER_UTILS_TEST_PROBES
    g_agg_runs_staged.fetch_add(1, std::memory_order_relaxed);
#endif
}

void AggregationFold::put_closed(index::FileBuild& build) {
    for (const Closed& closed : closed_)
        index::put_agg_partial_value(
            build, closed.chunk, closed.key.cat, closed.key.name,
            closed.key.host, closed.key.extra,
            static_cast<std::uint8_t>(closed.key.kind), bytes_of(closed.value));
    for (const ClosedColumns& closed : closed_columns_)
        index::put_agg_chunk_columns_value(build, closed.chunk,
                                           bytes_of(closed.value));
    release_closed();
}

void AggregationFold::release_closed() {
    closed_.clear();
    closed_columns_.clear();
    closed_bytes_ = 0;
    std::sort(referenced_.begin(), referenced_.end());
    referenced_.erase(std::unique(referenced_.begin(), referenced_.end()),
                      referenced_.end());
}

void AggregationFold::merge(trace::views::detail::Fold& other) {
    auto& o = static_cast<AggregationFold&>(other);
    auto& os = o.state_;

    state_.min_time_bucket =
        std::min(state_.min_time_bucket, os.min_time_bucket);
    state_.max_time_bucket =
        std::max(state_.max_time_bucket, os.max_time_bucket);
    state_.events_processed += os.events_processed;

    close_chunk();
    o.close_chunk();
    for (Closed& closed : o.closed_) closed_.push_back(std::move(closed));
    for (ClosedColumns& closed : o.closed_columns_)
        closed_columns_.push_back(std::move(closed));
    referenced_.insert(referenced_.end(), o.referenced_.begin(),
                       o.referenced_.end());
    closed_bytes_ += o.closed_bytes_;
    o.closed_bytes_ = 0;
    o.closed_.clear();
    o.closed_columns_.clear();
    o.referenced_.clear();
}

void AggregationFold::write(index::FileBuild& build) {
    if (!has_file_) {
        throw std::logic_error(
            "an aggregation fold writes the data of one file, and has none");
    }
    close_chunk();
    finish_rows(build);
}

void AggregationFold::write_through(index::FileBuild& build) {
    through_ = &build;
    has_file_ = true;
}

void AggregationFold::finish_rows(index::FileBuild& build) {
    put_closed(build);
    for (const std::uint32_t id : referenced_)
        index::put_agg_dictionary_entry(build, id,
                                        agg_intern_->intern.resolve(id));
    build.finish(index::store::IndexExtension::Agg, config_.params_hash());
    referenced_.clear();
}

}  // namespace dftracer::utils::index::schemas::dft::agg
