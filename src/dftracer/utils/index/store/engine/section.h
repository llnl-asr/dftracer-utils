#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_SECTION_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_SECTION_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::store::engine {

inline constexpr std::size_t SUBPAGE_BYTES = 32 * 1024;
inline constexpr std::size_t BIG_VALUE_BYTES = 1U << 20;
inline constexpr std::size_t SECTION_ALIGN = 8;

using ByteView = dftracer::utils::ByteView;

using FoldFn = void (*)(ByteView accumulated, ByteView value,
                        dstd::Vector<std::uint8_t>& out);

struct BigRef {
    std::uint64_t offset;
    std::uint64_t length;
    std::uint32_t crc;
};

using PlacerFn = BigRef (*)(void* context, ByteView value);

class SectionBuilder {
   public:
    void add(ByteView key, ByteView value);

    // Sub-page offsets are relative to the first byte of the section, and the
    // section length includes the padding to SECTION_ALIGN. A big value with a
    // null placer is an error.
    dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> seal(
        FoldFn fold, PlacerFn placer, void* placer_context);

    std::uint64_t entry_count() const { return entry_count_; }
    ByteView first_key() const;
    ByteView last_key() const;

   private:
    struct Record {
        std::size_t key_offset = 0;
        std::size_t key_length = 0;
        std::size_t value_offset = 0;
        std::size_t value_length = 0;
        std::uint64_t ordinal = 0;
    };

    ByteView view_at(std::size_t offset, std::size_t length) const;

    dstd::Vector<Record> records_;
    dstd::Vector<std::uint8_t> arena_;
    std::uint64_t entry_count_ = 0;
    std::size_t first_key_offset_ = 0;
    std::size_t first_key_length_ = 0;
    std::size_t last_key_offset_ = 0;
    std::size_t last_key_length_ = 0;
};

class SectionView {
   public:
    class Entry {
       public:
        ByteView key() const { return key_; }
        bool is_big() const { return is_big_; }
        ByteView value() const { return is_big_ ? ByteView{} : value_; }
        BigRef big_ref() const { return big_ref_; }

       private:
        friend class SectionView;

        ByteView key_;
        ByteView value_;
        BigRef big_ref_{};
        bool is_big_ = false;
    };

    class Cursor {
       public:
        /// At its end, over no section.
        Cursor() = default;

        bool at_end() const;
        Entry entry() const;
        void next();

       private:
        friend class SectionView;

        Cursor(const SectionView* view, std::uint32_t subpage,
               const std::uint8_t* position);

        const SectionView* view_ = nullptr;
        std::uint32_t subpage_ = 0;
        const std::uint8_t* position_ = nullptr;
        const std::uint8_t* subpage_end_ = nullptr;
    };

    /// Opens the header, the subpage index, the bounds of every subpage and the
    /// zero tail of `bytes`, and checks that the first keys of the subpages
    /// ascend, without walking the entries. Cheap enough for a query, which
    /// needs only the subpage holding its key.
    static dstd::Expected<SectionView, dstd::Error> open_index(ByteView bytes);

    /// `open_index` and then `check_entries`: the whole section, every entry of
    /// it walked.
    static dstd::Expected<SectionView, dstd::Error> open(ByteView bytes);

    /// The walk `open` makes after `open_index`, on a view it returned. False
    /// when an entry does not parse, when a subpage does not start with the
    /// first key the index names, when the keys are not strictly ascending, or
    /// when there are not as many of them as the header says.
    bool check_entries() const;

    dstd::Optional<Entry> find(ByteView key) const;
    Cursor lower_bound(ByteView key) const;
    Cursor end() const;

    std::uint64_t entry_count() const { return entry_count_; }

   private:
    struct Subpage {
        std::size_t offset = 0;
        std::size_t length = 0;
        ByteView first_key;
    };

    static bool parse_entry(ByteView bytes, std::size_t& at, std::size_t end,
                            Entry& out);

    SectionView(ByteView bytes, dstd::Vector<Subpage> subpages,
                std::uint64_t entry_count);

    ByteView bytes_;
    dstd::Vector<Subpage> subpages_;
    std::uint64_t entry_count_ = 0;
};

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_SECTION_H
