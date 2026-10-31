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

using ByteView = dftracer::utils::dstd::ByteView;

using FoldFn = void (*)(dstd::ByteView accumulated, dstd::ByteView value,
                        dstd::Vector<std::uint8_t>& out);

struct BigRef {
    std::uint64_t offset;
    std::uint64_t length;
    std::uint32_t crc;
};

using PlacerFn = BigRef (*)(void* context, dstd::ByteView value);

class SegmentBuilder {
   public:
    void add(dstd::ByteView key, dstd::ByteView value);
    /// Adds `key` and `value` without a copy: their bytes must stay valid and
    /// unchanged until seal() returns and first_key() and last_key() are read.
    void add_ref(dstd::ByteView key, dstd::ByteView value);

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
    dstd::ByteView first_key() const;
    dstd::ByteView last_key() const;

   private:
    struct Record {
        dstd::ByteView key;
        dstd::ByteView value;
        std::uint64_t ordinal = 0;
    };

    struct PlannedEntry {
        dstd::ByteView key;
        dstd::ByteView value;
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

    dstd::ByteView keep(dstd::ByteView bytes);
    dstd::ByteView value_of(const PlannedEntry& entry) const;

    dstd::Vector<Record> records_;
    /// Opened by the first add() that copies. add_ref() needs none.
    dstd::UniquePtr<dstd::StringArena> arena_;
    dstd::Vector<PlannedEntry> entries_;
    dstd::Vector<PlannedGroup> groups_;
    dstd::Vector<std::uint8_t> folded_values_;
    std::size_t index_bytes_ = 0;
    std::size_t planned_bytes_ = 0;
    std::uint64_t entry_count_ = 0;
    dstd::ByteView first_key_;
    dstd::ByteView last_key_;
};

class SegmentView {
   public:
    struct GranuleSpan {
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
    };

    class Entry {
       public:
        dstd::ByteView key() const { return key_; }
        bool is_big() const { return is_big_; }
        dstd::ByteView value() const {
            return is_big_ ? dstd::ByteView{} : value_;
        }
        BigRef big_ref() const { return big_ref_; }

       private:
        friend class SegmentView;

        dstd::ByteView key_;
        dstd::ByteView value_;
        BigRef big_ref_{};
        bool is_big_ = false;
    };

    class Cursor {
       public:
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
    static dstd::Expected<SegmentView, dstd::Error> open_index(
        dstd::ByteView bytes);

    /// `open_index` and then `check_entries`: the whole segment, every entry of
    /// it walked.
    static dstd::Expected<SegmentView, dstd::Error> open(dstd::ByteView bytes);

    /// The walk `open` makes after `open_index`, on a view it returned. False
    /// when an entry does not parse, when a granule does not start with the
    /// first key the index names, when the keys are not strictly ascending, or
    /// when there are not as many of them as the header says.
    bool check_entries() const;

    dstd::Optional<Entry> find(dstd::ByteView key) const;
    Cursor lower_bound(dstd::ByteView key) const;
    Cursor end() const;

    std::uint64_t entry_count() const { return entry_count_; }

    std::size_t granule_count() const { return granules_.size(); }

    GranuleSpan granule(std::size_t i) const;

    /// The byte length of the header and index at the front of `prefix`, walked
    /// in place. Zero when `prefix` is shorter, is not a segment header, or an
    /// index entry does not parse. A segment of no granules gives the header
    /// alone.
    static std::uint32_t head_length(dstd::ByteView prefix);

    /// The granule of the segment (`head` holds its header and index) that can
    /// hold `key`: the last one whose first key is at most it. Nullopt for a
    /// key below the first granule or a segment of no granules. A Parse error
    /// when the header does not match `segment_length` or the granules are
    /// malformed.
    static dstd::Expected<dstd::Optional<GranuleSpan>, dstd::Error>
    find_granule(dstd::ByteView head, std::uint32_t segment_length,
                 dstd::ByteView key);

    /// The header and granule index `head` of a segment of `segment_length`,
    /// with no entry bytes. Its first keys point into `head`. It checks what
    /// open_index checks except the zero tail. Use it for range_span and
    /// with_window only.
    static dstd::Expected<SegmentView, dstd::Error> open_head(
        dstd::ByteView head, std::uint32_t segment_length);

    /// The granules of a view from open_head that can hold a key in
    /// [begin, end), as one span. An empty `end` is unbounded. The view must
    /// have a granule.
    GranuleSpan range_span(dstd::ByteView begin, dstd::ByteView end) const;

    /// A view of the granules of a view from open_head that `window` holds,
    /// the bytes at `window_offset` of the segment. `window` and the head must
    /// outlive it. A Parse error when `window` does not start and end on
    /// granule bounds.
    dstd::Expected<SegmentView, dstd::Error> with_window(
        dstd::ByteView window, std::uint32_t window_offset) &&;

    /// The entry of `key` among the bytes of one granule, which the returned
    /// entry points into, or nullopt when the granule does not hold it. A Parse
    /// error when an entry does not parse or runs past the granule.
    static dstd::Expected<dstd::Optional<Entry>, dstd::Error> find_in_granule(
        dstd::ByteView granule, dstd::ByteView key);

   private:
    struct Granule {
        std::size_t offset = 0;
        std::size_t length = 0;
        dstd::ByteView first_key;
    };

    static bool parse_entry(dstd::ByteView bytes, std::size_t& at,
                            std::size_t end, Entry& out);

    SegmentView(dstd::ByteView bytes, dstd::Vector<Granule> granules,
                std::uint64_t entry_count);

    dstd::ByteView bytes_;
    dstd::Vector<Granule> granules_;
    std::uint64_t entry_count_ = 0;
};

/// Appends the spans of one sealed segment that starts at `offset` in an
/// extent, for `page_starts`: the header and index as one span marked alone,
/// then one per granule, the last ending at the segment end. False when the
/// segment does not open.
bool append_segment_spans(dstd::ByteView segment, std::uint32_t offset,
                          dstd::Vector<SegmentSpan>& out);

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_SEGMENT_H
