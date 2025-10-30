#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INTERNAL_CHUNK_SPEC_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INTERNAL_CHUNK_SPEC_H

#include <dftracer/utils/utilities/io/types/chunk_spec.h>

namespace dftracer::utils::utilities::composites::dft::internal {

/**
 * @brief DFTracer-specific extension of ChunkSpec with line tracking.
 *
 * Extends the base io::ChunkSpec with line number information for
 * verification, debugging, and metadata tracking purposes.
 */
struct DFTracerChunkSpec : public io::ChunkSpec {
    std::size_t start_line;  // Starting line number (1-based, inclusive)
    std::size_t end_line;    // Ending line number (1-based, inclusive)

    DFTracerChunkSpec() : io::ChunkSpec(), start_line(0), end_line(0) {}

    DFTracerChunkSpec(std::string path, std::string idx, double mb,
                      std::size_t start_byte_offset,
                      std::size_t end_byte_offset, std::size_t start_ln,
                      std::size_t end_ln)
        : io::ChunkSpec(std::move(path), std::move(idx), mb, start_byte_offset,
                        end_byte_offset),
          start_line(start_ln),
          end_line(end_ln) {}

    // Convert from base ChunkSpec
    static DFTracerChunkSpec from_chunk_spec(const io::ChunkSpec& spec) {
        DFTracerChunkSpec dft_spec;
        dft_spec.file_path = spec.file_path;
        dft_spec.idx_path = spec.idx_path;
        dft_spec.size_mb = spec.size_mb;
        dft_spec.start_byte = spec.start_byte;
        dft_spec.end_byte = spec.end_byte;
        dft_spec.start_line = 0;
        dft_spec.end_line = 0;
        return dft_spec;
    }

    DFTracerChunkSpec& with_lines(std::size_t start_ln, std::size_t end_ln) {
        start_line = start_ln;
        end_line = end_ln;
        return *this;
    }

    std::size_t num_lines() const {
        return (end_line >= start_line && start_line > 0)
                   ? (end_line - start_line + 1)
                   : 0;
    }

    bool has_line_info() const { return start_line > 0 && end_line > 0; }

    bool operator==(const DFTracerChunkSpec& other) const {
        return io::ChunkSpec::operator==(other) &&
               start_line == other.start_line && end_line == other.end_line;
    }

    bool operator!=(const DFTracerChunkSpec& other) const {
        return !(*this == other);
    }
};

}  // namespace dftracer::utils::utilities::composites::dft::internal

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INTERNAL_CHUNK_SPEC_H
