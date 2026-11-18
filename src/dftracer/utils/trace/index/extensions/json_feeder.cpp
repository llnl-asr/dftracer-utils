// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/extensions/json_feeder.h>
#include <dftracer/utils/trace/views/fold_event.h>

#include <cmath>
#include <variant>

namespace dftracer::utils::trace::index::extensions {

namespace {

using BV = trace::index::schemas::dft::BloomCore;
constexpr std::uint32_t NO_ID = dftracer::utils::dstd::StringIntern::NO_ID;

dstd::StringView resolve_or_empty(
    const dftracer::utils::dstd::StringIntern& intern, std::uint32_t id) {
    return id == NO_ID ? dstd::StringView{} : intern.resolve(id);
}

dstd::String capture_path(dstd::StringView dim) {
    return dstd::fmt<"args.{}">(dim);
}

}  // namespace

JsonFeeder::JsonFeeder(dftracer::utils::dstd::StringIntern& intern,
                       const BloomCore::ChunkIndexerConfig& config,
                       bool captures_nested)
    : intern_(&intern), config_(&config) {
    extra_keys_.reserve(config.extra_dimensions.size());
    for (const dstd::String& dim : config.extra_dimensions)
        extra_keys_.push_back(intern_->get_or_insert(
            captures_nested && trace::views::detail::is_nested_path(dim)
                ? capture_path(dim)
                : dim));
    auto_skip_.insert(extra_keys_.begin(), extra_keys_.end());
    for (const dstd::String& dim : config.extra_dimensions)
        auto_skip_.insert(intern_->get_or_insert(dim));
    auto_skip_.insert(intern_->get_or_insert("cmd_hash"));
    auto_skip_.insert(intern_->get_or_insert("exec_hash"));
    // An args key named like a fixed dimension would be written under the
    // fixed dimension's path and replace its data.
    if (config.fixed_dimensions)
        for (dstd::StringView dim : BV::fixed_dimension_names())
            auto_skip_.insert(intern_->get_or_insert(dim));
    dur_key_ = intern_->get_or_insert("dur");
}

void JsonFeeder::feed_auto(AutoChunk& chunk,
                           const trace::views::detail::FoldEvent& e) const {
    for (const auto& [k, v] : e.args) {
        if (auto_skip_.count(k)) continue;
        AutoField& f = chunk[k];
        if (const auto* i = std::get_if<std::int64_t>(&v)) {
            BV::observe_value(f.stats, *i);
        } else if (const auto* u = std::get_if<std::uint64_t>(&v)) {
            BV::observe_value(f.stats, *u);
        } else if (const auto* d = std::get_if<double>(&v)) {
            BV::observe_value(f.stats, *d);
        } else {
            const std::uint32_t id = std::get<std::uint32_t>(v);
            BV::observe_value(f.stats, intern_->resolve(id));
            if (!f.overflow) {
                f.values.insert(id);
                if (f.values.size() > config_->auto_max_distinct) {
                    f.overflow = true;
                    f.values.clear();
                }
            }
        }
    }
}

void JsonFeeder::feed(ChunkState& chunk, AutoChunk& auto_chunk,
                      const trace::views::detail::FoldBatch& batch) const {
    // A parse never suspends, so no two feeds share a thread's cache.
    thread_local BV::EventCache cache;
    cache.clear();

    for (const auto& e : batch.events) {
        if (e.phase == trace::RecordPhase::Metadata) {
            BV::observe_metadata(chunk, resolve_or_empty(*intern_, e.name_id));
            for (const char* p : {"name", "ph", "pid", "tid", "ts"})
                BV::observe_metadata_path(chunk, p);
            if (e.has_dur) BV::observe_metadata_path(chunk, "dur");
            if (e.cat_id != NO_ID) BV::observe_metadata_path(chunk, "cat");
            // A hash arg is parsed into its own field, off e.args.
            if (e.fhash_id != NO_ID) BV::observe_metadata_path(chunk, "fhash");
            if (e.hhash_id != NO_ID) BV::observe_metadata_path(chunk, "hhash");
            for (const auto& [k, v] : e.args)
                BV::observe_metadata_path(
                    chunk, "args." + dstd::String(intern_->resolve(k)));
            continue;
        }

        const dstd::StringView name = resolve_or_empty(*intern_, e.name_id);
        const dstd::StringView cat = resolve_or_empty(*intern_, e.cat_id);
        const dstd::StringView hhash = resolve_or_empty(*intern_, e.hhash_id);
        const dstd::StringView fhash = resolve_or_empty(*intern_, e.fhash_id);

        dstd::StringView shash;
        for (const auto& [k, v] : e.args) {
            const auto* id = std::get_if<std::uint32_t>(&v);
            if (!id) continue;
            const auto key = intern_->resolve(k);
            if (key == "cmd_hash") {
                shash = intern_->resolve(*id);
                break;
            }
            if (key == "exec_hash" && shash.empty())
                shash = intern_->resolve(*id);
        }

        if (config_->fixed_dimensions) {
            BV::observe_data(chunk, cache, e.name_id, e.cat_id, name, cat,
                             e.pid, e.tid, e.ts, e.dur, e.has_dur, hhash, fhash,
                             shash);
            // A record without a top-level dur (an aggregated one) matches
            // `dur` on args.dur, so the dur zone covers it too.
            if (!e.has_dur)
                for (const auto& [k, v] : e.args) {
                    if (k != dur_key_) continue;
                    auto& ds = chunk.fixed_dim_stats[BV::FdDur];
                    // Durations are not negative, so the uint range holds.
                    if (const auto* i = std::get_if<std::int64_t>(&v);
                        i && *i >= 0)
                        ds.observe_range_only(static_cast<std::uint64_t>(*i));
                    if (const auto* d = std::get_if<double>(&v); d && *d >= 0) {
                        ds.observe_range_only(
                            static_cast<std::uint64_t>(std::floor(*d)));
                        ds.observe_range_only(
                            static_cast<std::uint64_t>(std::ceil(*d)));
                    }
                    break;
                }
        } else
            ++chunk.statistics.total_events;

        for (std::size_t x = 0; x < extra_keys_.size(); ++x) {
            for (const auto& [k, v] : e.args) {
                if (k != extra_keys_[x]) continue;
                if (const auto* i = std::get_if<std::int64_t>(&v))
                    BV::observe_extra(chunk, x, *i);
                else if (const auto* u = std::get_if<std::uint64_t>(&v))
                    BV::observe_extra(chunk, x, *u);
                else if (const auto* d = std::get_if<double>(&v))
                    BV::observe_extra(chunk, x, *d);
                else
                    BV::observe_extra(
                        chunk, x, intern_->resolve(std::get<std::uint32_t>(v)));
                break;
            }
        }
        feed_auto(auto_chunk, e);
    }
}

}  // namespace dftracer::utils::trace::index::extensions
