// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_LINE_SCALAR_H
#define DFTRACER_UTILS_YAML_LINE_SCALAR_H

#include <dftracer/utils/yaml/line_index.h>

#include <cstdint>

namespace dftracer::utils::yaml::detail {

enum class Decoded : std::uint8_t {
    Ok,
    /// Valid text that the line index leaves to the tape reader.
    Decline,
    /// Invalid: `EscapeProblem` says how.
    Invalid,
};

struct EscapeProblem {
    enum Kind : std::uint8_t { None, Unknown, Digits, Hex, Range } kind = None;
    std::uint32_t at = 0;
    std::uint32_t line = 0;
    char ch = 0;
    std::uint32_t count = 0;
};

/// Where the text of a decoded scalar is: in the arena of the index, or in
/// its text when `in_text`.
struct ScalarText {
    std::uint32_t off = 0;
    std::uint32_t len = 0;
    bool in_text = false;
};

/// Decodes the inner bytes `[s, e)` of a quoted scalar that one line holds,
/// and appends the text to the arena.
Decoded decode_quoted(LineIndex& ix, std::uint32_t s, std::uint32_t e,
                      bool double_quoted, EscapeProblem& problem,
                      ScalarText& out);

/// A quoted scalar of several lines: its opening quote is at `quote_at` on
/// line `first`, its closing quote at `close_at` on line `last`.
Decoded decode_quoted_lines(LineIndex& ix, std::uint32_t first,
                            std::uint32_t last, std::uint32_t quote_at,
                            std::uint32_t close_at, EscapeProblem& problem,
                            ScalarText& out);

/// A plain scalar of several lines: its first line `first` holds
/// `[first_s, first_e)`, the lines after it up to `last` hold the rest, and
/// the text of `last` ends at `last_e`.
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
