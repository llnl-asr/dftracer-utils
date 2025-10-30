#ifndef DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_INTERNAL_COMPRESSED_CHUNK_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_INTERNAL_COMPRESSED_CHUNK_ITERATOR_H

#include <dftracer/utils/utilities/compression/zlib/types.h>
#include <dftracer/utils/utilities/io/types/types.h>
#include <zlib.h>

#include <cstring>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace dftracer::utils::utilities::compression::zlib::internal {

using io::ChunkIterator;
using io::CompressedData;
using io::RawData;

/**
 * @brief Lazy iterator that compresses chunks on-the-fly.
 *
 * This iterator wraps a ChunkIterator and compresses each chunk as it's read.
 * Only one input chunk and its compressed output are in memory at a time.
 *
 * Usage:
 * @code
 * ChunkIterator input_it("/file.txt", 64*1024);
 * ChunkIterator input_end;
 * CompressedChunkIterator compressed_it(input_it, input_end, 9);
 * CompressedChunkIterator compressed_end;
 *
 * for (; compressed_it != compressed_end; ++compressed_it) {
 *     const CompressedData& chunk = *compressed_it;
 *     // Write compressed chunk immediately
 * }
 * @endcode
 */
class CompressedChunkIterator {
   private:
    struct CompressionState {
        ChunkIterator input_it;
        ChunkIterator input_end;
        z_stream stream;
        bool initialized = false;
        bool is_end = false;
        int compression_level;
        CompressionFormat format;

        std::vector<unsigned char> output_buffer;
        std::vector<CompressedData> pending_chunks;  // Buffered output chunks
        std::size_t pending_index = 0;

        static constexpr std::size_t OUTPUT_BUFFER_SIZE = 64 * 1024;

        CompressionState(ChunkIterator it, ChunkIterator end, int level,
                         CompressionFormat fmt = CompressionFormat::GZIP)
            : input_it(it),
              input_end(end),
              compression_level(level),
              format(fmt),
              output_buffer(OUTPUT_BUFFER_SIZE) {
            std::memset(&stream, 0, sizeof(stream));

            if (input_it != input_end) {
                initialize();
                compress_next_input_chunk();
            } else {
                is_end = true;
            }
        }

        ~CompressionState() {
            if (initialized) {
                deflateEnd(&stream);
            }
        }

        void initialize() {
            int ret =
                deflateInit2(&stream, compression_level, Z_DEFLATED,
                             static_cast<int>(format),  // Use format enum value
                             8, Z_DEFAULT_STRATEGY);

            if (ret != Z_OK) {
                throw std::runtime_error("Failed to initialize deflate");
            }

            initialized = true;
        }

        void compress_next_input_chunk() {
            pending_chunks.clear();
            pending_index = 0;

            if (input_it == input_end) {
                // Finalize compression
                finalize();
                return;
            }

            const RawData& chunk = *input_it;

            stream.avail_in = static_cast<uInt>(chunk.size());
            stream.next_in = const_cast<Bytef*>(chunk.data.data());

            while (stream.avail_in > 0) {
                stream.avail_out = static_cast<uInt>(output_buffer.size());
                stream.next_out = output_buffer.data();

                int ret = deflate(&stream, Z_NO_FLUSH);
                if (ret == Z_STREAM_ERROR) {
                    throw std::runtime_error("Deflate stream error");
                }

                std::size_t compressed_size =
                    output_buffer.size() - stream.avail_out;
                if (compressed_size > 0) {
                    std::vector<unsigned char> compressed_data(
                        output_buffer.begin(),
                        output_buffer.begin() + compressed_size);

                    pending_chunks.push_back(CompressedData{
                        std::move(compressed_data), chunk.size()});
                }
            }

            ++input_it;

            if (pending_chunks.empty() && input_it != input_end) {
                // No output yet, compress next chunk
                compress_next_input_chunk();
            } else if (pending_chunks.empty() && input_it == input_end) {
                // End of input, finalize
                finalize();
            }
        }

        void finalize() {
            int ret;
            do {
                stream.avail_out = static_cast<uInt>(output_buffer.size());
                stream.next_out = output_buffer.data();

                ret = deflate(&stream, Z_FINISH);
                if (ret == Z_STREAM_ERROR) {
                    throw std::runtime_error(
                        "Deflate error during finalization");
                }

                std::size_t compressed_size =
                    output_buffer.size() - stream.avail_out;
                if (compressed_size > 0) {
                    std::vector<unsigned char> compressed_data(
                        output_buffer.begin(),
                        output_buffer.begin() + compressed_size);

                    pending_chunks.push_back(
                        CompressedData{std::move(compressed_data), 0});
                }
            } while (ret == Z_OK);

            if (ret != Z_STREAM_END) {
                throw std::runtime_error("Failed to finalize compression");
            }

            if (pending_chunks.empty()) {
                is_end = true;
            }
        }

        void advance() {
            pending_index++;
            if (pending_index >= pending_chunks.size()) {
                // Need more chunks
                if (input_it != input_end) {
                    compress_next_input_chunk();
                } else {
                    is_end = true;
                }
            }
        }

        const CompressedData& current() const {
            return pending_chunks[pending_index];
        }
    };

    std::shared_ptr<CompressionState> state_;

   public:
    // Iterator traits
    using iterator_category = std::input_iterator_tag;
    using value_type = CompressedData;
    using difference_type = std::ptrdiff_t;
    using pointer = const CompressedData*;
    using reference = const CompressedData&;

    // End iterator
    CompressedChunkIterator() : state_(nullptr) {}

    // Begin iterator
    CompressedChunkIterator(ChunkIterator input_begin, ChunkIterator input_end,
                            int compression_level = Z_DEFAULT_COMPRESSION,
                            CompressionFormat format = CompressionFormat::GZIP)
        : state_(std::make_shared<CompressionState>(
              input_begin, input_end, compression_level, format)) {
        if (state_->is_end) {
            state_ = nullptr;
        }
    }

    reference operator*() const { return state_->current(); }

    pointer operator->() const { return &state_->current(); }

    CompressedChunkIterator& operator++() {
        if (state_) {
            state_->advance();
            if (state_->is_end) {
                state_ = nullptr;
            }
        }
        return *this;
    }

    CompressedChunkIterator operator++(int) {
        CompressedChunkIterator tmp = *this;
        ++(*this);
        return tmp;
    }

    bool operator==(const CompressedChunkIterator& other) const {
        if (!state_ && !other.state_) return true;
        if (!state_ || !other.state_) return false;
        return state_ == other.state_;
    }

    bool operator!=(const CompressedChunkIterator& other) const {
        return !(*this == other);
    }
};
}  // namespace dftracer::utils::utilities::compression::zlib::internal

#endif  // DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_INTERNAL_COMPRESSED_CHUNK_ITERATOR_H
