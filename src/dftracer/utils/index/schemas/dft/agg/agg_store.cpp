#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/agg_row_reader.h>
#include <dftracer/utils/index/store/layout.h>

#include <algorithm>
#include <array>
#include <map>

namespace dftracer::utils::index::schemas::dft::agg::tier {

namespace layout = index::store::layout;
using index::store::AggRowReader;

namespace {

constexpr std::string_view CONFIG_NAME = "config";
constexpr std::string_view TIME_BOUNDS_NAME = "time_bounds";
constexpr std::string_view TRACKER_NAME = "tracker";
constexpr std::size_t CONFIG_FIXED_BYTES = 17;
constexpr char KEY_SEP = '\x1f';
constexpr std::uint8_t FLAG_GROUP_BY_FILE = 1U << 0;

constexpr std::array<std::string_view, 3> META_NAMES = {
    CONFIG_NAME, TIME_BOUNDS_NAME, TRACKER_NAME};

std::string meta_key(std::string_view name) {
    auto key = layout::file_prefix(layout::Ext::Agg, layout::agg_kind::META,
                                   layout::INDEX_WIDE);
    key.append(name);
    return key;
}

std::string dict_prefix() {
    return layout::kind_prefix(layout::Ext::Agg, layout::agg_kind::DICT);
}

// The payload of the index-wide record `name`; nullopt when it is absent or
// in another format.
std::optional<std::string> get_meta(const IndexDatabase& db,
                                    std::string_view name) {
    return AggRowReader::get_row(db, layout::agg_kind::META, meta_key(name));
}

void put_meta(IndexDatabase& db, std::string_view name,
              std::string_view value) {
    auto write = db.begin_write();
    index::store::records::put_agg_meta(*write, name, value);
    write->commit();
}

[[noreturn]] void throw_conflict(std::uint32_t id, std::string_view existing,
                                 std::string_view text) {
    throw DFTUtilsException(
        ErrorCode::InvalidArgument,
        "string intern: id " + std::to_string(id) + " is already '" +
            std::string(existing) + "', cannot rebind to '" +
            std::string(text) +
            "' (dictionaries from two indexes in one table?)");
}

/// The id of one dictionary record, whose key is the file prefix and the id.
std::uint32_t entry_id(std::string_view prefix, std::string_view key) {
    if (key.size() != prefix.size() + 2 * sizeof(std::uint32_t))
        throw DFTUtilsException(ErrorCode::Parse,
                                "aggregation dictionary: malformed entry");
    return layout::read_u32(key.substr(prefix.size() + sizeof(std::uint32_t)));
}

std::string_view row_key_of(std::string_view key, std::string_view prefix) {
    return key.substr(prefix.size() + sizeof(std::uint32_t));
}

std::uint16_t shard_of(std::string_view row) {
    return read_u16_be(reinterpret_cast<const std::uint8_t*>(row.data()));
}

}  // namespace

std::optional<Config> read_config(const IndexDatabase& db) {
    auto value = get_meta(db, CONFIG_NAME);
    if (!value || value->size() < CONFIG_FIXED_BYTES) return std::nullopt;
    Config c;
    c.time_interval_us = layout::read_u64(*value);
    c.params_hash = layout::read_u64(std::string_view(*value).substr(8));
    c.group_by_file =
        (static_cast<std::uint8_t>((*value)[16]) & FLAG_GROUP_BY_FILE) != 0;
    // The extra group keys follow, joined by 0x1F.
    std::string_view keys = std::string_view(*value).substr(CONFIG_FIXED_BYTES);
    while (!keys.empty()) {
        const std::size_t at = keys.find(KEY_SEP);
        c.extra_group_keys.emplace_back(keys.substr(0, at));
        if (at == std::string_view::npos) break;
        keys.remove_prefix(at + 1);
    }
    return c;
}

void write_config(IndexDatabase& db, const Config& config) {
    std::string value;
    append_u64_be(value, config.time_interval_us);
    append_u64_be(value, config.params_hash);
    value.push_back(
        static_cast<char>(config.group_by_file ? FLAG_GROUP_BY_FILE : 0));
    for (std::size_t i = 0; i < config.extra_group_keys.size(); ++i) {
        if (i != 0) value.push_back(KEY_SEP);
        value.append(config.extra_group_keys[i]);
    }
    put_meta(db, CONFIG_NAME, value);
}

void put_file(IndexWrite& w, int file_id, std::uint64_t params_hash) {
    index::store::records::put_manifest(w, file_id, layout::Ext::Agg,
                                        params_hash);
}

void put_files(IndexDatabase& db, std::span<const int> file_ids,
               std::uint64_t params_hash) {
    auto write = db.begin_write();
    for (const int file_id : file_ids)
        if (file_id >= 0) put_file(*write, file_id, params_hash);
    write->commit();
}

std::vector<SystemRow> system_rows(const IndexDatabase& db, std::uint16_t begin,
                                   std::uint16_t end) {
    const auto prefix =
        layout::kind_prefix(layout::Ext::Agg, layout::agg_kind::SYSTEM);
    std::map<std::string, SystemAggregationMetrics, std::less<>> merged;
    // The rows of one file are in shard order, so each file's scan stops at
    // the end of the range.
    for (const std::uint32_t file_id : AggRowReader::file_ids(db)) {
        AggRowReader::for_each_row(
            db, layout::agg_kind::SYSTEM,
            layout::file_prefix(layout::Ext::Agg, layout::agg_kind::SYSTEM,
                                file_id),
            [&](std::string_view key, std::string_view body) {
                const std::string_view row = row_key_of(key, prefix);
                if (row.size() < 2) return true;
                const std::uint16_t shard = shard_of(row);
                if (shard >= end || shard >= AGG_KEY_NUM_SHARDS) return false;
                if (shard < begin) return true;
                auto at = merged.find(row);
                if (at == merged.end())
                    merged.emplace(std::string(row),
                                   deserialize_system_value(body));
                else
                    at->second.merge_from(deserialize_system_value(body));
                return true;
            });
    }
    std::vector<SystemRow> out;
    out.reserve(merged.size());
    for (auto& [row, metrics] : merged)
        out.emplace_back(row, serialize_system_value(metrics));
    return out;
}

void for_each_system_row(
    const IndexDatabase& db, int file_id,
    const std::function<bool(std::string_view, std::string_view)>& fn) {
    const auto prefix =
        layout::file_prefix(layout::Ext::Agg, layout::agg_kind::SYSTEM,
                            static_cast<std::uint32_t>(file_id));
    AggRowReader::for_each_row(
        db, layout::agg_kind::SYSTEM, prefix,
        [&](std::string_view key, std::string_view body) {
            return fn(row_key_of(key, prefix), body);
        });
}

void for_each_partial(
    const IndexDatabase& db, int file_id,
    const std::function<bool(std::uint64_t, std::uint32_t, std::uint32_t,
                             std::uint32_t, std::uint32_t, PartialKind,
                             std::span<const std::uint8_t>)>& fn) {
    constexpr std::size_t RECORD_BYTES =
        5 * sizeof(std::uint32_t) + sizeof(std::uint8_t);
    const auto prefix =
        layout::file_prefix(layout::Ext::Agg, layout::agg_kind::PARTIAL,
                            static_cast<std::uint32_t>(file_id));
    AggRowReader::for_each_row(
        db, layout::agg_kind::PARTIAL, prefix,
        [&](std::string_view key, std::string_view body) {
            if (key.size() != prefix.size() + RECORD_BYTES)
                throw DFTUtilsException(
                    ErrorCode::Parse, "aggregation partial: malformed record");
            const auto tail = key.substr(prefix.size());
            return fn(
                layout::read_u32(tail), layout::read_u32(tail.substr(4)),
                layout::read_u32(tail.substr(8)),
                layout::read_u32(tail.substr(12)),
                layout::read_u32(tail.substr(16)),
                static_cast<PartialKind>(static_cast<std::uint8_t>(tail[20])),
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(body.data()),
                    body.size()));
        });
}

std::optional<TimeBounds> read_time_bounds(const IndexDatabase& db) {
    auto value = get_meta(db, TIME_BOUNDS_NAME);
    if (!value || value->size() < 16) return std::nullopt;
    return TimeBounds{layout::read_u64(*value),
                      layout::read_u64(std::string_view(*value).substr(8))};
}

void widen_time_bounds(IndexDatabase& db, TimeBounds bounds) {
    if (auto stored = read_time_bounds(db)) {
        bounds.min_bucket = std::min(bounds.min_bucket, stored->min_bucket);
        bounds.max_bucket = std::max(bounds.max_bucket, stored->max_bucket);
    }
    std::string value;
    append_u64_be(value, bounds.min_bucket);
    append_u64_be(value, bounds.max_bucket);
    put_meta(db, TIME_BOUNDS_NAME, value);
}

std::optional<std::string> read_tracker(const IndexDatabase& db) {
    return get_meta(db, TRACKER_NAME);
}

void write_tracker(IndexDatabase& db, std::string_view bytes) {
    put_meta(db, TRACKER_NAME, bytes);
}

void load_dictionary(const IndexDatabase& db, AggInternTable& table) {
    const auto prefix = dict_prefix();
    AggRowReader::for_each_row(
        db, layout::agg_kind::DICT, prefix,
        [&](std::string_view key, std::string_view body) {
            const std::uint32_t id = entry_id(prefix, key);
            switch (table.intern.insert_at_id(id, body)) {
                case dstd::StringIntern::InsertStatus::Ok:
                    break;
                case dstd::StringIntern::InsertStatus::Conflict:
                    throw_conflict(id, table.intern.resolve(id), body);
                case dstd::StringIntern::InsertStatus::OutOfRange:
                    throw DFTUtilsException(ErrorCode::InvalidArgument,
                                            "string intern: id " +
                                                std::to_string(id) +
                                                " is out of range");
            }
            return true;
        });
}

Dictionary::Dictionary(const IndexDatabase& db) {
    const auto prefix = dict_prefix();
    AggRowReader::for_each_row(
        db, layout::agg_kind::DICT, prefix,
        [&](std::string_view key, std::string_view body) {
            const std::uint32_t id = entry_id(prefix, key);
            auto [at, inserted] = entries_.try_emplace(id, body);
            if (!inserted && at->second != body)
                throw_conflict(id, at->second, body);
            return true;
        });
}

const std::string* Dictionary::find(std::uint32_t id) const {
    auto at = entries_.find(id);
    return at == entries_.end() ? nullptr : &at->second;
}

std::optional<std::string> read_dictionary_entry(const IndexDatabase& db,
                                                 std::uint32_t id) {
    const auto prefix = dict_prefix();
    std::optional<std::string> out;
    AggRowReader::for_each_row(
        db, layout::agg_kind::DICT, prefix,
        [&](std::string_view key, std::string_view body) {
            if (entry_id(prefix, key) != id) return true;
            out = std::string(body);
            return false;
        });
    return out;
}

void clear(IndexDatabase& db) {
    const auto file_ids = AggRowReader::file_ids(db);
    auto write = db.begin_write();
    for (const std::uint32_t file_id : file_ids)
        index::store::records::clear_file(*write, layout::Ext::Agg,
                                          static_cast<int>(file_id));
    for (const std::string_view name : META_NAMES)
        index::store::records::del_agg_meta(*write, name);
    write->commit();
}

}  // namespace dftracer::utils::index::schemas::dft::agg::tier
