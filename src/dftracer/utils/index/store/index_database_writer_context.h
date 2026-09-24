#ifndef DFTRACER_UTILS_INDEX_STORE_INDEX_DATABASE_WRITER_CONTEXT_H
#define DFTRACER_UTILS_INDEX_STORE_INDEX_DATABASE_WRITER_CONTEXT_H

#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/index_write.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace dftracer::utils::index::store {

class IndexDatabase;

/// An IndexWrite over one RocksDB WriteBatch, applied by commit().
class IndexDatabaseWriterContext : public IndexWrite {
   public:
    IndexDatabaseWriterContext(IndexDatabaseWriterContext&&) noexcept;
    IndexDatabaseWriterContext& operator=(
        IndexDatabaseWriterContext&&) noexcept;
    IndexDatabaseWriterContext(const IndexDatabaseWriterContext&) = delete;
    IndexDatabaseWriterContext& operator=(const IndexDatabaseWriterContext&) =
        delete;
    ~IndexDatabaseWriterContext() override;

    void put(layout::Family family, std::string_view key,
             std::string_view value) override;
    void merge(layout::Family family, std::string_view key,
               std::string_view operand) override;
    void delete_range(layout::Family family, std::string_view begin,
                      std::string_view end) override;

    void commit();

    /// Writes the format version and the extension registry when absent.
    void init_schema();

    /// The file id of `logical_path`: its registered id, or the next free id
    /// (reserved in this write) when the path is new.
    int file_id_for(std::string_view logical_path);

   private:
    friend class IndexDatabase;
    explicit IndexDatabaseWriterContext(
        std::shared_ptr<index::store::RocksDatabase> db);

    std::shared_ptr<index::store::RocksDatabase> db_;
    index::store::RocksDatabase::Batch batch_;
    bool committed_ = false;
    std::int64_t cached_next_file_id_ = -1;
};

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_INDEX_DATABASE_WRITER_CONTEXT_H
