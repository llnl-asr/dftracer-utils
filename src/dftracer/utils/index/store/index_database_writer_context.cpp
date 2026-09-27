#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/internal/db_error.h>

#include <utility>

namespace dftracer::utils::index::store {

IndexDatabaseWriterContext::IndexDatabaseWriterContext(
    std::shared_ptr<index::store::RocksDatabase> db)
    : db_(std::move(db)), batch_(db_->begin_batch()) {}

IndexDatabaseWriterContext::IndexDatabaseWriterContext(
    IndexDatabaseWriterContext&&) noexcept = default;

IndexDatabaseWriterContext& IndexDatabaseWriterContext::operator=(
    IndexDatabaseWriterContext&&) noexcept = default;

IndexDatabaseWriterContext::~IndexDatabaseWriterContext() = default;

void IndexDatabaseWriterContext::put(layout::Family family,
                                     std::string_view key,
                                     std::string_view value) {
    auto status = db_->put(batch_, layout::family_name(family), key, value);
    if (!status.ok()) internal::throw_db_error("Failed to stage put", status);
}

void IndexDatabaseWriterContext::merge(layout::Family family,
                                       std::string_view key,
                                       std::string_view operand) {
    auto status = db_->merge(batch_, layout::family_name(family), key, operand);
    if (!status.ok()) internal::throw_db_error("Failed to stage merge", status);
}

void IndexDatabaseWriterContext::delete_range(layout::Family family,
                                              std::string_view begin,
                                              std::string_view end) {
    auto status =
        db_->delete_range(batch_, layout::family_name(family), begin, end);
    if (!status.ok())
        internal::throw_db_error("Failed to stage range delete", status);
}

void IndexDatabaseWriterContext::commit() {
    if (committed_) return;
    auto status = db_->commit_batch(batch_);
    committed_ = true;
    if (!status.ok()) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to commit WriteBatch: " + status.ToString());
    }
}

void IndexDatabaseWriterContext::init_schema() {
    std::string value;
    auto status = db_->get(layout::format_key(), &value);
    if (status.IsNotFound()) {
        // A database holding data without a format key has another layout:
        // stamping it would make that data look current, so it stays
        // outdated and is rebuilt.
        auto it = db_->new_iterator();
        it->SeekToFirst();
        if (it->Valid()) return;
        std::string body;
        layout::append_u32(body, IndexDatabase::FORMAT_VERSION);
        put(layout::Family::REGISTRY, layout::format_key(),
            layout::with_header(layout::Ext::HOST, layout::host::FORMAT, body));
        for (auto ext :
             {layout::Ext::MEMBERS, layout::Ext::ROWSET, layout::Ext::ZONEMAP,
              layout::Ext::BLOOM, layout::Ext::COUNTS, layout::Ext::POSTINGS,
              layout::Ext::STATS}) {
            std::string id;
            layout::append_u16(id, static_cast<std::uint16_t>(ext));
            put(layout::Family::REGISTRY, layout::ext_registry_key(ext),
                layout::with_header(layout::Ext::HOST, layout::host::EXT, id));
        }
    } else if (!status.ok()) {
        internal::throw_db_error("Failed to read format version", status);
    }
}

int IndexDatabaseWriterContext::file_id_for(std::string_view logical_path) {
    std::string existing;
    auto status = db_->get(layout::file_by_path_key(logical_path), &existing);
    if (status.ok()) {
        if (auto record = layout::decode_file_record(existing))
            return static_cast<int>(record->file_id);
    } else if (!status.IsNotFound()) {
        internal::throw_db_error("Failed to query file registry", status);
    }

    if (cached_next_file_id_ < 0) {
        cached_next_file_id_ = 1;
        std::string next;
        status = db_->get(layout::next_file_id_key(), &next);
        if (status.ok()) {
            if (auto body = layout::payload(next, layout::Ext::HOST,
                                            layout::host::NEXT_FILE_ID))
                cached_next_file_id_ = layout::read_u32(*body);
        } else if (!status.IsNotFound()) {
            internal::throw_db_error("Failed to read next file id", status);
        }
    }
    const auto file_id = static_cast<std::uint32_t>(cached_next_file_id_++);
    std::string body;
    layout::append_u32(body, file_id + 1);
    put(layout::Family::REGISTRY, layout::next_file_id_key(),
        layout::with_header(layout::Ext::HOST, layout::host::NEXT_FILE_ID,
                            body));
    return static_cast<int>(file_id);
}

}  // namespace dftracer::utils::index::store
