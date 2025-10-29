#ifndef DFTRACER_UTILS_UTILITIES_IO_CHUNK_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_IO_CHUNK_ITERATOR_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/io/types/types.h>

#include <fstream>
#include <iterator>
#include <memory>

namespace dftracer::utils::utilities::io {

/**
 * @brief Iterator that lazily reads file chunks on demand.
 *
 * This iterator reads chunks from a file only when dereferenced.
 * Only one chunk is kept in memory at a time.
 *
 * Usage:
 * @code
 * ChunkIterator it("/path/to/file.txt", 64 * 1024);
 * ChunkIterator end;  // End iterator
 *
 * for (; it != end; ++it) {
 *     const RawData& chunk = *it;
 *     // Process chunk - only this chunk is in memory
 * }
 * @endcode
 *
 * Range-based for loop:
 * @code
 * ChunkRange range("/path/to/file.txt", 64 * 1024);
 * for (const auto& chunk : range) {
 *     // Process chunk lazily
 * }
 * @endcode
 */
class ChunkIterator {
   private:
    struct State {
        fs::path path;
        std::size_t chunk_size;
        std::ifstream file;
        std::vector<unsigned char> buffer;
        RawData current_chunk;
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
                std::vector<unsigned char> chunk_data(
                    buffer.begin(), buffer.begin() + bytes_read);
                current_chunk = RawData{std::move(chunk_data)};
            } else {
                is_end = true;
            }
        }
    };

    std::shared_ptr<State> state_;

   public:
    // Iterator traits
    using iterator_category = std::input_iterator_tag;
    using value_type = RawData;
    using difference_type = std::ptrdiff_t;
    using pointer = const RawData*;
    using reference = const RawData&;

    // Default constructor creates end iterator
    ChunkIterator() : state_(nullptr) {}

    // Constructor for begin iterator
    ChunkIterator(fs::path path, std::size_t chunk_size = 64 * 1024)
        : state_(std::make_shared<State>(std::move(path), chunk_size)) {
        if (state_->is_end) {
            state_ = nullptr;  // Convert to end iterator
        }
    }

    // Dereference - returns current chunk
    reference operator*() const { return state_->current_chunk; }

    pointer operator->() const { return &state_->current_chunk; }

    // Pre-increment - read next chunk
    ChunkIterator& operator++() {
        if (state_) {
            state_->read_next_chunk();
            if (state_->is_end) {
                state_ = nullptr;  // Convert to end iterator
            }
        }
        return *this;
    }

    // Post-increment
    ChunkIterator operator++(int) {
        ChunkIterator tmp = *this;
        ++(*this);
        return tmp;
    }

    // Equality comparison
    bool operator==(const ChunkIterator& other) const {
        // Two end iterators are equal
        if (!state_ && !other.state_) return true;
        // End iterator != non-end iterator
        if (!state_ || !other.state_) return false;
        // Compare paths (same file, same position)
        return state_->path == other.state_->path &&
               state_->file.tellg() == other.state_->file.tellg();
    }

    bool operator!=(const ChunkIterator& other) const {
        return !(*this == other);
    }
};

/**
 * @brief Range wrapper for chunk iteration (enables range-based for).
 *
 * Usage:
 * @code
 * ChunkRange range("/path/to/file.txt", 64 * 1024);
 * for (const auto& chunk : range) {
 *     // Each chunk is read lazily, only one in memory at a time
 *     process_chunk(chunk);
 * }
 * @endcode
 */
class ChunkRange {
   private:
    fs::path path_;
    std::size_t chunk_size_;

   public:
    ChunkRange(fs::path path, std::size_t chunk_size = 64 * 1024)
        : path_(std::move(path)), chunk_size_(chunk_size) {}

    ChunkIterator begin() const { return ChunkIterator{path_, chunk_size_}; }

    ChunkIterator end() const {
        return ChunkIterator{};  // End iterator
    }
};

}  // namespace dftracer::utils::utilities::io

#endif  // DFTRACER_UTILS_UTILITIES_IO_CHUNK_ITERATOR_H
