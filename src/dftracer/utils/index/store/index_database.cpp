#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_merge_operator.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_merge_operator.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/db_error.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/internal/index_encoding.h>
#include <dftracer/utils/index/store/internal/iterator_codec.h>
#include <dftracer/utils/index/store/internal/payload_codec.h>
#include <dftracer/utils/index/store/internal/scan_prefix.h>
#include <dftracer/utils/index/store/internal/statistics_codec.h>
#include <dftracer/utils/index/store/layout.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store {

namespace rocks = index::store;

using namespace dftracer::utils::index::gzip;
using namespace dftracer::utils::index::store::internal;
using layout::Ext;
using layout::Family;
namespace sk = layout::stats_kind;
namespace pk = layout::path_kind;

ColumnType merge_column_type(ColumnType a, ColumnType b) {
    if (a == b) return a;
    if (a == ColumnType::Unknown) return b;
    if (b == ColumnType::Unknown) return a;
    if ((a == ColumnType::Int64 && b == ColumnType::Float64) ||
        (a == ColumnType::Float64 && b == ColumnType::Int64))
        return ColumnType::Float64;
    return ColumnType::Json;
}

const char* column_type_name(ColumnType t) {
    switch (t) {
        case ColumnType::Int64:
            return "int64";
        case ColumnType::Float64:
            return "float64";
        case ColumnType::String:
            return "string";
        case ColumnType::Json:
            return "json";
        case ColumnType::Unknown:
            return "";
    }
    return "";
}

namespace {

std::uint32_t fid(int file_id) { return static_cast<std::uint32_t>(file_id); }

// Calls fn(key, payload) for every key under `prefix` in `family` whose value
// carries the (ext, kind) header; values with another header are skipped.
template <typename Fn>
void scan(const rocks::RocksDatabase& db, Ext ext, std::uint8_t kind,
          std::string_view prefix, Fn&& fn) {
    scan_prefix_iterator(
        "Failed to scan index data", prefix,
        [&] {
            return db.new_iterator(
                layout::family_name(layout::family_of(ext, kind)));
        },
        [&](::rocksdb::Iterator& it) {
            const std::string_view key(it.key().data(), it.key().size());
            const std::string_view value(it.value().data(), it.value().size());
            if (auto body = layout::payload(value, ext, kind)) fn(key, *body);
        });
}

// Presence keys (empty values) under `prefix`.
template <typename Fn>
void scan_keys(const rocks::RocksDatabase& db, Ext ext, std::uint8_t kind,
               std::string_view prefix, Fn&& fn) {
    scan_prefix_iterator(
        "Failed to scan index data", prefix,
        [&] {
            return db.new_iterator(
                layout::family_name(layout::family_of(ext, kind)));
        },
        [&](::rocksdb::Iterator& it) {
            fn(std::string_view(it.key().data(), it.key().size()));
        });
}

// The payload stored at `key` for (ext, kind); nullopt when absent or when
// its header does not match.
std::optional<std::string> get(const rocks::RocksDatabase& db, Ext ext,
                               std::uint8_t kind, std::string_view key) {
    std::string value;
    auto status =
        db.get(key, &value, layout::family_name(layout::family_of(ext, kind)));
    if (status.IsNotFound()) return std::nullopt;
    if (!status.ok()) throw_db_error("Failed to read index data", status);
    auto body = layout::payload(value, ext, kind);
    if (!body) return std::nullopt;
    return std::string(*body);
}

std::uint32_t tail_u32(std::string_view key, std::size_t offset) {
    return layout::read_u32(key.substr(offset));
}

index::schemas::dft::ChunkStatistics decode_chunk_statistics_value(
    std::string_view value) {
    Cursor cursor(value);
    index::schemas::dft::ChunkStatistics stats;
    stats.total_events = cursor.u64();
    stats.min_timestamp_us = cursor.u64();
    stats.max_timestamp_us = cursor.u64();
    stats.duration_sum_us = cursor.i64();
    stats.duration_min_us = cursor.u64();
    stats.duration_max_us = cursor.u64();
    stats.duration_count = cursor.u64();
    stats.duration_m2 = cursor.f64();

    auto duration_sketch = cursor.blob_view();
    if (!duration_sketch.empty()) {
        stats.duration_sketch =
            utilities::common::statistics::DDSketch::deserialize(
                reinterpret_cast<const std::uint8_t*>(duration_sketch.data()),
                duration_sketch.size());
    }

    auto duration_histogram = cursor.str();
    if (!duration_histogram.empty()) {
        stats.duration_histogram =
            utilities::common::statistics::Log2Histogram::from_json(
                duration_histogram);
    }

    auto name_sketches = cursor.blob_view();
    if (!name_sketches.empty()) {
        stats.name_duration_sketches = index::schemas::dft::ChunkStatistics::
            deserialize_name_duration_sketches(
                reinterpret_cast<const std::uint8_t*>(name_sketches.data()),
                name_sketches.size());
    }

    stats.name_duration_histograms =
        index::schemas::dft::ChunkStatistics::parse_histogram_map_json(
            cursor.str());
    stats.name_duration_sums =
        index::schemas::dft::ChunkStatistics::parse_double_map_json(
            cursor.str());
    stats.name_duration_sum_sqs =
        index::schemas::dft::ChunkStatistics::parse_double_map_json(
            cursor.str());
    stats.name_category =
        index::schemas::dft::ChunkStatistics::parse_string_map_json(
            cursor.str());

    auto cat_sketches = cursor.blob_view();
    if (!cat_sketches.empty()) {
        stats.cat_duration_sketches = index::schemas::dft::ChunkStatistics::
            deserialize_name_duration_sketches(
                reinterpret_cast<const std::uint8_t*>(cat_sketches.data()),
                cat_sketches.size());
    }
    stats.cat_duration_sums =
        index::schemas::dft::ChunkStatistics::parse_double_map_json(
            cursor.str());

    auto pid_sketches = cursor.blob_view();
    if (!pid_sketches.empty()) {
        stats.pid_duration_sketches = index::schemas::dft::ChunkStatistics::
            deserialize_name_duration_sketches(
                reinterpret_cast<const std::uint8_t*>(pid_sketches.data()),
                pid_sketches.size());
    }
    stats.pid_duration_sums =
        index::schemas::dft::ChunkStatistics::parse_double_map_json(
            cursor.str());

    return stats;
}

index::gzip::GzipMemberRecord decode_gzip_member(std::string_view key,
                                                 std::string_view body) {
    index::gzip::GzipMemberRecord member;
    member.member_idx = tail_u32(key, layout::KEY_PREFIX_BYTES);
    Cursor cursor(body);
    member.c_offset = cursor.u64();
    member.c_size = cursor.u64();
    member.uc_offset = cursor.u64();
    member.uc_size = cursor.u64();
    member.first_line_num = cursor.u64();
    member.last_line_num = cursor.u64();
    if (body.size() >= 6 * sizeof(std::uint64_t) + 2) {
        const auto kind = cursor.u8();
        if (kind >
            static_cast<std::uint8_t>(index::gzip::GzipRecordKind::RESTART))
            throw DFTUtilsException::cat(ErrorCode::INDEXER,
                                         "unknown gzip record kind ", kind);
        member.kind = static_cast<index::gzip::GzipRecordKind>(kind);
        member.bits = cursor.u8();
    }
    return member;
}

StringViewMap<std::uint64_t> decode_count_map_value(std::string_view value) {
    Cursor cursor(value);
    StringViewMap<std::uint64_t> counts;
    auto num_entries = cursor.u32();
    counts.reserve(num_entries);
    for (std::uint32_t i = 0; i < num_entries; ++i) {
        auto key = cursor.str();
        counts.emplace(std::move(key), cursor.u64());
    }
    return counts;
}

// Rows of (ext, kind) for the files in `file_ids`, in key order:
// fn(file_id, key, payload).
template <typename Fn>
void scan_files(const rocks::RocksDatabase& db, Ext ext, std::uint8_t kind,
                const std::vector<int>& file_ids, Fn&& fn) {
    if (file_ids.empty()) return;
    const ankerl::unordered_dense::set<int> wanted(file_ids.begin(),
                                                   file_ids.end());
    const auto [lo, hi] = std::minmax_element(file_ids.begin(), file_ids.end());
    const auto kind_prefix = layout::kind_prefix(ext, kind);
    const auto start = layout::file_prefix(ext, kind, fid(*lo));
    auto it =
        db.new_iterator(layout::family_name(layout::family_of(ext, kind)));
    for (it->Seek(::rocksdb::Slice(start.data(), start.size())); it->Valid();
         it->Next()) {
        const std::string_view key(it->key().data(), it->key().size());
        if (!key.starts_with(kind_prefix) ||
            key.size() < layout::KEY_PREFIX_BYTES)
            break;
        const int file_id = static_cast<int>(tail_u32(key, 3));
        if (file_id > *hi) break;
        if (!wanted.contains(file_id)) continue;
        const std::string_view value(it->value().data(), it->value().size());
        if (auto body = layout::payload(value, ext, kind))
            fn(file_id, key, *body);
    }
    if (!it->status().ok())
        throw_db_error("Failed to scan index data", it->status());
}

}  // namespace

namespace {

::rocksdb::CompressionType select_compression_type() {
#ifdef DFTRACER_UTILS_ENABLE_ZSTD
    return ::rocksdb::kZSTD;
#elif defined(DFTRACER_UTILS_ENABLE_LZ4)
    return ::rocksdb::kLZ4Compression;
#else
    return ::rocksdb::kZlibCompression;
#endif
}

// Every open registers the aggregation merge operators: RocksDBManager shares
// one handle per path, and ingested aggregation SSTs hold merge operands.
rocks::RocksDatabase::CfOptionsOverride make_aggregation_cf_override() {
    using index::schemas::dft::agg::AggregationMergeOperator;
    using index::schemas::dft::agg::SystemMetricsMergeOperator;
    auto agg_merge_op = std::make_shared<AggregationMergeOperator>();
    auto sys_merge_op = std::make_shared<SystemMetricsMergeOperator>();
    return [agg_merge_op, sys_merge_op](const std::string& cf_name,
                                        ::rocksdb::ColumnFamilyOptions& opts) {
        if (cf_name == cf::AGGREGATION) {
            opts.merge_operator = agg_merge_op;
            ::rocksdb::BlockBasedTableOptions bbt;
            bbt.block_size = 32 * 1024;
            bbt.format_version = 7;
            bbt.index_block_restart_interval = 16;
            bbt.whole_key_filtering = false;
            bbt.separate_key_value_in_data_block = true;
            opts.table_factory.reset(::rocksdb::NewBlockBasedTableFactory(bbt));
            opts.level0_file_num_compaction_trigger = 2;
            opts.max_bytes_for_level_multiplier = 20;
            opts.compression = select_compression_type();
            opts.bottommost_compression = select_compression_type();
        } else if (cf_name == cf::SYSTEM_METRICS) {
            opts.merge_operator = sys_merge_op;
            opts.compression = select_compression_type();
            opts.bottommost_compression = select_compression_type();
        }
    };
}

}  // namespace

struct IndexDatabase::Impl {
    std::string db_path_;
    rocks::RocksDatabase::OpenMode open_mode_ =
        rocks::RocksDatabase::OpenMode::ReadWrite;
    std::shared_ptr<rocks::RocksDatabase> db_;
};

IndexDatabase::IndexDatabase(const std::string& index_path,
                             IndexOpenMode open_mode)
    : impl_(std::make_unique<Impl>()) {
    impl_->db_path_ = index::store::internal::normalize_index_root(index_path);
    impl_->open_mode_ = open_mode == IndexOpenMode::ReadOnly
                            ? rocks::RocksDatabase::OpenMode::ReadOnly
                            : rocks::RocksDatabase::OpenMode::ReadWrite;
    impl_->db_ = rocks::RocksDBManager::instance().get_or_open(
        impl_->db_path_, impl_->open_mode_, make_aggregation_cf_override());
    if (impl_->open_mode_ == rocks::RocksDatabase::OpenMode::ReadWrite) {
        init_schema();
    }
}

IndexDatabase::IndexDatabase(IndexDatabase&&) noexcept = default;
IndexDatabase& IndexDatabase::operator=(IndexDatabase&&) noexcept = default;
IndexDatabase::~IndexDatabase() = default;

std::shared_ptr<rocks::RocksDatabase> IndexDatabase::db() const {
    return impl_->db_;
}

std::unique_ptr<IndexDatabaseWriterContext> IndexDatabase::begin_write() {
    return std::unique_ptr<IndexDatabaseWriterContext>(
        new IndexDatabaseWriterContext(impl_->db_));
}

void IndexDatabase::bulk_ingest(const SstArtifactRegistry& registry,
                                const StringViewSet& skip_cfs) {
    // One atomic multi-CF ingest: a single manifest edit, so the manifest
    // entries in the registry family commit together with their data.
    // Overlapping index-wide SSTs (dictionaries) and aggregation SSTs are fine
    // with allow_global_seqno, which assigns seqnos in list order.
    std::vector<std::pair<std::string_view, const std::vector<std::string>*>>
        per_cf;
    for (std::size_t f = 0; f < layout::FAMILY_COUNT; ++f) {
        const auto family = static_cast<Family>(f);
        const auto name = layout::family_name(family);
        const auto& files = registry.files(family);
        if (files.empty() || skip_cfs.contains(std::string(name))) continue;
        per_cf.emplace_back(name, &files);
    }
    if (per_cf.empty()) return;
    auto status = impl_->db_->ingest_external_files_multi(per_cf);
    if (!status.ok())
        throw_db_error("Failed to ingest SSTs into column family '" +
                           std::string(per_cf.front().first) + "'",
                       status);
}

std::vector<int> IndexDatabase::assign_file_ids(
    const std::vector<std::string>& file_paths) {
    std::vector<int> ids(file_paths.size(), -1);
    std::size_t fresh = 0;
    for (std::size_t i = 0; i < file_paths.size(); ++i) {
        ids[i] = get_file_info_id(get_logical_path(file_paths[i]));
        if (ids[i] < 0) ++fresh;
    }
    if (fresh == 0) return ids;
    int next = reserve_file_id_range(fresh);
    for (auto& id : ids)
        if (id < 0) id = next++;
    return ids;
}

namespace {
std::string next_file_id_value(std::uint32_t next) {
    std::string body;
    layout::append_u32(body, next);
    return layout::with_header(Ext::HOST, layout::host::NEXT_FILE_ID, body);
}
}  // namespace

int IndexDatabase::reserve_file_id_range(std::size_t count) {
    std::uint32_t first = 1;
    if (auto body = get(*impl_->db_, Ext::HOST, layout::host::NEXT_FILE_ID,
                        layout::next_file_id_key()))
        first = layout::read_u32(*body);
    if (count == 0) return static_cast<int>(first);
    auto status = impl_->db_->put(
        layout::next_file_id_key(),
        next_file_id_value(first + static_cast<std::uint32_t>(count)));
    if (!status.ok()) throw_db_error("Failed to advance next file id", status);
    return static_cast<int>(first);
}

void IndexDatabase::init_schema() {
    auto writer = begin_write();
    writer->init_schema();
    writer->commit();
    // Rollups moved to <index>/cache; drop the rows older builds left here.
    auto it = impl_->db_->new_iterator(cf::ROLLUP);
    it->SeekToFirst();
    if (it->Valid()) {
        auto status = impl_->db_->delete_range(
            std::string_view(), std::string(32, '\xFF'), cf::ROLLUP);
        if (!status.ok())
            throw_db_error("Failed to clear the legacy rollup family", status);
    }
}

std::optional<ExtensionState> IndexDatabase::extension_state(
    int file_id, IndexExtension ext) const {
    std::string value;
    auto status = impl_->db_->get(layout::manifest_key(fid(file_id), ext),
                                  &value, cf::DEFAULT);
    if (status.IsNotFound()) return std::nullopt;
    if (!status.ok()) throw_db_error("Failed to read the manifest", status);
    auto entry = layout::decode_manifest(value);
    if (!entry) return std::nullopt;
    return ExtensionState{entry->version, entry->params_hash,
                          entry->status == layout::ExtStatus::READY};
}

std::optional<ExtensionState> IndexDatabase::plugin_extension_state(
    int file_id, std::string_view name) const {
    std::string value;
    auto status = impl_->db_->get(
        layout::plugin_manifest_key(fid(file_id), name), &value, cf::DEFAULT);
    if (status.IsNotFound()) return std::nullopt;
    if (!status.ok()) throw_db_error("Failed to read the manifest", status);
    auto entry = layout::decode_manifest(value);
    if (!entry) return std::nullopt;
    return ExtensionState{entry->version, entry->params_hash,
                          entry->status == layout::ExtStatus::READY};
}

std::vector<std::pair<std::string, ExtensionState>>
IndexDatabase::plugin_extension_states(int file_id) const {
    std::vector<std::pair<std::string, ExtensionState>> out;
    const auto prefix = layout::manifest_key(fid(file_id), Ext::PLUGIN);
    auto it = impl_->db_->new_iterator(cf::DEFAULT);
    for (it->Seek(::rocksdb::Slice(prefix.data(), prefix.size())); it->Valid();
         it->Next()) {
        const std::string_view key(it->key().data(), it->key().size());
        if (!key.starts_with(prefix)) break;
        if (key.size() <= prefix.size() || key.back() != '\0') continue;
        auto entry = layout::decode_manifest(
            std::string_view(it->value().data(), it->value().size()));
        if (!entry) continue;
        out.emplace_back(
            std::string(
                key.substr(prefix.size(), key.size() - prefix.size() - 1)),
            ExtensionState{entry->version, entry->params_hash,
                           entry->status == layout::ExtStatus::READY});
    }
    if (!it->status().ok())
        throw_db_error("Failed to read the manifest", it->status());
    return out;
}

bool IndexDatabase::extension_current(int file_id, IndexExtension ext) const {
    auto state = extension_state(file_id, ext);
    return state && state->ready && state->version == layout::ext_version(ext);
}

bool IndexDatabase::pruning_tier_current(int file_id) const {
    return extension_current(file_id, Ext::STATS);
}

std::vector<std::string> IndexDatabase::extension_paths(
    int file_id, IndexExtension ext) const {
    std::vector<std::string> paths;
    const auto prefix = layout::file_prefix(ext, pk::PATHS, fid(file_id));
    scan_keys(*impl_->db_, ext, pk::PATHS, prefix, [&](std::string_view key) {
        paths.emplace_back(key.substr(prefix.size()));
    });
    return paths;
}

std::vector<std::pair<std::uint64_t, std::string>> IndexDatabase::path_granules(
    int file_id, IndexExtension ext, std::string_view path) const {
    std::vector<std::pair<std::uint64_t, std::string>> out;
    const auto prefix = layout::path_prefix(ext, pk::DATA, fid(file_id), path);
    scan(*impl_->db_, ext, pk::DATA, prefix,
         [&](std::string_view key, std::string_view body) {
             if (key.size() == prefix.size() + 4)
                 out.emplace_back(tail_u32(key, prefix.size()),
                                  std::string(body));
         });
    return out;
}

std::optional<std::string> IndexDatabase::path_file_value(
    int file_id, IndexExtension ext, std::string_view path) const {
    auto key = layout::file_prefix(ext, pk::FILE, fid(file_id));
    key.append(path);
    return get(*impl_->db_, ext, pk::FILE, key);
}

bool IndexDatabase::has_postings(int file_id, std::string_view path) const {
    const auto prefix =
        layout::path_prefix(Ext::POSTINGS, pk::IN_FILE, fid(file_id), path);
    auto it = impl_->db_->new_iterator(cf::POSTINGS);
    it->Seek(::rocksdb::Slice(prefix.data(), prefix.size()));
    const bool found =
        it->Valid() && std::string_view(it->key().data(), it->key().size())
                           .starts_with(prefix);
    if (!it->status().ok())
        throw_db_error("Failed to read postings", it->status());
    return found;
}

bool IndexDatabase::has_posting(int file_id, std::string_view path,
                                std::uint64_t value_hash) const {
    auto key =
        layout::path_prefix(Ext::POSTINGS, pk::IN_FILE, fid(file_id), path);
    layout::append_u64(key, value_hash);
    std::string value;
    auto status = impl_->db_->get(key, &value, cf::POSTINGS);
    if (status.IsNotFound()) return false;
    if (!status.ok()) throw_db_error("Failed to read postings", status);
    return true;
}

std::vector<std::uint64_t> IndexDatabase::posting_granules(
    int file_id, std::string_view path, std::uint64_t value_hash) const {
    std::vector<std::uint64_t> out;
    auto prefix =
        layout::path_prefix(Ext::POSTINGS, pk::IN_GRANULE, fid(file_id), path);
    layout::append_u64(prefix, value_hash);
    scan_keys(*impl_->db_, Ext::POSTINGS, pk::IN_GRANULE, prefix,
              [&](std::string_view key) {
                  if (key.size() == prefix.size() + 4)
                      out.push_back(tail_u32(key, prefix.size()));
              });
    return out;
}

namespace {
std::optional<layout::FileRecord> file_record(const rocks::RocksDatabase& db,
                                              std::string_view path) {
    std::string value;
    auto status = db.get(layout::file_by_path_key(path), &value);
    if (status.IsNotFound()) return std::nullopt;
    if (!status.ok())
        throw_db_error("Failed to look up the file registry", status);
    return layout::decode_file_record(value);
}
}  // namespace

int IndexDatabase::get_file_info_id(std::string_view path) const {
    auto record = file_record(*impl_->db_, path);
    return record ? static_cast<int>(record->file_id) : -1;
}

std::optional<std::uint64_t> IndexDatabase::get_file_hash(
    std::string_view path) const {
    auto record = file_record(*impl_->db_, path);
    if (!record) return std::nullopt;
    return record->hash;
}

std::optional<IndexDatabase::FileStat> IndexDatabase::get_file_stat(
    std::string_view path) const {
    auto record = file_record(*impl_->db_, path);
    if (!record) return std::nullopt;
    return FileStat{record->mtime, record->size};
}

std::uint32_t IndexDatabase::get_format_version() const {
    auto body =
        get(*impl_->db_, Ext::HOST, layout::host::FORMAT, layout::format_key());
    return body ? layout::read_u32(*body) : 0;
}

bool IndexDatabase::format_outdated() const {
    return get_format_version() != FORMAT_VERSION;
}

IndexDatabase::Freshness IndexDatabase::check_freshness(
    const std::string& file_path) const {
    // Stat-only by design: mtime + size are the only O(1) freshness signals,
    // and this runs on every index open and every stale scan, so it must not
    // read the file body. A same-size edit with a deliberately restored mtime
    // is the one change this cannot see; that is accepted rather than pay a
    // whole-file read on the hot path.
    if (format_outdated()) return Freshness::FormatOutdated;

    const auto logical = index::store::internal::get_logical_path(file_path);
    if (get_file_info_id(logical) < 0) return Freshness::Stale;

    auto stored = get_file_stat(logical);
    if (!stored) return Freshness::Stale;  // record predates stat tracking

    const auto current_mtime = static_cast<std::uint64_t>(
        index::store::internal::get_file_modification_time(file_path));
    const auto current_size =
        index::store::internal::file_size_bytes(file_path);
    if (stored->mtime != current_mtime || stored->size != current_size) {
        return Freshness::Stale;
    }
    return Freshness::Fresh;
}

IndexDatabase::StaleCheckResult IndexDatabase::find_stale_files(
    const std::vector<std::string>& current_paths) const {
    StaleCheckResult result;
    result.format_outdated = format_outdated();

    StringViewSet seen_logical;
    seen_logical.reserve(current_paths.size());
    for (const auto& path : current_paths) {
        const auto logical = index::store::internal::get_logical_path(path);
        seen_logical.insert(logical);
        if (get_file_info_id(logical) < 0) {
            result.added.push_back(path);
            continue;
        }
        if (check_freshness(path) != Freshness::Fresh) {
            result.changed.push_back(path);
        }
    }

    for (const auto& [logical, _] : query_all_file_info_ids()) {
        if (seen_logical.find(logical) == seen_logical.end()) {
            result.removed.push_back(logical);
        }
    }
    return result;
}

StringViewMap<int> IndexDatabase::query_all_file_info_ids() const {
    StringViewMap<int> results;
    for (const auto& [path, entry] : query_all_file_registry())
        results.emplace(path, entry.file_id);
    return results;
}

StringViewMap<FileRegistryEntry> IndexDatabase::query_all_file_registry()
    const {
    StringViewMap<FileRegistryEntry> results;
    const auto prefix =
        layout::kind_prefix(Ext::HOST, layout::host::FILE_BY_PATH);
    scan_prefix_iterator(
        "Failed to scan the file registry", prefix,
        [&] { return impl_->db_->new_iterator(cf::DEFAULT); },
        [&](::rocksdb::Iterator& it) {
            const std::string_view key(it.key().data(), it.key().size());
            if (auto record = layout::decode_file_record(
                    std::string_view(it.value().data(), it.value().size())))
                results.emplace(
                    key.substr(prefix.size()),
                    FileRegistryEntry{static_cast<int>(record->file_id)});
        });
    return results;
}

int IndexDatabase::find_file(std::string_view file_path) const {
    return get_file_info_id(get_logical_path(file_path));
}

namespace {

// The schema type of a catalog path from the base types seen for it; nullopt
// when only nulls were seen.
std::optional<ColumnType> schema_type(std::uint8_t seen) {
    auto has = [seen](PathType t) {
        return (seen >> static_cast<unsigned>(t)) & 1U;
    };
    const bool other = has(PathType::UINT) || has(PathType::DOUBLE) ||
                       has(PathType::BOOL) || has(PathType::INT);
    if (has(PathType::MIXED) || (has(PathType::STRING) && other))
        return ColumnType::Json;
    if (has(PathType::STRING)) return ColumnType::String;
    if (has(PathType::UINT) || has(PathType::DOUBLE))
        return ColumnType::Float64;
    if (has(PathType::BOOL) || has(PathType::INT)) return ColumnType::Int64;
    return std::nullopt;
}

}  // namespace

std::vector<std::string> IndexDatabase::query_all_columns() const {
    std::vector<std::string> out;
    for (auto& [name, _] : query_all_column_types())
        out.push_back(std::move(name));
    return out;
}

std::vector<std::pair<std::string, ColumnType>>
IndexDatabase::query_all_column_types() const {
    constexpr std::string_view ARGS = "args.";
    ankerl::unordered_dense::map<std::string, ColumnType> cols;
    scan(*impl_->db_, Ext::CATALOG, layout::catalog_kind::PATH,
         layout::kind_prefix(Ext::CATALOG, layout::catalog_kind::PATH),
         [&](std::string_view key, std::string_view body) {
             if (key.size() <= layout::KEY_PREFIX_BYTES) return;
             auto stat = layout::decode_path_stat(body);
             if (!stat) return;
             auto type = schema_type(stat->seen);
             if (!type) return;
             auto path = key.substr(layout::KEY_PREFIX_BYTES);
             if (path.starts_with(ARGS)) path.remove_prefix(ARGS.size());
             auto [pos, inserted] = cols.emplace(std::string(path), *type);
             if (!inserted) pos->second = merge_column_type(pos->second, *type);
         });
    std::vector<std::pair<std::string, ColumnType>> out(cols.begin(),
                                                        cols.end());
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

std::vector<std::pair<std::string, ColumnType>>
IndexDatabase::file_column_types(int file_id) const {
    std::vector<std::pair<std::string, ColumnType>> out;
    for (auto& [path, stat] : catalog(file_id))
        if (auto type = schema_type(stat.seen))
            out.emplace_back(std::move(path), *type);
    return out;
}

std::optional<std::string> IndexDatabase::file_schema(int file_id) const {
    if (!extension_current(file_id, Ext::PROFILE)) return std::nullopt;
    auto body = get(*impl_->db_, Ext::PROFILE, layout::profile_kind::ID,
                    layout::file_prefix(Ext::PROFILE, layout::profile_kind::ID,
                                        fid(file_id)));
    if (!body) return std::nullopt;
    return std::string(*body);
}

std::vector<std::pair<std::string, PathStat>> IndexDatabase::catalog(
    int file_id) const {
    std::vector<std::pair<std::string, PathStat>> out;
    const auto prefix = layout::file_prefix(
        Ext::CATALOG, layout::catalog_kind::PATH, fid(file_id));
    scan(*impl_->db_, Ext::CATALOG, layout::catalog_kind::PATH, prefix,
         [&](std::string_view key, std::string_view body) {
             if (auto stat = layout::decode_path_stat(body))
                 out.emplace_back(std::string(key.substr(prefix.size())),
                                  *stat);
         });
    return out;
}

std::vector<index::schemas::dft::ChunkStatisticsResult>
IndexDatabase::query_chunk_statistics(int file_id) const {
    std::vector<index::schemas::dft::ChunkStatisticsResult> results;
    scan(*impl_->db_, Ext::STATS, sk::CHUNK_STATS,
         layout::file_prefix(Ext::STATS, sk::CHUNK_STATS, fid(file_id)),
         [&](std::string_view key, std::string_view body) {
             results.push_back({tail_u32(key, layout::KEY_PREFIX_BYTES),
                                decode_chunk_statistics_value(body)});
         });
    return results;
}

std::optional<ankerl::unordered_dense::map<std::uint64_t, ChunkMetadata>>
IndexDatabase::chunk_metadata(int file_id) const {
    if (!extension_current(file_id, Ext::METADATA)) return std::nullopt;
    ankerl::unordered_dense::map<std::uint64_t, ChunkMetadata> out;
    scan(*impl_->db_, Ext::METADATA, layout::metadata_kind::CHUNK,
         layout::file_prefix(Ext::METADATA, layout::metadata_kind::CHUNK,
                             fid(file_id)),
         [&](std::string_view key, std::string_view body) {
             if (auto m = layout::decode_chunk_metadata(body))
                 out.emplace(tail_u32(key, layout::KEY_PREFIX_BYTES),
                             std::move(*m));
         });
    return out;
}

ankerl::unordered_dense::map<
    int, std::vector<index::schemas::dft::ChunkStatisticsResult>>
IndexDatabase::query_chunk_statistics_batch(
    const std::vector<int>& file_ids) const {
    ankerl::unordered_dense::map<
        int, std::vector<index::schemas::dft::ChunkStatisticsResult>>
        results;
    scan_files(
        *impl_->db_, Ext::STATS, sk::CHUNK_STATS, file_ids,
        [&](int file_id, std::string_view key, std::string_view body) {
            results[file_id].push_back({tail_u32(key, layout::KEY_PREFIX_BYTES),
                                        decode_chunk_statistics_value(body)});
        });
    return results;
}

ankerl::unordered_dense::map<int, index::schemas::dft::MergedStatisticsResult>
IndexDatabase::query_file_scalar_stats_batch(
    const std::vector<int>& file_ids) const {
    ankerl::unordered_dense::map<int,
                                 index::schemas::dft::MergedStatisticsResult>
        results;
    scan_files(*impl_->db_, Ext::STATS, sk::FILE_SCALAR_STATS, file_ids,
               [&](int file_id, std::string_view, std::string_view body) {
                   results.emplace(file_id,
                                   decode_file_scalar_stats_value(body));
               });
    return results;
}

ankerl::unordered_dense::map<int, FileMetadataResult>
IndexDatabase::query_file_metadata_batch(
    const std::vector<int>& file_ids) const {
    ankerl::unordered_dense::map<int, FileMetadataResult> results;
    scan_files(*impl_->db_, Ext::MEMBERS, layout::members::METADATA, file_ids,
               [&](int file_id, std::string_view, std::string_view body) {
                   Cursor cursor(body);
                   auto& meta = results[file_id];
                   meta.checkpoint_size = cursor.u64();
                   meta.num_lines = cursor.u64();
                   meta.max_bytes = cursor.u64();
                   meta.truncated =
                       (cursor.u64() & encoding::METADATA_FLAG_TRUNCATED) != 0;
               });
    return results;
}

ankerl::unordered_dense::map<int, StringViewMap<std::uint64_t>>
IndexDatabase::query_file_pid_tid_counts_batch(
    const std::vector<int>& file_ids) const {
    ankerl::unordered_dense::map<int, StringViewMap<std::uint64_t>> results;
    scan_files(*impl_->db_, Ext::STATS, sk::FILE_PID_TID_COUNTS, file_ids,
               [&](int file_id, std::string_view, std::string_view body) {
                   results.emplace(file_id, decode_count_map_value(body));
               });
    return results;
}

std::vector<index::gzip::GzipMemberRecord> IndexDatabase::query_gzip_members(
    int file_id) const {
    std::vector<index::gzip::GzipMemberRecord> members;
    if (!extension_current(file_id, Ext::MEMBERS)) return members;
    scan(*impl_->db_, Ext::MEMBERS, layout::members::MEMBER,
         layout::file_prefix(Ext::MEMBERS, layout::members::MEMBER,
                             fid(file_id)),
         [&](std::string_view key, std::string_view body) {
             members.push_back(decode_gzip_member(key, body));
         });
    return members;
}

std::optional<std::string> IndexDatabase::query_restart_window(
    int file_id, std::uint64_t member_idx) const {
    return get(*impl_->db_, Ext::MEMBERS, layout::members::RESTART,
               layout::granule_key(Ext::MEMBERS, layout::members::RESTART,
                                   fid(file_id),
                                   static_cast<std::uint32_t>(member_idx)));
}

std::vector<index::gzip::ChunkSpan> IndexDatabase::query_chunk_spans(
    int file_id) const {
    auto members = query_gzip_members(file_id);
    std::vector<index::gzip::ChunkSpan> spans;
    spans.reserve(members.size());
    for (const auto& m : members)
        spans.push_back(
            {m.uc_offset, m.uc_size, m.first_line_num, m.last_line_num});
    return spans;
}

namespace {
// The distinct PIDs a file touched, projected from its "pid:tid" -> count map
// (FILE_PID_TID_COUNTS, written by the bloom/stats pass). This is the same set
// the retired manifest P| keys used to store, now derived so PID collection
// needs no separate index tier.
ankerl::unordered_dense::set<std::uint64_t> pids_from_pid_tid_counts(
    const StringViewMap<std::uint64_t>& counts) {
    ankerl::unordered_dense::set<std::uint64_t> pids;
    for (const auto& [key, _] : counts) {
        std::string_view sv(key);
        const auto colon = sv.find(':');
        const std::string_view pid_sv =
            colon == std::string_view::npos ? sv : sv.substr(0, colon);
        std::uint64_t pid = 0;
        std::from_chars(pid_sv.data(), pid_sv.data() + pid_sv.size(), pid);
        pids.insert(pid);
    }
    return pids;
}
}  // namespace

ankerl::unordered_dense::set<std::uint64_t> IndexDatabase::query_file_pids(
    int file_id) const {
    auto body = get(
        *impl_->db_, Ext::STATS, sk::FILE_PID_TID_COUNTS,
        layout::file_prefix(Ext::STATS, sk::FILE_PID_TID_COUNTS, fid(file_id)));
    if (!body) return {};
    return pids_from_pid_tid_counts(decode_count_map_value(*body));
}

ankerl::unordered_dense::map<int, ankerl::unordered_dense::set<std::uint64_t>>
IndexDatabase::query_all_file_pids() const {
    ankerl::unordered_dense::map<int,
                                 ankerl::unordered_dense::set<std::uint64_t>>
        result;
    scan(*impl_->db_, Ext::STATS, sk::FILE_PID_TID_COUNTS,
         layout::kind_prefix(Ext::STATS, sk::FILE_PID_TID_COUNTS),
         [&](std::string_view key, std::string_view body) {
             result[static_cast<int>(tail_u32(key, 3))] =
                 pids_from_pid_tid_counts(decode_count_map_value(body));
         });
    return result;
}

namespace {

// Metadata record field `idx` for `file_id`, 0 when the record is absent.
std::uint64_t get_metadata_field(const rocks::RocksDatabase& db, int file_id,
                                 std::size_t idx) {
    auto body = get(db, Ext::MEMBERS, layout::members::METADATA,
                    layout::file_prefix(Ext::MEMBERS, layout::members::METADATA,
                                        fid(file_id)));
    if (!body || body->size() < (idx + 1) * 8) return 0;
    return layout::read_u64(std::string_view(*body).substr(idx * 8));
}

}  // namespace

std::uint64_t IndexDatabase::get_checkpoint_size(int file_id) const {
    return get_metadata_field(*impl_->db_, file_id, 0);
}

std::uint64_t IndexDatabase::get_num_lines(int file_id) const {
    return get_metadata_field(*impl_->db_, file_id, 1);
}

std::uint64_t IndexDatabase::get_max_bytes(int file_id) const {
    return get_metadata_field(*impl_->db_, file_id, 2);
}

std::optional<std::string> IndexDatabase::rowset(int file_id,
                                                 std::string_view name) const {
    if (!extension_current(file_id, Ext::ROWSET)) return std::nullopt;
    auto body = get(*impl_->db_, Ext::ROWSET, layout::rowset_kind::FRAME,
                    layout::path_prefix(Ext::ROWSET, layout::rowset_kind::FRAME,
                                        fid(file_id), name));
    if (!body) return std::nullopt;
    return std::string(*body);
}

}  // namespace dftracer::utils::index::store
