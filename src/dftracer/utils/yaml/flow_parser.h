// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_FLOW_PARSER_H
#define DFTRACER_UTILS_YAML_FLOW_PARSER_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/yaml/key_set.h>
#include <dftracer/utils/yaml/line_index.h>
#include <dftracer/utils/yaml/tape.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::yaml {

/// The text of a scalar entry of the tape of `ix`: a view into its text, or
/// into its arena for a scalar that grew while it was read.
inline dstd::StringView tape_text(const LineIndex& ix, const TapeEntry& entry) {
    const char* base =
        (entry.flags & tape_flag::ARENA) != 0 ? ix.arena.data() : ix.text;
    return dstd::StringView(base + entry.pos, entry.size);
}

/// Reads flow collections (YAML 1.2.2, chapter 7: `[a, b]`, `{a: 1}`, JSON) of
/// the text of a @ref LineIndex into its tape, one node at a time. A scalar
/// that the text does not hold as it is (an escape, a folded line) goes to the
/// arena of the index, so the text is never changed. The scratch it keeps is
/// reused from one node to the next.
class FlowParser {
   public:
    explicit FlowParser(LineIndex& ix) : ix_(ix) {}

    /// Reads the flow node that starts at the byte `start`, which holds `[`,
    /// `{`, or a quote, and may go on in later lines. A line after the first
    /// that holds a token must be indented by at least `min_indent` columns.
    /// On success, `end` is the offset after the node and `root` is the tape
    /// index of its first entry. On failure, `ix.problem` says where and why.
    bool parse(std::uint32_t start, std::uint32_t min_indent,
               std::uint32_t& end, std::uint32_t& root);

   private:
    struct Node {
        std::uint32_t index = 0;
        bool is_collection = false;
        /// A quoted scalar or a collection: a `:` right after it separates a
        /// key from its value with no space.
        bool is_json_like = false;
        bool is_multi_line = false;
        bool is_null = false;
    };

    bool fail(std::uint32_t offset, const char* reason);
    bool skip(bool& crossed);
    bool check_line_start(std::uint32_t at);
    bool is_marker(std::uint32_t at) const;
    bool separates(std::uint32_t at) const;
    void push_null();
    std::uint32_t push_scalar(std::uint32_t pos, std::uint32_t size,
                              std::uint8_t flags, TapeKind kind);

    bool node(std::size_t depth, Node& out);
    bool seq(std::size_t depth, Node& out);
    bool seq_entry(std::size_t depth);
    bool map(std::size_t depth, Node& out);
    bool map_entry(std::size_t depth, KeySet::Level& level);
    bool quoted(Node& out);
    bool plain(Node& out);

    LineIndex& ix_;
    const char* t_ = nullptr;
    std::uint32_t size_ = 0;
    std::uint32_t pos_ = 0;
    std::uint32_t min_indent_ = 0;
    KeySet keys_;
    KeySet::Level levels_[MAX_DEPTH];
};

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_FLOW_PARSER_H
