// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_ITERATOR_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdio>
#include <iterator>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Iterator that lazily reads file chunks on demand.
 *
 * Reads chunks from a file only when dereferenced. Only one chunk
 * is kept in memory at a time. Exposes chunks as @ref
 * dftracer::utils::dstd::ByteView "ByteView" into the internal read buffer --
 * zero copy on dereference.
 */
class ChunkIterator {
   private:
    struct State {
        dstd::Path path;
        std::size_t chunk_size;
        dstd::ScopedFile file;
        dstd::Vector<unsigned char> buffer;
        dstd::ByteView current_view;
        bool is_end = false;

        State(dstd::Path p, std::size_t cs)
            : path(std::move(p)), chunk_size(cs), buffer(cs) {
            file.reset(std::fopen(path.c_str(), "rb"));
            if (!file) {
                is_end = true;
            } else {
                read_next_chunk();
            }
        }

        void read_next_chunk() {
            const std::size_t bytes_read =
                std::fread(buffer.data(), 1, chunk_size, file.get());

            if (bytes_read > 0) {
                current_view = dstd::ByteView(buffer.data(), bytes_read);
            } else {
                is_end = true;
            }
        }
    };

    dstd::SharedPtr<State> state_;

   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = dstd::ByteView;
    using difference_type = std::ptrdiff_t;
    using pointer = const dstd::ByteView*;
    using reference = const dstd::ByteView&;

    ChunkIterator() : state_(nullptr) {}

    ChunkIterator(dstd::Path path, std::size_t chunk_size = 64 * 1024)
        : state_(dstd::make_shared<State>(std::move(path), chunk_size)) {
        if (state_->is_end) {
            state_ = nullptr;
        }
    }

    reference operator*() const { return state_->current_view; }
    pointer operator->() const { return &state_->current_view; }

    ChunkIterator& operator++() {
        if (state_) {
            state_->read_next_chunk();
            if (state_->is_end) {
                state_ = nullptr;
            }
        }
        return *this;
    }

    ChunkIterator operator++(int) {
        ChunkIterator tmp = *this;
        ++(*this);
        return tmp;
    }

    bool operator==(const ChunkIterator& other) const {
        if (!state_ && !other.state_) return true;
        if (!state_ || !other.state_) return false;
        return state_->path.str() == other.state_->path.str() &&
               std::ftell(state_->file.get()) ==
                   std::ftell(other.state_->file.get());
    }

    bool operator!=(const ChunkIterator& other) const {
        return !(*this == other);
    }
};

/**
 * @brief Range wrapper for chunk iteration (enables range-based for).
 */
class ChunkRange {
   private:
    dstd::Path path_;
    std::size_t chunk_size_;

   public:
    ChunkRange() : path_(), chunk_size_(64 * 1024) {}
    ChunkRange(dstd::Path path, std::size_t chunk_size = 64 * 1024)
        : path_(std::move(path)), chunk_size_(chunk_size) {}

    ChunkIterator begin() const { return ChunkIterator{path_, chunk_size_}; }
    ChunkIterator end() const { return ChunkIterator{}; }
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_ITERATOR_H
