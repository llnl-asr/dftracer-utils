// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_RECORDS_H
#define DFTRACER_UTILS_YAML_RECORDS_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/result.h>
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
/// The text is cut into chunks, and each chunk is parsed on its own, in
/// place, with its own tape, whose buffers the next chunk reuses. A cut falls
/// only where it cannot split a construct:
///
///  - A line that starts at column 0 with `- ` (or a lone `-`) in a text that
///    is a top-level block sequence starts an element. Every line of a block
///    scalar, of a multi-line plain or quoted scalar and of a multi-line flow
///    collection that belongs to an element is indented by at least one
///    column, because the content of a sequence entry at column 0 sits
///    deeper than that column (YAML 1.2.2, 8.2.1 and 7.3). A comment starts
///    with `#`, and an alias, an anchor or a tag cannot start a line of a
///    sequence at column 0 either. So a column-0 `- ` is always an entry.
///  - A line that starts with `---` followed by a blank or the end of the
///    line, and the line after a `...` line, start a document: a document
///    marker at column 0 ends every scalar, whatever its style (YAML 1.2.2,
///    9.1.2, the c-forbidden rule). A `%` directive line in front of a `---`
///    goes with it.
///
/// A text that has no such line after the chunk size is one large chunk. A
/// chunk is parsed with no state from the one before, so an alias must name
/// an anchor of its own chunk, and a failed parse reports its line counted
/// from the start of the text. A document marker inside a top-level sequence
/// is an error, as an anchor shared across a cut is.
///
/// The text is parsed in place: a string moved in is rewritten. A file is read
/// in blocks, so only a chunk and one block of the text are in memory.
class Records {
   public:
    Records(Records&&) noexcept;
    Records& operator=(Records&&) noexcept;
    ~Records();

    /// Takes the text, which the parse rewrites. `source_name` names it in an
    /// error. Nothing is parsed until @ref next.
    static Records from_text(dstd::String text, dstd::StringView source_name,
                             RecordsOptions options = {});

    /// Reads the file at `path` as it goes.
    static dstd::Result<Records> open(dstd::PathView path,
                                      RecordsOptions options = {});

    /// The next record, or nothing after the last one. The value and every
    /// view of it stay valid until the next call. A syntax error in a chunk
    /// is an InvalidArgument error like that of @ref Document::parse, and the
    /// records after the chunk are not read.
    dstd::Result<dstd::Optional<Value>> next();

   private:
    explicit Records(dstd::UniquePtr<RecordsImpl> impl);

    dstd::UniquePtr<RecordsImpl> impl_;
};

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_RECORDS_H
