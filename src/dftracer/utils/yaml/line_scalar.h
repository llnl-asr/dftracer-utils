// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_LINE_SCALAR_H
#define DFTRACER_UTILS_YAML_LINE_SCALAR_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/yaml/line_index.h>

#include <cstdint>

namespace dftracer::utils::yaml::detail {

enum class Decoded : std::uint8_t {
    Ok,
    /// Invalid: `EscapeProblem` says how.
    Invalid,
};

struct EscapeProblem {
    enum Kind : std::uint8_t { None, Unknown, Digits, Hex, Range } kind = None;
    /// The offset of the backslash in the text.
    std::uint32_t at = 0;
    char ch = 0;
    std::uint32_t count = 0;
};

/// The reason that an escape is invalid, for an error message.
dstd::String escape_reason(const EscapeProblem& problem);

/// Where the text of a decoded scalar is: in the arena of the index, or in
/// its text when `in_text`.
struct ScalarText {
    std::uint32_t off = 0;
    std::uint32_t len = 0;
    bool in_text = false;
};

/// Decodes the inner bytes `[s, e)` of a quoted scalar, which may hold line
/// breaks, and appends the text to the arena.
Decoded decode_quoted(LineIndex& ix, std::uint32_t s, std::uint32_t e,
                      bool double_quoted, EscapeProblem& problem,
                      ScalarText& out);

/// Appends the text of the plain scalar that lies in `[s, e)` of the text,
/// which may hold line breaks, to the arena. `e` is the end of its last line of
/// text.
void decode_plain(LineIndex& ix, std::uint32_t s, std::uint32_t e,
                  ScalarText& out);

/// @ref decode_plain for the lines `first` to `last` of the line index. The
/// first text of the first line is `[first_s, first_e)`; the last line ends its
/// text at `last_e`.
void decode_plain_lines(LineIndex& ix, std::uint32_t first, std::uint32_t last,
                        std::uint32_t first_s, std::uint32_t first_e,
                        std::uint32_t last_e, ScalarText& out);

/// The facts of a block scalar that its decoding needs.
struct BlockSpec {
    bool is_literal = true;
    /// -1 for strip, 0 for clip, 1 for keep.
    std::int8_t chomp = 0;
    /// False when no line of the scalar holds text, so no indent is known.
    bool is_indent_known = false;
    std::uint32_t indent = 0;
};

/// A block scalar whose lines are `[first, end)`.
Decoded decode_block(LineIndex& ix, const BlockSpec& spec, std::uint32_t first,
                     std::uint32_t end, ScalarText& out);

}  // namespace dftracer::utils::yaml::detail

#endif  // DFTRACER_UTILS_YAML_LINE_SCALAR_H
