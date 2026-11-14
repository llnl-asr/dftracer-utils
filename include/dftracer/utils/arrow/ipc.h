// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_ARROW_IPC_H
#define DFTRACER_UTILS_ARROW_IPC_H

#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/batch.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::arrow {

/// Whether a @ref Writer compresses each buffer of a record batch.
enum class Compression : std::uint8_t {
    None,  ///< Uncompressed
    Zstd,  ///< zstd, buffer level (the Arrow IPC body compression)
};

/// What a @ref Writer does with the batches it is given.
struct WriterOptions {
    Compression compression = Compression::None;
};

/**
 * @brief Writes record batches to an Arrow IPC sink: a file, or a buffer the
 * caller takes at the end.
 *
 * One frame is one record batch, and its columns must match the columns of the
 * first frame written. A file holds the Arrow IPC file format (magic, footer),
 * so any reader that opens a `.arrow` file opens it; each batch is compressed
 * and written as it comes, so the file never holds the frame set. A buffer
 * holds the Arrow IPC stream format, which a `.arrow` file can also carry, and
 * @ref Reader reads both. An error is sticky: after a failed call every later
 * call fails the same way.
 *
 * Move-only, not thread safe. Call @ref finish before destroying a Writer, and
 * read the file only after it.
 */
class Writer {
   public:
    /// A writer of `path`, which it creates, replacing any file there.
    static dstd::Result<Writer> open(const dstd::Path& path,
                                     WriterOptions options = {});

    /// A writer of a buffer; @ref take_bytes hands the bytes out.
    static Writer to_buffer(WriterOptions options = {});

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&& other) noexcept;
    Writer& operator=(Writer&& other) noexcept;
    ~Writer();

    /// Appends `frame` as one record batch.
    dstd::Result<void> write(const dataframe::DataFrame& frame);

    /// Writes the end-of-stream marker and, for a file, the footer; closes the
    /// file. A second call does nothing and succeeds.
    dstd::Result<void> finish();

    /// The bytes of a buffer writer, after @ref finish. Empty for a file
    /// writer.
    dstd::Vector<std::uint8_t> take_bytes();

   private:
    struct Impl;
    explicit Writer(dstd::UniquePtr<Impl> impl) noexcept;

    dstd::UniquePtr<Impl> impl_;
};

/**
 * @brief Reads the record batches of an Arrow IPC file, or of bytes holding
 * one, one at a time.
 *
 * @ref next gives one record batch per call as a DataFrame, in file order, and
 * nullopt at the end. The frames alias the file mapping or the caller's bytes
 * zero copy, so both must outlive every frame taken out of the reader.
 *
 * Move-only, not thread safe.
 */
class Reader {
   public:
    /// A reader over the whole of `path`, mapped read only.
    static dstd::Result<Reader> open(const dstd::Path& path);

    /**
     * A reader over @p bytes, which must outlive the reader and every frame
     * @ref next returned from it. The bytes are not copied.
     */
    static dstd::Result<Reader> from_bytes(
        dstd::Span<const std::uint8_t> bytes);

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&& other) noexcept;
    Reader& operator=(Reader&& other) noexcept;
    ~Reader();

    /// The columns every batch of this sink carries, without reading a batch.
    dataframe::Schema schema() const;

    /// The next record batch, or nullopt at the end of the stream.
    dstd::Result<dstd::Optional<dataframe::DataFrame>> next();

   private:
    struct Impl;
    explicit Reader(dstd::UniquePtr<Impl> impl) noexcept;

    dstd::UniquePtr<Impl> impl_;
};

/**
 * @brief Every record batch of @p paths as a lazy plan, one morsel per record
 * batch, in file order.
 *
 * The schema comes from the first file's footer, so planning reads no batch.
 * A column projection in the request is applied per batch, so a dropped column
 * is never decoded into a morsel. The files are decoded concurrently, as many
 * at once as the memory budget of the request holds, and their batches come
 * out in file order. Every file must hold the first file's columns: a file
 * with other columns fails with InvalidArgument. A file that is not Arrow IPC,
 * or a batch that does not decode, fails the collect.
 */
dstd::Result<dataframe::LazyFrame> scan_ipc(dstd::Span<const dstd::Path> paths);

/**
 * @brief @ref scan_ipc collected: `read_ipc(paths) == scan_ipc(paths) ->
 * collect()`, nothing more.
 */
dstd::Result<dataframe::DataFrame> read_ipc(dstd::Span<const dstd::Path> paths);

}  // namespace dftracer::utils::arrow

#endif  // DFTRACER_UTILS_ARROW_IPC_H
