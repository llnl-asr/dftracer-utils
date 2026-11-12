// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_TAPE_H
#define DFTRACER_UTILS_YAML_TAPE_H

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::yaml {

enum class TapeKind : std::uint8_t {
    /// An empty value, `~` or a bare `null`.
    Null,
    Scalar,
    /// A map: its members follow as key, value, key, value, with a scalar for
    /// each key.
    Map,
    Seq,
};

namespace tape_flag {
constexpr std::uint8_t KEY = 1;
/// A quoted scalar: never a null, never a bool or a number by its spelling.
constexpr std::uint8_t QUOTED = 2;
/// The scalar text is in the arena, not in the text of the document.
constexpr std::uint8_t ARENA = 4;
}  // namespace tape_flag

/// One node of a flow collection, 12 bytes. A container is its begin entry and
/// its members after it, with no end entry: `pos` of the begin entry is the
/// tape index just past the last member, so a reader skips a subtree in one
/// step. The offsets and indexes are 32 bits because a text is smaller than
/// 4 GiB and holds at least a byte for each entry.
struct TapeEntry {
    /// A scalar: the byte offset of its text, in the arena when the ARENA flag
    /// is set. A container: the tape index past its last member.
    std::uint32_t pos;
    /// A scalar: its length. A container: unused.
    std::uint32_t size;
    TapeKind kind;
    std::uint8_t flags;

    bool is_container() const {
        return kind == TapeKind::Map || kind == TapeKind::Seq;
    }
};
static_assert(sizeof(TapeEntry) == 12, "a tape entry is 12 bytes");

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_TAPE_H
