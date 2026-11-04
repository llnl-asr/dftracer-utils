// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_GZIP_CHECKPOINT_INDEXER_H
#define DFTRACER_UTILS_INDEX_GZIP_CHECKPOINT_INDEXER_H

#include <dftracer/utils/common/archive_format.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/index_visitor.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace dftracer::utils::index::gzip {

/**
 * Abstract base interface for all indexer implementations.
 * This provides a common API for GZIP, TAR.GZ, and other archive indexers.
 */
class CheckpointIndexer {
   public:
    static constexpr std::uint64_t DEFAULT_CHECKPOINT_SIZE =
        common::constants::indexer::DEFAULT_CHECKPOINT_SIZE;

    virtual ~CheckpointIndexer() = default;

    // Core indexer operations
    virtual coro::CoroTask<void> build_async() const = 0;

    void build() const { build_async().get(); }
    virtual bool need_rebuild() const = 0;
    virtual bool exists() const = 0;

    using VisitorList =
        dstd::Vector<std::reference_wrapper<index::build::IndexVisitor>>;

    virtual void set_visitors(VisitorList) {}

    // Metadata accessors
    virtual const dstd::String &index_path() const = 0;
    virtual const dstd::String &archive_path() const = 0;
    virtual std::uint64_t checkpoint_size() const = 0;
    virtual std::uint64_t max_bytes() const = 0;
    virtual std::uint64_t line_count() const = 0;

    /// Member record (a member or a piece of one) containing `target_offset`
    /// (uncompressed). A RESTART piece also needs its `restart_window`.
    virtual bool find_member(std::size_t target_offset,
                             GzipMemberRecord &member) const = 0;

    /// Every member record in file order. Empty when the file was never
    /// indexed.
    virtual dstd::Vector<GzipMemberRecord> members() const = 0;

    /// The compressed offsets of the HEAD records, the members too big to hold
    /// whole. Empty when there are none or the file was never indexed.
    virtual dstd::Vector<std::uint64_t> piece_heads() const = 0;

    /// The 32 KiB inflate window of RESTART piece `member_idx`; empty when
    /// the index has none for it.
    virtual dstd::String restart_window(std::uint64_t member_idx) const = 0;

    // Archive format identification
    virtual common::ArchiveFormat format_type() const = 0;
    virtual const char *format_name() const = 0;

   protected:
    CheckpointIndexer() = default;
    CheckpointIndexer(const CheckpointIndexer &) = delete;
    CheckpointIndexer &operator=(const CheckpointIndexer &) = delete;
};

}  // namespace dftracer::utils::index::gzip

#endif  // DFTRACER_UTILS_INDEX_GZIP_CHECKPOINT_INDEXER_H
