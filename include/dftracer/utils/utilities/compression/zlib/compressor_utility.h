#ifndef DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_COMPRESSOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_COMPRESSOR_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/io/types/types.h>
#include <zlib.h>

#include <stdexcept>

namespace dftracer::utils::utilities::compression::zlib {

// Use I/O types for compression
using io::CompressedData;
using io::RawData;

/**
 * @brief Utility that compresses raw data using gzip compression.
 *
 * This utility takes raw data and compresses it using the zlib library
 * with gzip format. It can be used standalone or composed in pipelines.
 *
 * Features:
 * - Configurable compression level (0-9, default 6)
 * - Returns compressed data with original size metadata
 * - Can be tagged with Cacheable, Retryable, Monitored behaviors
 *
 * Usage:
 * @code
 * auto compressor = std::make_shared<Compressor>();
 * compressor->set_compression_level(9);  // Maximum compression
 *
 * RawData input("Hello, World!");
 * CompressedData output = compressor->process(input);
 *
 * std::cout << "Compressed " << output.original_size
 *           << " bytes to " << output.data.size() << " bytes\n";
 * std::cout << "Compression ratio: " << output.compression_ratio() << "\n";
 * std::cout << "Space savings: " << output.space_savings() << "%\n";
 * @endcode
 *
 * With pipeline:
 * @code
 * Pipeline pipeline;
 * auto task = use(compressor).emit_on(pipeline);
 * auto output = SequentialExecutor().execute(pipeline, RawData{"data"});
 * auto compressed = output.get<CompressedData>(task.id());
 * @endcode
 */
class CompressorUtility
    : public utilities::Utility<RawData, CompressedData,
                                utilities::tags::Parallelizable> {
   private:
    int compression_level_ = Z_DEFAULT_COMPRESSION;  // Default: 6

   public:
    CompressorUtility() = default;
    ~CompressorUtility() override = default;

    /**
     * @brief Set compression level (0-9).
     * @param level 0 = no compression, 9 = maximum compression, -1 = default
     */
    void set_compression_level(int level) {
        if (level < 0 || level > 9) {
            if (level != Z_DEFAULT_COMPRESSION) {
                throw std::invalid_argument(
                    "Compression level must be 0-9 or Z_DEFAULT_COMPRESSION");
            }
        }
        compression_level_ = level;
    }

    int get_compression_level() const { return compression_level_; }

    /**
     * @brief Compress raw data using gzip format.
     *
     * @param input Raw data to compress
     * @return CompressedData with compressed bytes and metadata
     * @throws std::runtime_error if compression fails
     */
    CompressedData process(const RawData& input) override {
        if (input.data.empty()) {
            return CompressedData({}, 0);
        }

        // Estimate upper bound for compressed size
        uLong src_len = static_cast<uLong>(input.data.size());
        uLong dest_len = compressBound(src_len);

        std::vector<unsigned char> compressed(dest_len);

        // Perform compression
        int result = compress2(compressed.data(), &dest_len, input.data.data(),
                               src_len, compression_level_);

        if (result != Z_OK) {
            throw std::runtime_error(
                "Gzip compression failed with error code: " +
                std::to_string(result));
        }

        // Resize to actual compressed size
        compressed.resize(dest_len);

        return CompressedData(std::move(compressed), input.data.size());
    }
};

}  // namespace dftracer::utils::utilities::compression::zlib

#endif  // DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_COMPRESSOR_UTILITY_H
