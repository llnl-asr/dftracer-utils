// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/trace/index/store/layout.h>
#include <dftracer/utils/trace/index/tables.h>

#include <array>
#include <utility>

namespace dftracer::utils::trace::index::schemas::dft::agg::tier {

namespace {

constexpr dstd::StringView CONFIG_NAME = "config";
constexpr dstd::StringView TIME_BOUNDS_NAME = "time_bounds";
constexpr std::size_t CONFIG_FIXED_BYTES = 37;
constexpr char KEY_SEP = '\x1f';
constexpr std::uint8_t FLAG_GROUP_BY_FILE = 1U << 0;
constexpr std::uint8_t FLAG_RELATIVE_TIME = 1U << 1;
constexpr std::uint8_t FLAG_NORMALIZE_TIME = 1U << 2;
constexpr std::uint8_t FLAG_STATISTICS = 1U << 3;
constexpr std::uint8_t FLAG_PERCENTILES = 1U << 4;
// An earlier record has only the first bit and no settings after the flags.
constexpr std::uint8_t FLAG_SETTINGS = 1U << 7;

constexpr std::array<dstd::StringView, 2> META_NAMES = {CONFIG_NAME,
                                                        TIME_BOUNDS_NAME};

/// Publishes one index-wide record. Blocking, so a caller inside a coroutine
/// does not have to await it.
dstd::Result<void> publish_meta(const Index& index, dstd::StringView name,
                                dstd::StringView value) {
    dstd::Expected<void, dstd::Error> published;
    auto ran = default_runtime().block_on(
        coro::named("aggregation meta", [&]() -> coro::CoroTask<void> {
            published = co_await put_agg_meta(index, name, value);
        }()));
    if (!ran) return ran.error();
    return published;
}

dstd::Result<void> drop_meta(const Index& index, dstd::StringView name) {
    dstd::Expected<void, dstd::Error> dropped;
    auto ran = default_runtime().block_on(
        coro::named("aggregation meta drop", [&]() -> coro::CoroTask<void> {
            dropped = co_await delete_agg_meta(index, name);
        }()));
    if (!ran) return ran.error();
    return dropped;
}

dstd::Optional<Config> decode_config(dstd::StringView value) {
    if (value.size() < CONFIG_FIXED_BYTES) return dstd::nullopt;
    const auto flags = static_cast<std::uint8_t>(value[16]);
    if ((flags & FLAG_SETTINGS) == 0) return dstd::nullopt;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
    Config c;
    c.time_interval_us = dstd::read_u64_be(bytes);
    c.params_hash = dstd::read_u64_be(bytes + 8);
    c.group_by_file = (flags & FLAG_GROUP_BY_FILE) != 0;
    c.use_relative_time = (flags & FLAG_RELATIVE_TIME) != 0;
    c.normalize_time = (flags & FLAG_NORMALIZE_TIME) != 0;
    c.compute_statistics = (flags & FLAG_STATISTICS) != 0;
    c.compute_percentiles = (flags & FLAG_PERCENTILES) != 0;
    c.reference_timestamp = dstd::read_u64_be(bytes + 17);
    c.sketch_accuracy = dstd::read_f64_be(bytes + 25);
    const std::uint32_t count = dstd::read_u32_be(bytes + 33);
    if (value.size() - CONFIG_FIXED_BYTES < std::size_t{count} * 8)
        return dstd::nullopt;
    const std::uint8_t* at = bytes + CONFIG_FIXED_BYTES;
    for (std::uint32_t i = 0; i < count; ++i, at += 8)
        c.percentiles.push_back(dstd::read_f64_be(at));
    // The extra group keys follow, joined by 0x1F.
    dstd::StringView keys = value.substr(CONFIG_FIXED_BYTES + count * 8);
    while (!keys.empty()) {
        const std::size_t at_sep = keys.find(KEY_SEP);
        c.extra_group_keys.emplace_back(keys.substr(0, at_sep));
        if (at_sep == dstd::StringView::npos) break;
        keys.remove_prefix(at_sep + 1);
    }
    return c;
}

dstd::Optional<TimeBounds> decode_time_bounds(dstd::StringView value) {
    if (value.size() < 16) return dstd::nullopt;
    return TimeBounds{trace::index::store::layout::read_u64(value),
                      trace::index::store::layout::read_u64(value.substr(8))};
}

}  // namespace

Config make_config(const AggregationConfig& config) {
    Config c;
    c.time_interval_us = config.time_interval_us;
    c.params_hash = config.params_hash();
    c.group_by_file = config.group_by_file;
    c.extra_group_keys = config.extra_group_keys;
    c.use_relative_time = config.use_relative_time;
    c.normalize_time = config.normalize_time;
    c.compute_statistics = config.compute_statistics;
    c.compute_percentiles = config.compute_percentiles;
    c.reference_timestamp = config.reference_timestamp;
    c.sketch_accuracy = config.sketch_accuracy;
    c.percentiles = config.percentiles;
    return c;
}

AggregationConfig aggregation_config_of(const Config& config) {
    AggregationConfig c;
    c.time_interval_us = config.time_interval_us;
    c.group_by_file = config.group_by_file;
    c.extra_group_keys = config.extra_group_keys;
    c.use_relative_time = config.use_relative_time;
    c.normalize_time = config.normalize_time;
    c.compute_statistics = config.compute_statistics;
    c.compute_percentiles = config.compute_percentiles;
    c.reference_timestamp = config.reference_timestamp;
    c.sketch_accuracy = config.sketch_accuracy;
    c.percentiles = config.percentiles;
    return c;
}

dstd::Expected<dstd::Optional<Config>, dstd::Error> read_config(
    const Snapshot& snapshot) {
    auto value = agg_meta(snapshot, CONFIG_NAME);
    if (!value) return dstd::Unexpected<dstd::Error>(value.error());
    if (!*value) return dstd::Optional<Config>{};
    return decode_config(dstd::StringView((*value)->data(), (*value)->size()));
}

dstd::Result<AggregationConfig> serving_config(const Snapshot& snapshot,
                                               const AggregationConfig& want) {
    auto stored = read_config(snapshot);
    if (!stored) return stored.error();
    if (!*stored) return want;
    auto config = aggregation_config_of(**stored);
    if (config.params_hash() != (*stored)->params_hash || !config.covers(want))
        return want;
    return config;
}

dstd::Expected<bool, dstd::Error> has_config_record(const Snapshot& snapshot) {
    auto value = agg_meta(snapshot, CONFIG_NAME);
    if (!value) return dstd::Unexpected<dstd::Error>(value.error());
    return value->has_value();
}

dstd::Result<void> write_config(const Index& index, const Config& config) {
    for (const auto& key : config.extra_group_keys)
        if (key.empty() || key.find(KEY_SEP) != dstd::String::npos)
            return common::make_error<
                "an extra group key cannot be empty or hold 0x1F: '{}'">(
                common::ErrorCode::InvalidArgument, key);
    dstd::String value;
    dstd::append_u64_be(value, config.time_interval_us);
    dstd::append_u64_be(value, config.params_hash);
    std::uint8_t flags = FLAG_SETTINGS;
    if (config.group_by_file) flags |= FLAG_GROUP_BY_FILE;
    if (config.use_relative_time) flags |= FLAG_RELATIVE_TIME;
    if (config.normalize_time) flags |= FLAG_NORMALIZE_TIME;
    if (config.compute_statistics) flags |= FLAG_STATISTICS;
    if (config.compute_percentiles) flags |= FLAG_PERCENTILES;
    value.push_back(static_cast<char>(flags));
    dstd::append_u64_be(value, config.reference_timestamp);
    dstd::append_f64_be(value, config.sketch_accuracy);
    dstd::append_u32_be(value,
                        static_cast<std::uint32_t>(config.percentiles.size()));
    for (const double p : config.percentiles) dstd::append_f64_be(value, p);
    for (std::size_t i = 0; i < config.extra_group_keys.size(); ++i) {
        if (i != 0) value.push_back(KEY_SEP);
        value.append(config.extra_group_keys[i]);
    }
    return publish_meta(index, CONFIG_NAME, value);
}

dstd::Expected<dstd::Optional<TimeBounds>, dstd::Error> read_time_bounds(
    const Snapshot& snapshot) {
    auto value = agg_meta(snapshot, TIME_BOUNDS_NAME);
    if (!value) return dstd::Unexpected<dstd::Error>(value.error());
    if (!*value) return dstd::Optional<TimeBounds>{};
    return decode_time_bounds(
        dstd::StringView((*value)->data(), (*value)->size()));
}

dstd::Result<void> write_time_bounds(const Index& index, TimeBounds bounds) {
    dstd::String value;
    dstd::append_u64_be(value, bounds.min_bucket);
    dstd::append_u64_be(value, bounds.max_bucket);
    return publish_meta(index, TIME_BOUNDS_NAME, value);
}

dstd::Result<void> clear_files(const Index& index,
                               dstd::Span<const dstd::StringView> paths) {
    dstd::Expected<void, dstd::Error> dropped;
    auto ran = default_runtime().block_on(
        coro::named("aggregation files clear", [&]() -> coro::CoroTask<void> {
            dropped = co_await remove_extension(
                index, trace::index::store::layout::Ext::Agg, paths);
        }()));
    if (!ran) return ran.error();
    return dropped;
}

dstd::Result<void> clear(const Index& index) {
    auto snapshot = index.snapshot();
    if (!snapshot) return snapshot.error();
    auto files = snapshot->files();
    if (!files) return files.error();
    dstd::Vector<dstd::StringView> paths;
    paths.reserve(files->size());
    for (const FileRef& file : *files) paths.push_back(file.path());
    auto dropped = clear_files(
        index, dstd::Span<const dstd::StringView>(paths.data(), paths.size()));
    if (!dropped) return dropped;
    for (const dstd::StringView name : META_NAMES) {
        auto gone = drop_meta(index, name);
        if (!gone) return gone;
    }
    return {};
}

}  // namespace dftracer::utils::trace::index::schemas::dft::agg::tier
