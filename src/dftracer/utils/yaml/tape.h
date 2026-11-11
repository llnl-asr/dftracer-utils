// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_TAPE_H
#define DFTRACER_UTILS_YAML_TAPE_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/raw_buffer.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/yaml/line_index.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::yaml {

enum class TapeKind : std::uint8_t {
    /// An empty value, `~` or a bare `null`.
    Null,
    Scalar,
    /// A map: its members follow as key, value, key, value, with a scalar or
    /// an alias for each key.
    Map,
    Seq,
    /// An alias to an earlier node: `pos` is the tape index of that node.
    Alias,
};

namespace tape_flag {
constexpr std::uint8_t KEY = 1;
/// A quoted scalar: never a null, never a bool or a number by its spelling.
constexpr std::uint8_t QUOTED = 2;
constexpr std::uint8_t BLOCK = 4;
/// The scalar text is in the arena, not in the text of the document.
constexpr std::uint8_t ARENA = 8;
/// On a key: a plain `<<`. On a map: it holds one.
constexpr std::uint8_t MERGE = 16;
}  // namespace tape_flag

/// One node of a parsed text, 16 bytes. A container is its begin entry and its
/// members after it, with no end entry: `pos` of the begin entry is the tape
/// index just past the last member, so a reader skips a subtree in one step.
struct TapeEntry {
    /// A scalar: the byte offset of its text, in the arena when the ARENA flag
    /// is set. A container: the tape index past its last member, 0 while the
    /// container is open. An alias: the tape index of its target.
    std::uint64_t pos;
    /// A scalar: its length. A container or an alias: the nodes a walk visits
    /// below and including it, with every alias counted as what it names.
    /// 48 bits: a low and a high part.
    std::uint32_t size_lo;
    std::uint16_t size_hi;
    TapeKind kind;
    std::uint8_t flags;

    std::uint64_t size() const {
        return size_lo | (static_cast<std::uint64_t>(size_hi) << 32);
    }
    void set_size(std::uint64_t n) {
        size_lo = static_cast<std::uint32_t>(n);
        size_hi = static_cast<std::uint16_t>(n >> 32);
    }
    bool is_container() const {
        return kind == TapeKind::Map || kind == TapeKind::Seq;
    }
};
static_assert(sizeof(TapeEntry) == 16, "a tape entry is 16 bytes");

/// The largest scalar length and node count a tape entry holds.
constexpr std::uint64_t TAPE_SIZE_MAX = (std::uint64_t{1} << 48) - 1;

/// What a parse leaves: the text it read in place, the tape over it and the
/// arena for the scalars that grew while rapidyaml filtered them.
struct DocumentStorage {
    /// The buffer a Document owns. Empty when the text lives elsewhere.
    dstd::String text;
    /// The parsed text: `text`, or a buffer a Records object holds.
    const char* base = nullptr;
    std::size_t size = 0;
    dstd::RawBuffer<TapeEntry> tape;
    dstd::Vector<char> arena;
    std::size_t arena_used = 0;
    /// The line index, when the text is in the accepted subset of the line
    /// index: the tape is empty then.
    LineIndex lines;
    bool is_lines = false;

    void reset_tape() {
        tape.clear();
        arena_used = 0;
    }
};

/// The text of a scalar entry.
inline dstd::StringView entry_text(const DocumentStorage& storage,
                                   const TapeEntry& entry) {
    const char* base = (entry.flags & tape_flag::ARENA) != 0
                           ? storage.arena.data()
                           : storage.base;
    return dstd::StringView(base + entry.pos, entry.size());
}

/// The text of the key entry at tape index `index`: an alias reads as the
/// scalar it names, and a null key is empty.
inline dstd::StringView key_text(const DocumentStorage& storage,
                                 std::size_t index) {
    const TapeEntry* entry = &storage.tape[index];
    if (entry->kind == TapeKind::Alias) entry = &storage.tape[entry->pos];
    if (entry->kind != TapeKind::Scalar) return {};
    return entry_text(storage, *entry);
}

struct TapeParserImpl;

/// Parses YAML text in place into a tape. It keeps its scratch buffers between
/// parses, so a loop of chunks allocates nothing after the first.
class TapeParser {
   public:
    TapeParser();
    TapeParser(const TapeParser&) = delete;
    TapeParser& operator=(const TapeParser&) = delete;
    ~TapeParser();

    /// Parses the buffer `storage.base` with `storage.size` bytes, which it
    /// rewrites, and fills `storage.tape`: one top node for each YAML document
    /// of the text, and none for a text without a document. A failure is an
    /// InvalidArgument error `source_name:line:column: reason`, or
    /// `line:column: reason` for an empty `source_name`, and the line counts
    /// from `first_line`.
    dstd::Result<void> parse(DocumentStorage& storage,
                             dstd::StringView source_name,
                             std::uint64_t first_line = 1);

   private:
    dstd::UniquePtr<TapeParserImpl> impl_;
};

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_TAPE_H
