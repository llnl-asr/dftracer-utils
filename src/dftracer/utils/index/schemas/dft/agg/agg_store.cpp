#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/agg_row_reader.h>
#include <dftracer/utils/index/store/layout.h>

#include <algorithm>
#include <array>

namespace dftracer::utils::index::schemas::dft::agg::tier {

namespace layout = index::store::layout;
using index::store::AggRowReader;

namespace {

constexpr std::string_view CONFIG_NAME = "config";
constexpr std::string_view TIME_BOUNDS_NAME = "time_bounds";
constexpr std::size_t CONFIG_FIXED_BYTES = 17;
constexpr char KEY_SEP = '\x1f';
constexpr std::uint8_t FLAG_GROUP_BY_FILE = 1U << 0;

constexpr std::array<std::string_view, 2> META_NAMES = {CONFIG_NAME,
                                                        TIME_BOUNDS_NAME};

std::string meta_key(std::string_view name) {
    auto key = layout::file_prefix(layout::Ext::Agg, layout::agg_kind::META,
                                   layout::INDEX_WIDE);
    key.append(name);
    return key;
}

std::string dict_prefix() {
    return layout::kind_prefix(layout::Ext::Agg, layout::agg_kind::DICT);
}

// nullopt when the record is absent or in another format.
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

std::uint32_t entry_id(std::string_view prefix, std::string_view key) {
    if (key.size() != prefix.size() + 2 * sizeof(std::uint32_t))
        throw DFTUtilsException(ErrorCode::Parse,
                                "aggregation dictionary: malformed entry");
    return layout::read_u32(key.substr(prefix.size() + sizeof(std::uint32_t)));
}

void load_file_texts(const IndexDatabase& db, int file_id,
                     dstd::HashMap<std::uint32_t, dstd::String>& texts) {
    const auto entries = dict_prefix();
    const auto range =
        layout::file_prefix(layout::Ext::Agg, layout::agg_kind::DICT,
                            static_cast<std::uint32_t>(file_id));
    AggRowReader::for_each_row(
        db, layout::agg_kind::DICT, range,
        [&](std::string_view dict_key, std::string_view text) {
            texts.try_emplace(entry_id(entries, dict_key), dstd::String(text));
            return true;
        });
}

/// An id the file holds no entry for gets the empty text.
void resolve_texts(const dstd::HashMap<std::uint32_t, dstd::String>& texts,
                   ChunkColumns& block) {
    block.texts.resize(block.ids.size());
    for (std::size_t i = 0; i < block.ids.size(); ++i) {
        const auto at = texts.find(block.ids[i]);
        if (at != texts.end()) block.texts[i] = at->second;
    }
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
            if (static_cast<std::uint8_t>(tail[20]) >
                static_cast<std::uint8_t>(PartialKind::Aggregated))
                throw DFTUtilsException(ErrorCode::Parse,
                                        "aggregation partial: unknown kind");
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

void for_each_chunk_columns(
    const IndexDatabase& db, int file_id,
    const std::function<bool(std::uint64_t, const ChunkColumns&)>& fn) {
    constexpr std::size_t RECORD_BYTES = sizeof(std::uint32_t);
    const auto prefix =
        layout::file_prefix(layout::Ext::Agg, layout::agg_kind::COLUMNS,
                            static_cast<std::uint32_t>(file_id));
    dstd::HashMap<std::uint32_t, dstd::String> texts;
    bool texts_read = false;
    AggRowReader::for_each_row(
        db, layout::agg_kind::COLUMNS, prefix,
        [&](std::string_view key, std::string_view body) {
            if (key.size() != prefix.size() + RECORD_BYTES)
                throw DFTUtilsException(ErrorCode::Parse,
                                        "chunk columns: bad key");
            if (!texts_read) {
                load_file_texts(db, file_id, texts);
                texts_read = true;
            }
            auto block = decode_chunk_columns(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(body.data()),
                body.size()));
            if (!block)
                throw DFTUtilsException(ErrorCode::Parse,
                                        "chunk columns: malformed record");
            block->chunk = layout::read_u32(key.substr(prefix.size()));
            resolve_texts(texts, *block);
            return fn(block->chunk, *block);
        });
}

bool get_chunk_columns(const IndexDatabase& db, int file_id,
                       std::uint64_t chunk, ChunkColumns& out) {
    const auto body = AggRowReader::get_row(
        db, layout::agg_kind::COLUMNS,
        layout::granule_key(layout::Ext::Agg, layout::agg_kind::COLUMNS,
                            static_cast<std::uint32_t>(file_id),
                            static_cast<std::uint32_t>(chunk)));
    if (!body) return false;
    auto block = decode_chunk_columns(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body->data()), body->size()));
    if (!block)
        throw DFTUtilsException(ErrorCode::Parse,
                                "chunk columns: malformed record");
    block->chunk = chunk;
    dstd::HashMap<std::uint32_t, dstd::String> texts;
    load_file_texts(db, file_id, texts);
    resolve_texts(texts, *block);
    out = std::move(*block);
    return true;
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
