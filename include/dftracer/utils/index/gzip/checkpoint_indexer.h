#ifndef DFTRACER_UTILS_INDEX_GZIP_CHECKPOINT_INDEXER_H
#define DFTRACER_UTILS_INDEX_GZIP_CHECKPOINT_INDEXER_H

#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/index/build/index_visitor.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dftracer::utils::index::gzip {

/**
 * Abstract base interface for all indexer implementations.
 * This provides a common API for GZIP, TAR.GZ, and other archive indexers.
 */
class CheckpointIndexer {
   public:
    static constexpr std::uint64_t DEFAULT_CHECKPOINT_SIZE =
        constants::indexer::DEFAULT_CHECKPOINT_SIZE;

    virtual ~CheckpointIndexer() = default;

    // Core indexer operations
    virtual coro::CoroTask<void> build_async() const = 0;

    void build() const { build_async().get(); }
    virtual bool need_rebuild() const = 0;
    virtual bool exists() const = 0;

    using VisitorList =
        std::vector<std::reference_wrapper<index::build::IndexVisitor>>;

    virtual void set_visitors(VisitorList) {}

    // Metadata accessors
    virtual const std::string &get_index_path() const = 0;
    virtual const std::string &get_archive_path() const = 0;
    virtual std::uint64_t get_checkpoint_size() const = 0;
    virtual std::uint64_t get_max_bytes() const = 0;
    virtual std::uint64_t get_num_lines() const = 0;

    /// Member record (a member or a piece of one) containing `target_offset`
    /// (uncompressed). A RESTART piece also needs its `restart_window`.
    virtual bool find_member(std::size_t target_offset,
                             GzipMemberRecord &member) const = 0;

    /// Every member record in file order. Empty when the file was never
    /// indexed.
    virtual std::vector<GzipMemberRecord> get_members() const = 0;

    /// The 32 KiB inflate window of RESTART piece `member_idx`; empty when
    /// the index has none for it.
    virtual std::string restart_window(std::uint64_t member_idx) const = 0;

    // Archive format identification
    virtual ArchiveFormat get_format_type() const = 0;
    virtual const char *get_format_name() const = 0;

   protected:
    CheckpointIndexer() = default;
    CheckpointIndexer(const CheckpointIndexer &) = delete;
    CheckpointIndexer &operator=(const CheckpointIndexer &) = delete;
};

}  // namespace dftracer::utils::index::gzip

#endif  // DFTRACER_UTILS_INDEX_GZIP_CHECKPOINT_INDEXER_H
