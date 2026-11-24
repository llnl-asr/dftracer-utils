// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_FORMATS_TEXT_SOURCE_H
#define DFTRACER_UTILS_PYTHON_FORMATS_TEXT_SOURCE_H

#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/gzip_stream_inflater.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::python::formats {

/// The text of a file, read in blocks: a plain file as it is, and a gzip file,
/// whatever its name, as the text of its members. Not thread safe.
class TextSource {
   public:
    static dstd::Result<TextSource> open(dstd::StringView path);

    TextSource(TextSource &&) noexcept;
    TextSource &operator=(TextSource &&) noexcept;
    ~TextSource();

    /// Reads up to `capacity` bytes to `out`; 0 at the end of the text.
    dstd::Result<std::size_t> read(char *out, std::size_t capacity);

    /// The size of the file on disk, which bounds a plain file's text.
    std::uint64_t file_size() const { return file_size_; }
    bool is_gzip() const { return gzip_; }

   private:
    TextSource() = default;

    dstd::File file_;
    dstd::UniquePtr<fileio::compress::GzipStreamInflater> inflater_;
    dstd::Vector<unsigned char> input_;
    std::uint64_t at_ = 0;
    std::uint64_t file_size_ = 0;
    bool gzip_ = false;
    bool in_member_ = false;
    bool ended_ = false;
};

}  // namespace dftracer::utils::python::formats

#endif  // DFTRACER_UTILS_PYTHON_FORMATS_TEXT_SOURCE_H
