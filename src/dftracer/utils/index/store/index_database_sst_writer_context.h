#ifndef DFTRACER_UTILS_INDEX_STORE_INDEX_DATABASE_SST_WRITER_CONTEXT_H
#define DFTRACER_UTILS_INDEX_STORE_INDEX_DATABASE_SST_WRITER_CONTEXT_H

#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/index_write.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store {

/// An IndexWrite that buffers per family and emits sorted SSTs on `commit()`,
/// one per key prefix ([ext][kind]) of each non-empty family, including its
/// range deletes. A prefix keeps one writer's SST out of the key range of
/// another writer's whose file ids are disjoint. A coordinator
/// ingests the artifacts of many writers in one call through
/// `IndexDatabase::bulk_ingest()`, which is the commit point.
///
/// Holds no RocksDB handle. Concurrent writers need disjoint file ids.
/// `buffered_bytes()` is the memory the staged entries hold, their reserved
/// capacity included.
class IndexDatabaseSstWriterContext : public IndexWrite {
   public:
    /// The SST paths of each layout::Family.
    struct Artifacts {
        std::array<std::vector<std::string>, layout::FAMILY_COUNT> sst;

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
    void del(layout::Family family, std::string_view key) override;

    /// Writes the SSTs and returns their paths; a second call returns none.
    Artifacts commit();

    enum class Op : std::uint8_t { Put, Merge, Delete, Range };

    /// One family's staged bytes: the key and value of an entry, or the two
    /// bounds of a range, lie one after the other in `bytes`.
    struct Entry {
        std::uint64_t offset = 0;
        std::uint32_t key_size = 0;
        std::uint32_t value_size = 0;
        Op op = Op::Put;
    };

    struct Range {
        std::uint64_t offset = 0;
        std::uint32_t begin_size = 0;
        std::uint32_t end_size = 0;
    };

    struct Buffer {
        dstd::Vector<char> bytes;
        dstd::Vector<Entry> entries;
        dstd::Vector<Range> ranges;

        std::size_t footprint() const noexcept {
            return bytes.capacity() + entries.capacity() * sizeof(Entry) +
                   ranges.capacity() * sizeof(Range);
        }
    };

   private:
    void stage(layout::Family family, std::string_view first,
               std::string_view second, Op op);

    std::size_t held_ = 0;
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
            for (auto& path : artifacts.sst[f])
                files_[f].push_back(std::move(path));
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
