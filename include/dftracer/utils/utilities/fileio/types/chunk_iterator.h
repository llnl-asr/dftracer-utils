#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_ITERATOR_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/filesystem.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Iterator that lazily reads file chunks on demand.
 *
 * Reads chunks from a file only when dereferenced. Only one chunk
 * is kept in memory at a time. Exposes chunks as ByteView into
 * the internal read buffer -- zero copy on dereference.
 */
class ChunkIterator {
   private:
    struct State {
        fs::path path;
        std::size_t chunk_size;
        std::ifstream file;
        std::vector<unsigned char> buffer;
        ByteView current_view;
        bool is_end = false;

        State(fs::path p, std::size_t cs)
            : path(std::move(p)), chunk_size(cs), buffer(cs) {
            file.open(path, std::ios::binary);
            if (!file) {
                is_end = true;
            } else {
                read_next_chunk();
            }
        }

        void read_next_chunk() {
            file.read(reinterpret_cast<char*>(buffer.data()),
                      static_cast<std::streamsize>(chunk_size));
            std::streamsize bytes_read = file.gcount();

            if (bytes_read > 0) {
                current_view = ByteView(buffer.data(),
                                        static_cast<std::size_t>(bytes_read));
            } else {
                is_end = true;
            }
        }
    };

    std::shared_ptr<State> state_;

   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = ByteView;
    using difference_type = std::ptrdiff_t;
    using pointer = const ByteView*;
    using reference = const ByteView&;

    ChunkIterator() : state_(nullptr) {}

    ChunkIterator(fs::path path, std::size_t chunk_size = 64 * 1024)
        : state_(std::make_shared<State>(std::move(path), chunk_size)) {
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
        return state_->path == other.state_->path &&
               state_->file.tellg() == other.state_->file.tellg();
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
    fs::path path_;
    std::size_t chunk_size_;

   public:
    ChunkRange() : path_(), chunk_size_(64 * 1024) {}
    ChunkRange(fs::path path, std::size_t chunk_size = 64 * 1024)
        : path_(std::move(path)), chunk_size_(chunk_size) {}

    ChunkIterator begin() const { return ChunkIterator{path_, chunk_size_}; }
    ChunkIterator end() const { return ChunkIterator{}; }
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_ITERATOR_H
