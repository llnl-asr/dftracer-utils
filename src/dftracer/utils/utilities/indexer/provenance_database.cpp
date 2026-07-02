#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <dftracer/utils/utilities/indexer/error.h>
#include <dftracer/utils/utilities/indexer/internal/db_error.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/iterator_codec.h>
#include <dftracer/utils/utilities/indexer/internal/payload_codec.h>
#include <dftracer/utils/utilities/indexer/internal/scan_prefix.h>
#include <dftracer/utils/utilities/indexer/provenance_database.h>

#include <utility>

namespace dftracer::utils::utilities::indexer {

namespace rocks = dftracer::utils::rocksdb;
namespace cf = rocks::cf;

using namespace internal;

namespace {

std::string file_key(std::string_view path) {
    return std::string("pf|") + std::string(path);
}

std::string file_reverse_key(int file_info_id) {
    std::string key("pr|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_info_id));
    return key;
}

std::string next_file_id_key() { return "_next_prov_file_id"; }

std::string encode_file_record(int file_info_id, std::uint64_t file_hash) {
    std::string value;
    rocks::KeyCodec::append_be32(value,
                                 static_cast<std::uint32_t>(file_info_id));
    rocks::KeyCodec::append_be64(value, file_hash);
    return value;
}

int decode_file_id(std::string_view value) {
    if (value.size() < 4) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt provenance file record");
    }
    return static_cast<int>(rocks::KeyCodec::decode_be32(value.substr(0, 4)));
}

std::uint64_t decode_hash(std::string_view value) {
    if (value.size() < 12) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt provenance file record");
    }
    return rocks::KeyCodec::decode_be64(value.substr(4, 8));
}

std::string source_key(int file_info_id, int source_idx) {
    std::string key("ps|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_info_id));
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(source_idx));
    return key;
}

std::string info_key(int file_info_id, std::string_view key_suffix) {
    std::string key("pi|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_info_id));
    key.append(key_suffix);
    return key;
}

std::string group_prefix(int file_info_id) {
    std::string key("pg|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_info_id));
    return key;
}

std::string group_key(int file_info_id, std::string_view name) {
    auto key = group_prefix(file_info_id);
    key.append(name);
    return key;
}

std::string segment_key(int file_info_id, int source_idx, int source_checkpoint,
                        int segment_seq) {
    std::string key("px|");
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(file_info_id));
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(source_idx));
    rocks::KeyCodec::append_be32(key,
                                 static_cast<std::uint32_t>(source_checkpoint));
    rocks::KeyCodec::append_be32(key, static_cast<std::uint32_t>(segment_seq));
    return key;
}

template <typename Fn>
void scan_prefix(const rocks::RocksDatabase& db, std::string_view prefix,
                 Fn&& fn) {
    internal::scan_prefix_iterator(
        "Failed to scan provenance prefix", prefix,
        [&] { return db.new_iterator(cf::PROVENANCE); }, std::forward<Fn>(fn));
}

}  // namespace

ProvenanceDatabase::ProvenanceDatabase(const std::string& provenance_path,
                                       rocks::RocksDatabase::OpenMode open_mode)
    : db_path_(internal::normalize_index_root(provenance_path)),
      open_mode_(open_mode),
      db_(rocks::RocksDBManager::instance().get_or_open(db_path_, open_mode_)) {
    if (open_mode_ == rocks::RocksDatabase::OpenMode::ReadWrite) {
        init_schema();
    }
}

void ProvenanceDatabase::init_schema() {}

int ProvenanceDatabase::get_or_create_file_info(const std::string& path,
                                                std::uint64_t file_hash) {
    const auto key = file_key(path);
    std::string value;
    auto status = db_->get(key, &value, cf::PROVENANCE);
    if (status.ok()) {
        const auto id = decode_file_id(value);
        if (decode_hash(value) == file_hash) {
            return id;
        }
        const auto encoded = encode_file_record(id, file_hash);
        status = txn_batch_
                     ? db_->put(*txn_batch_, cf::PROVENANCE, key, encoded)
                     : db_->put(key, encoded, cf::PROVENANCE);
        if (!status.ok()) {
            throw_db_error("Failed to update provenance file info", status);
        }
        status = txn_batch_
                     ? db_->put(*txn_batch_, cf::PROVENANCE,
                                file_reverse_key(id), path)
                     : db_->put(file_reverse_key(id), path, cf::PROVENANCE);
        if (!status.ok()) {
            throw_db_error("Failed to update provenance reverse file info",
                           status);
        }
        return id;
    }
    if (!status.IsNotFound()) {
        throw_db_error("Failed to query provenance file info", status);
    }

    std::uint32_t next_id = 1;
    std::string next_value;
    status = db_->get(next_file_id_key(), &next_value, cf::PROVENANCE);
    if (status.ok()) {
        next_id = rocks::KeyCodec::decode_be32(next_value);
    } else if (!status.IsNotFound()) {
        throw_db_error("Failed to read next provenance file id", status);
    }

    const auto encoded =
        encode_file_record(static_cast<int>(next_id), file_hash);
    const auto next_encoded = rocks::KeyCodec::encode_be32(next_id + 1);
    if (txn_batch_) {
        status = db_->put(*txn_batch_, cf::PROVENANCE, key, encoded);
        if (!status.ok()) throw_db_error("Failed to insert file info", status);
        status = db_->put(*txn_batch_, cf::PROVENANCE,
                          file_reverse_key(next_id), path);
        if (!status.ok()) {
            throw_db_error("Failed to insert reverse file info", status);
        }
        status = db_->put(*txn_batch_, cf::PROVENANCE, next_file_id_key(),
                          next_encoded);
        if (!status.ok()) {
            throw_db_error("Failed to update next provenance file id", status);
        }
    } else {
        status = db_->put(key, encoded, cf::PROVENANCE);
        if (!status.ok()) throw_db_error("Failed to insert file info", status);
        status = db_->put(file_reverse_key(next_id), path, cf::PROVENANCE);
        if (!status.ok()) {
            throw_db_error("Failed to insert reverse file info", status);
        }
        status = db_->put(next_file_id_key(), next_encoded, cf::PROVENANCE);
        if (!status.ok()) {
            throw_db_error("Failed to update next provenance file id", status);
        }
    }
    return static_cast<int>(next_id);
}

int ProvenanceDatabase::get_file_info_id(const std::string& path) const {
    std::string value;
    auto status = db_->get(file_key(path), &value, cf::PROVENANCE);
    if (status.IsNotFound()) {
        return -1;
    }
    if (!status.ok()) {
        throw_db_error("Failed to read provenance file info id", status);
    }
    return decode_file_id(value);
}

void ProvenanceDatabase::begin_transaction() {
    txn_batch_ =
        std::make_unique<rocks::RocksDatabase::Batch>(db_->begin_batch());
}

void ProvenanceDatabase::commit_transaction() {
    if (!txn_batch_) {
        return;
    }
    auto status = db_->commit_batch(*txn_batch_);
    txn_batch_.reset();
    if (!status.ok()) {
        throw_db_error("Failed to commit provenance RocksDB batch", status);
    }
}

void ProvenanceDatabase::rollback_transaction() noexcept { txn_batch_.reset(); }

std::string determine_provenance_index_path(const std::string& data_path,
                                            const std::string& index_dir) {
    fs::path path = index_dir.empty() ? fs::path(data_path).parent_path()
                                      : fs::path(index_dir);
    return internal::normalize_index_root((path / ".dftindex").string());
}

void ProvenanceDatabase::insert_info(int file_info_id, std::string_view key,
                                     std::string_view value) {
    const auto db_key = info_key(file_info_id, key);
    auto status = txn_batch_
                      ? db_->put(*txn_batch_, cf::PROVENANCE, db_key, value)
                      : db_->put(db_key, value, cf::PROVENANCE);
    if (!status.ok()) {
        throw_db_error("Failed to insert provenance info", status);
    }
}

void ProvenanceDatabase::insert_source(int file_info_id, int source_idx,
                                       std::string_view path,
                                       int num_checkpoints,
                                       std::string_view event_hash) {
    std::string value;
    append_string(value, path);
    append_u32(value, static_cast<std::uint32_t>(num_checkpoints));
    append_string(value, event_hash);
    auto status = txn_batch_
                      ? db_->put(*txn_batch_, cf::PROVENANCE,
                                 source_key(file_info_id, source_idx), value)
                      : db_->put(source_key(file_info_id, source_idx), value,
                                 cf::PROVENANCE);
    if (!status.ok()) {
        throw_db_error("Failed to insert provenance source", status);
    }
}

void ProvenanceDatabase::insert_group(int file_info_id, std::string_view name,
                                      std::string_view predicate) {
    const auto db_key = group_key(file_info_id, name);
    auto status =
        txn_batch_ ? db_->put(*txn_batch_, cf::PROVENANCE, db_key,
                              std::string(predicate))
                   : db_->put(db_key, std::string(predicate), cf::PROVENANCE);
    if (!status.ok()) {
        throw_db_error("Failed to insert provenance group", status);
    }
}

void ProvenanceDatabase::insert_segment(int file_info_id, int source_idx,
                                        int source_checkpoint, int segment_seq,
                                        int output_line_start,
                                        int output_line_end, int event_count) {
    std::string value;
    append_u32(value, static_cast<std::uint32_t>(output_line_start));
    append_u32(value, static_cast<std::uint32_t>(output_line_end));
    append_u32(value, static_cast<std::uint32_t>(event_count));
    auto key =
        segment_key(file_info_id, source_idx, source_checkpoint, segment_seq);
    auto status = txn_batch_ ? db_->put(*txn_batch_, cf::PROVENANCE, key, value)
                             : db_->put(key, value, cf::PROVENANCE);
    if (!status.ok()) {
        throw_db_error("Failed to insert provenance segment", status);
    }
}

std::vector<ProvenanceDatabase::ProvenanceSource>
ProvenanceDatabase::query_sources(int file_info_id) const {
    std::vector<ProvenanceSource> results;
    std::string prefix("ps|");
    rocks::KeyCodec::append_be32(prefix,
                                 static_cast<std::uint32_t>(file_info_id));
    scan_prefix(*db_, prefix, [&](::rocksdb::Iterator& it) {
        const auto key = iterator_key(it);
        const auto value = iterator_value(it);
        ProvenanceSource source;
        source.source_idx = static_cast<int>(
            rocks::KeyCodec::decode_be32(std::string_view(key).substr(7, 4)));
        Cursor cursor(value);
        source.path = cursor.str();
        source.num_checkpoints = static_cast<int>(cursor.u32());
        source.event_hash = cursor.str();
        results.push_back(std::move(source));
    });
    return results;
}

std::vector<ProvenanceDatabase::ProvenanceSegment>
ProvenanceDatabase::query_segments(int file_info_id, int source_idx) const {
    std::vector<ProvenanceSegment> results;
    std::string prefix("px|");
    rocks::KeyCodec::append_be32(prefix,
                                 static_cast<std::uint32_t>(file_info_id));
    rocks::KeyCodec::append_be32(prefix,
                                 static_cast<std::uint32_t>(source_idx));
    scan_prefix(*db_, prefix, [&](::rocksdb::Iterator& it) {
        const auto key = iterator_key(it);
        const auto value = iterator_value(it);
        Cursor cursor(value);
        ProvenanceSegment segment;
        segment.source_idx = source_idx;
        segment.source_checkpoint = static_cast<int>(
            rocks::KeyCodec::decode_be32(std::string_view(key).substr(11, 4)));
        segment.output_line_start = static_cast<int>(cursor.u32());
        segment.output_line_end = static_cast<int>(cursor.u32());
        segment.event_count = static_cast<int>(cursor.u32());
        results.push_back(std::move(segment));
    });
    return results;
}

std::vector<ProvenanceDatabase::ProvenanceSegment>
ProvenanceDatabase::query_all_segments(int file_info_id) const {
    std::vector<ProvenanceSegment> results;
    std::string prefix("px|");
    rocks::KeyCodec::append_be32(prefix,
                                 static_cast<std::uint32_t>(file_info_id));
    scan_prefix(*db_, prefix, [&](::rocksdb::Iterator& it) {
        const auto key = iterator_key(it);
        const auto value = iterator_value(it);
        Cursor cursor(value);
        ProvenanceSegment segment;
        segment.source_idx = static_cast<int>(
            rocks::KeyCodec::decode_be32(std::string_view(key).substr(7, 4)));
        segment.source_checkpoint = static_cast<int>(
            rocks::KeyCodec::decode_be32(std::string_view(key).substr(11, 4)));
        segment.output_line_start = static_cast<int>(cursor.u32());
        segment.output_line_end = static_cast<int>(cursor.u32());
        segment.event_count = static_cast<int>(cursor.u32());
        results.push_back(std::move(segment));
    });
    return results;
}

std::string ProvenanceDatabase::query_info(int file_info_id,
                                           std::string_view key) const {
    std::string value;
    auto status = db_->get(info_key(file_info_id, key), &value, cf::PROVENANCE);
    if (status.IsNotFound()) {
        return {};
    }
    if (!status.ok()) {
        throw_db_error("Failed to query provenance info", status);
    }
    return value;
}

std::string ProvenanceDatabase::query_group_name(int file_info_id) const {
    std::string result;
    const auto prefix = group_prefix(file_info_id);
    scan_prefix(*db_, prefix, [&](::rocksdb::Iterator& it) {
        if (result.empty()) {
            const auto key = iterator_key(it);
            result = key.substr(prefix.size());
        }
    });
    return result;
}

std::string ProvenanceDatabase::query_group_predicate(int file_info_id) const {
    std::string result;
    scan_prefix(*db_, group_prefix(file_info_id), [&](::rocksdb::Iterator& it) {
        if (result.empty()) {
            result = iterator_value(it);
        }
    });
    return result;
}

}  // namespace dftracer::utils::utilities::indexer
