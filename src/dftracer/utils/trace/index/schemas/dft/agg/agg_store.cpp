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
constexpr std::size_t CONFIG_FIXED_BYTES = 17;
constexpr char KEY_SEP = '\x1f';
constexpr std::uint8_t FLAG_GROUP_BY_FILE = 1U << 0;

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
    Config c;
    c.time_interval_us = trace::index::store::layout::read_u64(value);
    c.params_hash = trace::index::store::layout::read_u64(value.substr(8));
    c.group_by_file =
        (static_cast<std::uint8_t>(value[16]) & FLAG_GROUP_BY_FILE) != 0;
    // The extra group keys follow, joined by 0x1F.
    dstd::StringView keys = value.substr(CONFIG_FIXED_BYTES);
    while (!keys.empty()) {
        const std::size_t at = keys.find(KEY_SEP);
        c.extra_group_keys.emplace_back(keys.substr(0, at));
        if (at == dstd::StringView::npos) break;
        keys.remove_prefix(at + 1);
    }
    return c;
}

dstd::Optional<TimeBounds> decode_time_bounds(dstd::StringView value) {
    if (value.size() < 16) return dstd::nullopt;
    return TimeBounds{trace::index::store::layout::read_u64(value),
                      trace::index::store::layout::read_u64(value.substr(8))};
}

}  // namespace

dstd::Expected<dstd::Optional<Config>, dstd::Error> read_config(
    const Snapshot& snapshot) {
    auto value = agg_meta(snapshot, CONFIG_NAME);
    if (!value) return dstd::Unexpected<dstd::Error>(value.error());
    if (!*value) return dstd::Optional<Config>{};
    return decode_config(dstd::StringView((*value)->data(), (*value)->size()));
}

dstd::Result<void> write_config(const Index& index, const Config& config) {
    dstd::String value;
    dstd::append_u64_be(value, config.time_interval_us);
    dstd::append_u64_be(value, config.params_hash);
    value.push_back(
        static_cast<char>(config.group_by_file ? FLAG_GROUP_BY_FILE : 0));
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

dstd::Result<void> clear(const Index& index) {
    auto snapshot = index.snapshot();
    if (!snapshot) return snapshot.error();
    auto files = snapshot->files();
    if (!files) return files.error();
    dstd::Vector<dstd::StringView> paths;
    paths.reserve(files->size());
    for (const FileRef& file : *files) paths.push_back(file.path());
    const dstd::Span<const dstd::StringView> listed(paths.data(), paths.size());

    dstd::Expected<void, dstd::Error> dropped;
    auto ran = default_runtime().block_on(
        coro::named("aggregation tier clear", [&]() -> coro::CoroTask<void> {
            dropped = co_await remove_extension(
                index, trace::index::store::layout::Ext::Agg, listed);
        }()));
    if (!ran) return ran.error();
    if (!dropped) return dropped.error();
    for (const dstd::StringView name : META_NAMES) {
        auto gone = drop_meta(index, name);
        if (!gone) return gone;
    }
    return {};
}

}  // namespace dftracer::utils::trace::index::schemas::dft::agg::tier
