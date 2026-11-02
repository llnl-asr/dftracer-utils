#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/runtime.h>

#include <array>
#include <utility>

namespace dftracer::utils::index::schemas::dft::agg::tier {

namespace layout = index::store::layout;

namespace {

constexpr std::string_view CONFIG_NAME = "config";
constexpr std::string_view TIME_BOUNDS_NAME = "time_bounds";
constexpr std::size_t CONFIG_FIXED_BYTES = 17;
constexpr char KEY_SEP = '\x1f';
constexpr std::uint8_t FLAG_GROUP_BY_FILE = 1U << 0;

constexpr std::array<std::string_view, 2> META_NAMES = {CONFIG_NAME,
                                                        TIME_BOUNDS_NAME};

/// Publishes one index-wide record. Blocking, so a caller inside a coroutine
/// does not have to await it.
void publish_meta(const Index& index, std::string_view name,
                  std::string_view value) {
    dstd::Expected<void, dstd::Error> published;
    default_runtime().run_blocking(
        "aggregation meta", [&](CoroScope&) -> coro::CoroTask<void> {
            published = co_await put_agg_meta(index, name, value);
        });
    common::unwrap(std::move(published));
}

void drop_meta(const Index& index, std::string_view name) {
    dstd::Expected<void, dstd::Error> dropped;
    default_runtime().run_blocking(
        "aggregation meta drop", [&](CoroScope&) -> coro::CoroTask<void> {
            dropped = co_await delete_agg_meta(index, name);
        });
    common::unwrap(std::move(dropped));
}

std::optional<Config> decode_config(std::string_view value) {
    if (value.size() < CONFIG_FIXED_BYTES) return std::nullopt;
    Config c;
    c.time_interval_us = layout::read_u64(value);
    c.params_hash = layout::read_u64(value.substr(8));
    c.group_by_file =
        (static_cast<std::uint8_t>(value[16]) & FLAG_GROUP_BY_FILE) != 0;
    // The extra group keys follow, joined by 0x1F.
    std::string_view keys = value.substr(CONFIG_FIXED_BYTES);
    while (!keys.empty()) {
        const std::size_t at = keys.find(KEY_SEP);
        c.extra_group_keys.emplace_back(keys.substr(0, at));
        if (at == std::string_view::npos) break;
        keys.remove_prefix(at + 1);
    }
    return c;
}

std::optional<TimeBounds> decode_time_bounds(std::string_view value) {
    if (value.size() < 16) return std::nullopt;
    return TimeBounds{layout::read_u64(value),
                      layout::read_u64(value.substr(8))};
}

}  // namespace

dstd::Expected<std::optional<Config>, dstd::Error> read_config(
    const Snapshot& snapshot) {
    auto value = agg_meta(snapshot, CONFIG_NAME);
    if (!value) return dstd::Unexpected<dstd::Error>(value.error());
    if (!*value) return std::optional<Config>{};
    return decode_config(std::string_view((*value)->data(), (*value)->size()));
}

void write_config(const Index& index, const Config& config) {
    std::string value;
    dstd::append_u64_be(value, config.time_interval_us);
    dstd::append_u64_be(value, config.params_hash);
    value.push_back(
        static_cast<char>(config.group_by_file ? FLAG_GROUP_BY_FILE : 0));
    for (std::size_t i = 0; i < config.extra_group_keys.size(); ++i) {
        if (i != 0) value.push_back(KEY_SEP);
        value.append(config.extra_group_keys[i]);
    }
    publish_meta(index, CONFIG_NAME, value);
}

dstd::Expected<std::optional<TimeBounds>, dstd::Error> read_time_bounds(
    const Snapshot& snapshot) {
    auto value = agg_meta(snapshot, TIME_BOUNDS_NAME);
    if (!value) return dstd::Unexpected<dstd::Error>(value.error());
    if (!*value) return std::optional<TimeBounds>{};
    return decode_time_bounds(
        std::string_view((*value)->data(), (*value)->size()));
}

void write_time_bounds(const Index& index, TimeBounds bounds) {
    std::string value;
    dstd::append_u64_be(value, bounds.min_bucket);
    dstd::append_u64_be(value, bounds.max_bucket);
    publish_meta(index, TIME_BOUNDS_NAME, value);
}

void clear(const Index& index) {
    auto snapshot = common::unwrap(index.snapshot());
    auto files = common::unwrap(snapshot.files());
    dstd::Vector<dstd::StringView> paths;
    paths.reserve(files.size());
    for (const FileRef& file : files) paths.push_back(file.path());
    const dstd::Span<const dstd::StringView> listed(paths.data(), paths.size());

    dstd::Expected<void, dstd::Error> dropped;
    default_runtime().run_blocking(
        "aggregation tier clear", [&](CoroScope&) -> coro::CoroTask<void> {
            dropped =
                co_await remove_extension(index, layout::Ext::Agg, listed);
        });
    common::unwrap(std::move(dropped));
    for (const std::string_view name : META_NAMES) drop_meta(index, name);
}

}  // namespace dftracer::utils::index::schemas::dft::agg::tier
