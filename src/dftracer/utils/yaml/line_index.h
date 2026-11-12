// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_LINE_INDEX_H
#define DFTRACER_UTILS_YAML_LINE_INDEX_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/raw_buffer.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/yaml/tape.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::yaml {

/// What a line of a YAML text is. The Open kinds are an entry or an item with
/// no inline value that has a deeper block under it.
///
/// A scalar that goes on in later lines (a block scalar, a plain or quoted
/// scalar of several lines) belongs to the line that holds its start, whose
/// `VAL_ESC` flag says that its text is in the side table. A flow collection
/// belongs to the line that holds its start in the same way, with the `FLOW`
/// flag, and its entry in the table names the first tape entry of the
/// collection. The lines after it are Cont, up to the end the table entry
/// names.
enum class LineKind : std::uint8_t {
    Blank,
    Comment,
    MapEntry,
    MapOpen,
    SeqItem,
    SeqOpen,
    /// `- k: v`: an item that starts a map on its own line.
    SeqMap,
    SeqMapOpen,
    /// `- - x`: an item that holds a sequence that starts on its own line. The
    /// dashes are read from the text, and the kind is that of the last piece
    /// of the line: an item, an item with a block under it, a map or a map with
    /// a block under it.
    NestItem,
    NestItemOpen,
    NestMap,
    NestMapOpen,
    DocStart,
    DocEnd,
    /// A line inside a scalar of several lines.
    Cont,
    /// A top-level scalar that starts the line: plain, quoted, or the header
    /// of a block scalar.
    Root,
    /// `--- ` and a top-level scalar on the same line.
    RootDoc,
};

namespace line_flag {
/// A comment follows the content of the line.
constexpr std::uint8_t COMMENT = 1;
constexpr std::uint8_t KEY_ESC = 2;
constexpr std::uint8_t VAL_ESC = 4;
/// No inline value: a null, or a block below.
constexpr std::uint8_t VAL_EMPTY = 8;
/// The line ends in CR LF.
constexpr std::uint8_t CR = 16;
/// The value is a flow collection, whose tape index is in the side table.
constexpr std::uint8_t FLOW = 32;
constexpr std::uint8_t KEY_QUOTED = 64;
constexpr std::uint8_t VAL_QUOTED = 128;
}  // namespace line_flag

/// One line of the text, 8 bytes.
struct LineEntry {
    std::uint32_t start;
    std::uint16_t indent;
    LineKind kind;
    std::uint8_t flags;
};
static_assert(sizeof(LineEntry) == 8, "a line entry is 8 bytes");

/// The text of a scalar of a line that the text does not hold as it is: an
/// escaped quoted scalar, a block scalar, a scalar of several lines. `key` is
/// the line index times two, plus one for a value. `off` and `len` give the
/// text in the arena, or in the text of the index when `end` has `IN_TEXT`. For
/// a scalar of several lines, the rest of `end` is the line after its last
/// line, and the first Cont line has the index of this entry in `next`.
struct EscapeEntry {
    std::uint32_t key;
    std::uint32_t off;
    std::uint32_t len;
    std::uint32_t end;
};
static_assert(sizeof(EscapeEntry) == 16, "an escape entry is 16 bytes");

constexpr std::uint32_t ESCAPE_IN_TEXT = 0x80000000u;

/// What goes on after the first line of a scalar, as classify_line finds it.
namespace LineOpen {
constexpr std::uint8_t NONE = 0;
/// A quoted scalar that the line does not close.
constexpr std::uint8_t Quoted = 1;
/// A block scalar: the line holds its header.
constexpr std::uint8_t Block = 2;
/// A flow collection that starts at `open_at`, and may go on in later lines.
constexpr std::uint8_t Flow = 3;
}  // namespace LineOpen

/// The facts of one line, read from its text.
struct LineParts {
    /// Set when classify_line fails: the reason, and the offset it is at.
    const char* reason = nullptr;
    std::uint32_t error_at = 0;
    LineKind kind = LineKind::Blank;
    std::uint8_t flags = 0;
    std::uint32_t indent = 0;
    /// Where the item dash is, for the Seq kinds.
    std::uint32_t dash = 0;
    /// Where the key starts, or the item value for a SeqItem.
    std::uint32_t content = 0;
    /// The key text without quotes, then the value text without quotes. The
    /// value of a flow collection starts at `val_s`.
    std::uint32_t key_s = 0;
    std::uint32_t key_e = 0;
    std::uint32_t val_s = 0;
    std::uint32_t val_e = 0;
    /// A @ref LineOpen, and the offset of the quote, of the `|` or `>`, or of
    /// the `[` or `{`.
    std::uint8_t open = LineOpen::NONE;
    std::uint32_t open_at = 0;
    /// The dashes after the first one of a NestItem or NestMap line.
    std::uint32_t nest = 0;
    /// A TAB follows the spaces of the line and then comes its content, as
    /// white space: `error_at` is the offset of the TAB.
    bool has_tab_lead = false;
};

/// Reads the facts of the line that holds `[start, end)`, with the CR cut.
/// False when the line is invalid or holds a feature that is not supported:
/// then `parts.reason` says which.
bool classify_line(const char* text, std::uint32_t start, std::uint32_t end,
                   LineParts& parts);

/// What the line index made of a text.
enum class BuildResult : std::uint8_t {
    Accepted,
    /// `problem` says where and why.
    Invalid,
};

/// Where a text is invalid and why. `line` and `col` count from 1 in the text.
struct LineProblem {
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    dstd::String reason;
};

/// The bytes of a UTF-8 byte order mark at the start of a text, else 0. The
/// reader skips it, so it is no part of the first line.
inline std::size_t bom_size(const char* text, std::size_t size) {
    return size >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                   static_cast<unsigned char>(text[1]) == 0xBB &&
                   static_cast<unsigned char>(text[2]) == 0xBF
               ? 3
               : 0;
}

/// The line index of a text: a line for each line of the text, the end of the
/// subtree of each content line, the decoded text of the scalars that the text
/// does not hold as they are, and the tape of each flow collection.
struct LineIndex {
    dstd::RawBuffer<LineEntry> entries;
    /// The nodes of the flow collections of the text, in text order.
    dstd::RawBuffer<TapeEntry> tape;
    /// The index of the first content line after the subtree of a line: the
    /// first one whose indent is not deeper. For a SeqMap line the subtree is
    /// the whole item.
    /// For the first Cont line of a scalar of several lines: the index of its
    /// entry in `escapes`.
    dstd::RawBuffer<std::uint32_t> next;
    dstd::RawBuffer<char> arena;
    dstd::Vector<EscapeEntry> escapes;
    const char* text = nullptr;
    std::uint32_t size = 0;
    /// The lines of the text. `entries` and `next` hold one more, whose start
    /// is one past the end of the last line.
    std::uint32_t line_count = 0;
    /// True when the text holds a `---` or `...` marker.
    bool has_markers = false;

    /// Indexes `size` bytes at `text`. The index keeps the pointer, so the
    /// text outlives it. When the text is invalid, the index is unusable.
    BuildResult build(const char* text, std::size_t size);

    /// Set by a build that gives Invalid.
    LineProblem problem;

    /// Frees the buffers.
    void release();

    /// The text of the line `line`, without its CR.
    std::uint32_t line_end(std::uint32_t line) const {
        const LineEntry& e = entries[line];
        return entries[line + 1].start - 1 -
               ((e.flags & line_flag::CR) != 0 ? 1u : 0u);
    }
};

/// The InvalidArgument error of a problem: `name:line:col: reason`, or
/// `line:col: reason` for an empty `name`. The line is counted from
/// `first_line`, the line of the text in a larger one.
dstd::Unexpected<dstd::Error> line_problem_error(const LineProblem& problem,
                                                 dstd::StringView name,
                                                 std::uint64_t first_line);

namespace detail {

/// Sets `ix.problem` to the line and column of the byte at `offset` of its text
/// and to `reason`, and returns false. The line is found by counting, so only
/// an error pays for it.
bool fail_at(LineIndex& ix, std::uint32_t offset, dstd::String reason);

/// Forces the scalar Highway target, or the best one again. Tests only.
void set_scalar_line_masks_for_test(bool scalar);

/// The count of '\n' in the text.
std::size_t count_newlines(const char* text, std::size_t size);

/// The bit arrays of `block_count` 64-byte blocks, a bit for each byte.
struct MaskArrays {
    /// '\n'.
    std::uint64_t* nl;
    /// A byte that no line of the subset holds, unless it is a TAB: a control
    /// character other than LF, a CR that is not before a LF, DEL.
    std::uint64_t* bad;
    /// A byte of 0x80 and above.
    std::uint64_t* high;
    /// ' '.
    std::uint64_t* space;
    /// A ':' followed by a space, a LF or a CR.
    std::uint64_t* sep;
    /// A byte that sets a line apart from a plain one: any of `# ' " & * ! ? |
    /// > % @`, a backtick, `[ ] { } ,`, and a byte of 0x80 and above.
    std::uint64_t* special;
};

/// Fills `out`. `text[block_count * 64]` must be readable.
void line_masks(const char* text, std::size_t block_count,
                const MaskArrays& out);

/// Makes every line go through the byte-by-byte classifier. Tests only.
void set_slow_lines_for_test(bool slow);

}  // namespace detail

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_LINE_INDEX_H
