// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_RECORDS_H
#define DFTRACER_UTILS_YAML_RECORDS_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/yaml/document.h>

#include <cstddef>

namespace dftracer::utils::yaml {

struct RecordsOptions {
    /// About how many bytes of text one parse covers. A parse ends at the
    /// first record boundary at or after this many bytes, so a record larger
    /// than this is parsed whole.
    std::size_t chunk_bytes = std::size_t{64} << 20;
};

struct RecordsImpl;

/// The records of a large YAML text, one @ref Value at a time, with memory
/// bounded by the chunk and not by the text. The records are:
///
///  - the elements of a top-level block sequence, when the first content line
///    of the text starts with `- ` at column 0;
///  - the documents of a stream, when that line is a `---` marker;
///  - the whole text, as one record, for anything else, and no record for a
///    text with no content.
///
/// The text is cut into chunks, and each chunk is parsed on its own, where it
/// lies, with its own line index, whose buffers the next chunk reuses. A cut
/// falls only where it cannot split a construct:
///
///  - A line that starts at column 0 with `- ` (or a lone `-`) in a text that
///    is a top-level block sequence starts an element. Every line of a block
///    scalar, of a multi-line plain or quoted scalar and of a multi-line flow
///    collection that belongs to an element is indented by at least one
///    column, because the content of a sequence entry at column 0 sits
///    deeper than that column (YAML 1.2.2, 8.2.1 and 7.3). A comment starts
///    with `#`. So a column-0 `- ` is always an entry.
///  - A line that starts with `---` followed by a blank or the end of the
///    line, and the line after a `...` line, start a document: a document
///    marker at column 0 ends every scalar, whatever its style (YAML 1.2.2,
///    9.1.2, the c-forbidden rule). A `%` directive line in front of a `---`
///    goes with it.
///
/// A text that has no such line after the chunk size is one large chunk. A
/// chunk is parsed with no state from the one before, and a failed parse
/// reports its line counted from the start of the text. A document marker
/// inside a top-level sequence is an error.
///
/// The text is not changed and not copied for a parse. Text that the caller
/// gives in blocks (@ref from_blocks) is held only from the start of the
/// chunk, so only a chunk and the blocks after it are in memory. Records reads
/// no file: the caller reads the blocks.
class Records {
   public:
    Records(Records&&) noexcept;
    Records& operator=(Records&&) noexcept;
    ~Records();

    /// Takes the text, which the parse rewrites. `source_name` names it in an
    /// error. Nothing is parsed until @ref next.
    static Records from_text(dstd::String text, dstd::StringView source_name,
                             RecordsOptions options = {});

    /// Takes the text in blocks that the caller writes with @ref input_space
    /// and @ref commit_input, and ends with @ref end_input.
    static Records from_blocks(dstd::StringView source_name,
                               RecordsOptions options = {});

    /// Space for at least `size` more bytes of text, after the text given so
    /// far. The space and every record view stay valid until the next call of
    /// this, @ref commit_input or @ref next.
    dstd::Span<char> input_space(std::size_t size);

    /// Adds the first `size` bytes written to the last @ref input_space.
    void commit_input(std::size_t size);

    /// Says that no more text follows.
    void end_input();

    /// True when @ref next gave nothing because it needs more text, before
    /// @ref end_input.
    bool needs_input() const;

    /// The next record, or nothing after the last one or when @ref
    /// needs_input. The value and every view of it stay valid until the next
    /// call. A syntax error in a chunk is an InvalidArgument error like that of
    /// @ref Document::parse, and the records after the chunk are not read.
    dstd::Result<dstd::Optional<Value>> next();

   private:
    explicit Records(dstd::UniquePtr<RecordsImpl> impl);

    dstd::UniquePtr<RecordsImpl> impl_;
};

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_RECORDS_H
