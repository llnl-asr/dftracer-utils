// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/versions.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/trace/index/cache/phase_seed_store.h>
#include <dftracer/utils/trace/index/cache/rollup_store.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <algorithm>
#include <cstddef>
#include <limits>

namespace dftracer::utils::trace::index::cache {

namespace {

constexpr dstd::StringView VALUE_KEY = "\x01";
constexpr std::uint8_t VALUE_FORMAT = 1;
constexpr std::size_t VALUE_BYTES = 1 + 3 * 8;
// The size of an entry counts the signature that names it.
constexpr std::uint64_t SIG_BYTES = 8;

}  // namespace

dstd::Optional<InputStats> index_input_stats(
    const trace::views::detail::ViewPlan& plan) {
    if (trace::views::detail::has_pin(plan)) return dstd::nullopt;
    InputStats total;
    bool any_start = false;
    dstd::Map<dstd::String, dstd::Optional<trace::index::Snapshot>, std::less<>>
        snaps;
    for (const auto& f : plan.files) {
        if (f.index_path.empty()) return dstd::nullopt;
        auto [it, fresh] =
            snaps.try_emplace(dstd::String(dstd::StringView(f.index_path)));
        if (fresh) {
            auto opened = trace::index::plan::open_snapshot(it->first);
            if (!opened) return dstd::nullopt;
            it->second.emplace(std::move(*opened));
        }
        const trace::index::Snapshot& snapshot = *it->second;
        const dstd::String logical = fileio::get_logical_path(f.file_path);
        common::InputFileStat now;
        if (!common::stat_input(f.file_path, now)) return dstd::nullopt;
        const SourceStat now_stat{static_cast<std::uint64_t>(now.mtime),
                                  now.size};
        if (snapshot.freshness(logical, now_stat) !=
            trace::index::Freshness::Current)
            return dstd::nullopt;
        const auto file = snapshot.file(logical);
        if (!file || !trace::index::extension_current(
                         snapshot, *file, store::IndexExtension::Stats))
            return dstd::nullopt;
        // The statistics are of dftracer events; a file read by path has
        // others, with no clock.
        auto named = trace::index::schema(snapshot, *file);
        if (!named) {
            DFTRACER_UTILS_LOG_WARN(
                "phase seed: the record schema of {} cannot be read: {}",
                logical, named.error().message());
            return dstd::nullopt;
        }
        if (*named) {
            auto registered = trace::index::get_schema(**named);
            if (!registered ||
                (*registered)->decoder != trace::index::Decoder::Dftracer)
                return dstd::nullopt;
        }
        auto scalar = trace::index::scalar_stats(snapshot, *file);
        if (!scalar) {
            DFTRACER_UTILS_LOG_WARN(
                "phase seed: the index statistics of {} cannot be read: {}",
                logical, scalar.error().message());
            return dstd::nullopt;
        }
        if (!*scalar) return dstd::nullopt;
        const auto& stats = (*scalar)->stats;
        total.event_count += stats.total_events;
        total.ts_max = std::max(total.ts_max, stats.max_timestamp_us);
        if (stats.min_nonzero_timestamp_us !=
            std::numeric_limits<std::uint64_t>::max()) {
            total.ts_min = any_start ? std::min(total.ts_min,
                                                stats.min_nonzero_timestamp_us)
                                     : stats.min_nonzero_timestamp_us;
            any_start = true;
        }
    }
    return total;
}

dstd::Optional<dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>>>
input_file_keys(const trace::views::detail::ViewPlan& plan) {
    if (trace::views::detail::has_pin(plan)) return dstd::nullopt;
    dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>> keys;
    keys.reserve(plan.files.size());
    for (const auto& f : plan.files) {
        common::InputFileStat now;
        if (!common::stat_input(f.file_path, now)) return dstd::nullopt;
        dstd::String sig;
        sig.append(dstd::to_string(now.mtime_ns));
        sig.push_back('\0');
        sig.append(dstd::to_string(now.size));
        sig.push_back('\0');
        sig.append(dstd::to_string(fileio::calculate_file_hash(f.file_path)));
        keys.emplace_back(fileio::get_logical_path(f.file_path),
                          dftracer::utils::dstd::hash::xxh3_64(sig));
    }
    return keys;
}

std::uint64_t fingerprint_of_keys(
    dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>> keys) {
    std::sort(keys.begin(), keys.end());
    dstd::String sig;
    auto add = [&](dstd::StringView s) {
        sig.append(s);
        sig.push_back('\0');
    };
    add("phase_seed");
    add(DFTRACER_UTILS_API_VERSION_STRING);
    add(dstd::to_string(DFTRACER_UTILS_INDEX_FORMAT_VERSION));
    for (const auto& [path, key] : keys) {
        add(path);
        add(dstd::to_string(key));
    }
    return dftracer::utils::dstd::hash::xxh3_64(sig);
}

dstd::Optional<std::uint64_t> input_fingerprint(
    const trace::views::detail::ViewPlan& plan) {
    auto keys = input_file_keys(plan);
    if (!keys) return dstd::nullopt;
    return fingerprint_of_keys(std::move(*keys));
}

dstd::String phase_seed_cache_path(const trace::views::detail::ViewPlan& plan) {
    if (trace::views::detail::has_pin(plan)) return {};
    const dstd::String dir = cache_dir(plan);
    return dir.empty() ? dir : trace::index::phase_seeds_dir(dir);
}

dstd::Result<dstd::Optional<InputStats>> read_input_stats(
    CacheStore& db, std::uint64_t fingerprint) {
    auto value = db.get(fingerprint, VALUE_KEY);
    if (!value) return value.error();
    if (!*value) return dstd::Optional<InputStats>{};
    dstd::BeReader in(reinterpret_cast<const std::uint8_t*>((*value)->data()),
                      (*value)->size());
    std::uint8_t format = 0;
    InputStats stats;
    if (!in.u8(format) || format != VALUE_FORMAT ||
        in.remaining() != VALUE_BYTES - 1 || !in.u64(stats.event_count) ||
        !in.u64(stats.ts_min) || !in.u64(stats.ts_max))
        return dstd::Optional<InputStats>{};
    return dstd::Optional<InputStats>(stats);
}

dstd::Result<void> persist_input_stats(CacheStore& db,
                                       std::uint64_t fingerprint,
                                       const InputStats& stats) {
    dstd::String value;
    value.push_back(static_cast<char>(VALUE_FORMAT));
    dstd::append_u64_be(value, stats.event_count);
    dstd::append_u64_be(value, stats.ts_min);
    dstd::append_u64_be(value, stats.ts_max);
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> entries;
    entries.emplace_back(VALUE_KEY, value);
    return db.put(fingerprint, entries,
                  2 * (1 + SIG_BYTES) + value.size() + USAGE_BYTES);
}

}  // namespace dftracer::utils::trace::index::cache
