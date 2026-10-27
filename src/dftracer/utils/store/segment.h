#ifndef DFTRACER_UTILS_STORE_SEGMENT_H
#define DFTRACER_UTILS_STORE_SEGMENT_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/extent_codec.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::store {

inline constexpr std::size_t GRANULE_BYTES = 32 * 1024;
inline constexpr std::size_t BIG_VALUE_BYTES = 1U << 20;
inline constexpr std::size_t SEGMENT_ALIGN = 8;

using ByteView = dftracer::utils::ByteView;

using FoldFn = void (*)(ByteView accumulated, ByteView value,
                        dstd::Vector<std::uint8_t>& out);

struct BigRef {
    std::uint64_t offset;
    std::uint64_t length;
    std::uint32_t crc;
};

using PlacerFn = BigRef (*)(void* context, ByteView value);

class SegmentBuilder {
   public:
    /// Copies `key` and `value` into the builder.
    void add(ByteView key, ByteView value);
    /// Adds `key` and `value` without a copy: their bytes must stay valid and
    /// unchanged until seal() returns and first_key() and last_key() are read.
    void add_ref(ByteView key, ByteView value);

    // Granule offsets are relative to the first byte of the segment, and the
    // segment length includes the padding to SEGMENT_ALIGN. A big value with a
    // null placer is an error.
    dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> seal(
        FoldFn fold, PlacerFn placer, void* placer_context);

    /// The first half of seal(): sorts and folds the entries, places every big
    /// value through `placer` and returns the exact bytes write_to() writes.
    /// The same errors as seal().
    dstd::Expected<std::size_t, dstd::Error> plan(FoldFn fold, PlacerFn placer,
                                                  void* placer_context);
    /// The second half: writes the segment plan() measured to `out`, which
    /// has room for that many bytes. Never fails.
    void write_to(std::uint8_t* out) const;

    std::uint64_t entry_count() const { return entry_count_; }
    ByteView first_key() const;
    ByteView last_key() const;

   private:
    struct Record {
        ByteView key;
        ByteView value;
        std::uint64_t ordinal = 0;
    };

    /// One entry after plan(): its key, and its value in place, or in the
    /// folded values, or as a big run.
    struct PlannedEntry {
        ByteView key;
        ByteView value;
        std::size_t value_offset = 0;
        std::size_t value_length = 0;
        BigRef big{};
        bool folded = false;
        bool is_big = false;
    };
    struct PlannedGroup {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t bytes = 0;
    };

    /// `bytes` copied into the builder's arena, which never moves them.
    ByteView keep(ByteView bytes);
    ByteView value_of(const PlannedEntry& entry) const;

    dstd::Vector<Record> records_;
    /// Opened by the first add() that copies; add_ref() needs none. Behind a
    /// pointer, so the builder moves.
    dstd::UniquePtr<StringArena> arena_;
    dstd::Vector<PlannedEntry> entries_;
    dstd::Vector<PlannedGroup> groups_;
    dstd::Vector<std::uint8_t> folded_values_;
    std::size_t index_bytes_ = 0;
    std::size_t planned_bytes_ = 0;
    std::uint64_t entry_count_ = 0;
    ByteView first_key_;
    ByteView last_key_;
};

class SegmentView {
   public:
    /// The bytes of one granule within its segment.
    struct GranuleSpan {
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
    };

    class Entry {
       public:
        ByteView key() const { return key_; }
        bool is_big() const { return is_big_; }
        ByteView value() const { return is_big_ ? ByteView{} : value_; }
        BigRef big_ref() const { return big_ref_; }

       private:
        friend class SegmentView;

        ByteView key_;
        ByteView value_;
        BigRef big_ref_{};
        bool is_big_ = false;
    };

    class Cursor {
       public:
        /// At its end, over no segment.
        Cursor() = default;

        bool at_end() const;
        Entry entry() const;
        void next();

       private:
        friend class SegmentView;

        Cursor(const SegmentView* view, std::uint32_t granule,
               const std::uint8_t* position);

        const SegmentView* view_ = nullptr;
        std::uint32_t granule_ = 0;
        const std::uint8_t* position_ = nullptr;
        const std::uint8_t* granule_end_ = nullptr;
    };

    /// Opens the header, the granule index, the bounds of every granule and the
    /// zero tail of `bytes`, and checks that the first keys of the granules
    /// ascend, without walking the entries. Cheap enough for a query, which
    /// needs only the granule holding its key.
    static dstd::Expected<SegmentView, dstd::Error> open_index(ByteView bytes);

    /// `open_index` and then `check_entries`: the whole segment, every entry of
    /// it walked.
    static dstd::Expected<SegmentView, dstd::Error> open(ByteView bytes);

    /// The walk `open` makes after `open_index`, on a view it returned. False
    /// when an entry does not parse, when a granule does not start with the
    /// first key the index names, when the keys are not strictly ascending, or
    /// when there are not as many of them as the header says.
    bool check_entries() const;

    dstd::Optional<Entry> find(ByteView key) const;
    Cursor lower_bound(ByteView key) const;
    Cursor end() const;

    std::uint64_t entry_count() const { return entry_count_; }

    std::size_t granule_count() const { return granules_.size(); }

    /// The bytes of granule `i` within the segment, for `i` below
    /// granule_count().
    GranuleSpan granule(std::size_t i) const;

    /// The byte length of the header and the index that follow it in `prefix`,
    /// walked in place and allocated nothing. Zero when `prefix` is shorter
    /// than they are, when the header is not a segment header, or when an index
    /// entry does not parse. A segment of no granules answers the header
    /// length alone.
    static std::uint32_t head_length(ByteView prefix);

    /// The granule of the segment whose bytes `head` holds, the header and the
    /// index of a segment of `segment_length` bytes, that can hold `key`: the
    /// last one whose first key is at most it. Nullopt for a key below the
    /// first granule's first key and for a segment of no granules. A Parse
    /// error when the header is not one of a segment of `segment_length`, or
    /// when the granules are not contiguous from the end of the index, empty,
    /// ascending by first key and inside the segment.
    static dstd::Expected<dstd::Optional<GranuleSpan>, dstd::Error>
    find_granule(ByteView head, std::uint32_t segment_length, ByteView key);

    /// The entry of `key` among the bytes of one granule, which the returned
    /// entry points into, or nullopt when the granule does not hold it. A Parse
    /// error when an entry does not parse or runs past the granule.
    static dstd::Expected<dstd::Optional<Entry>, dstd::Error> find_in_granule(
        ByteView granule, ByteView key);

   private:
    struct Granule {
        std::size_t offset = 0;
        std::size_t length = 0;
        ByteView first_key;
    };

    static bool parse_entry(ByteView bytes, std::size_t& at, std::size_t end,
                            Entry& out);

    SegmentView(ByteView bytes, dstd::Vector<Granule> granules,
                std::uint64_t entry_count);

    ByteView bytes_;
    dstd::Vector<Granule> granules_;
    std::uint64_t entry_count_ = 0;
};

/// Appends the spans of one sealed segment that starts at `offset` in an
/// extent, for `page_starts`: the header and the index as one span marked
/// alone, then one span per granule with the last one ending at the segment
/// end, so the padding goes with it. A segment of at most PAGE_BYTES gives one
/// span. False when the segment does not open.
bool append_segment_spans(ByteView segment, std::uint32_t offset,
                          dstd::Vector<SegmentSpan>& out);

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_SEGMENT_H
