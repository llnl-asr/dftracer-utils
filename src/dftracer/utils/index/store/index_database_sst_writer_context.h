#ifndef DFTRACER_UTILS_INDEX_STORE_INDEX_DATABASE_SST_WRITER_CONTEXT_H
#define DFTRACER_UTILS_INDEX_STORE_INDEX_DATABASE_SST_WRITER_CONTEXT_H

#include <dftracer/utils/index/store/index_write.h>

#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store {

/// An IndexWrite that buffers per family and emits one sorted SST per
/// non-empty family on `commit()`, including its range deletes. A coordinator
/// ingests the artifacts of many writers in one call through
/// `IndexDatabase::bulk_ingest()`, which is the commit point.
///
/// Holds no RocksDB handle. Concurrent writers need disjoint file ids.
class IndexDatabaseSstWriterContext : public IndexWrite {
   public:
    /// One optional SST path per layout::Family.
    struct Artifacts {
        std::array<std::optional<std::string>, layout::FAMILY_COUNT> sst;

        bool empty() const noexcept;

        /// Moves every SST to `dest_dir` (created if missing): rename on one
        /// filesystem, copy and unlink across filesystems.
        Artifacts move_to(std::string_view dest_dir) &&;
    };

    /// Builds SSTs under `staging_dir/batch_id`; `batch_id` must be unique
    /// among writers sharing `staging_dir`.
    IndexDatabaseSstWriterContext(std::string staging_dir,
                                  std::string batch_id);

    IndexDatabaseSstWriterContext(const IndexDatabaseSstWriterContext&) =
        delete;
    IndexDatabaseSstWriterContext& operator=(
        const IndexDatabaseSstWriterContext&) = delete;
    IndexDatabaseSstWriterContext(IndexDatabaseSstWriterContext&&) noexcept;
    IndexDatabaseSstWriterContext& operator=(
        IndexDatabaseSstWriterContext&&) noexcept;
    ~IndexDatabaseSstWriterContext() override;

    void put(layout::Family family, std::string_view key,
             std::string_view value) override;
    void merge(layout::Family family, std::string_view key,
               std::string_view operand) override;
    void delete_range(layout::Family family, std::string_view begin,
                      std::string_view end) override;

    /// Writes the SSTs and returns their paths; a second call returns none.
    Artifacts commit();

    struct Entry {
        std::string key;
        std::string value;
        bool is_merge = false;
    };

   private:
    struct Buffer {
        std::vector<Entry> entries;
        std::vector<std::pair<std::string, std::string>> ranges;
    };

    std::string staging_dir_;
    std::string batch_id_;
    bool committed_ = false;
    std::array<Buffer, layout::FAMILY_COUNT> buffers_;
};

/// Thread-safe collector of the artifacts of many SST writers.
class SstArtifactRegistry {
   public:
    void append(IndexDatabaseSstWriterContext::Artifacts artifacts) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t f = 0; f < layout::FAMILY_COUNT; ++f)
            if (artifacts.sst[f])
                files_[f].push_back(std::move(*artifacts.sst[f]));
    }

    const std::vector<std::string>& files(layout::Family family) const {
        return files_[static_cast<std::size_t>(family)];
    }

   private:
    std::mutex mutex_;
    std::array<std::vector<std::string>, layout::FAMILY_COUNT> files_;
};

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_INDEX_DATABASE_SST_WRITER_CONTEXT_H
