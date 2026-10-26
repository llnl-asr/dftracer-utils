#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/store/engine/section.h>
#include <dftracer/utils/index/store/engine/varint.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace dftracer::utils::index::store::engine {
namespace {

constexpr std::size_t HEADER_BYTES = 20;
constexpr std::size_t MIN_SUBPAGE_INDEX_BYTES = 9;
constexpr std::array<std::uint8_t, 4> SECTION_MAGIC = {'S', 'E', 'C', 'T'};

dstd::Unexpected<dstd::Error> parse_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

int compare_bytes(ByteView a, ByteView b) {
    const std::size_t common = a.size() < b.size() ? a.size() : b.size();
    if (common != 0) {
        const int cmp = std::memcmp(a.data(), b.data(), common);
        if (cmp != 0) return cmp;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

const std::uint8_t* byte_data(ByteView bytes) {
    return reinterpret_cast<const std::uint8_t*>(bytes.data());
}

void append_bytes(dstd::Vector<std::uint8_t>& out, ByteView bytes) {
    if (bytes.empty()) return;
    const auto* begin = byte_data(bytes);
    out.insert(out.end(), begin, begin + bytes.size());
}

ByteView vector_view(const dstd::Vector<std::uint8_t>& bytes,
                     std::size_t offset, std::size_t length) {
    if (length == 0) return {};
    return {bytes.data() + offset, length};
}

std::size_t varint_size(std::uint64_t value) {
    std::size_t size = 1;
    while (value >= 0x80U) {
        value >>= 7U;
        ++size;
    }
    return size;
}

std::size_t encoded_entry_size(ByteView key, ByteView value) {
    const bool big = value.size() > BIG_VALUE_BYTES;
    const std::size_t stored_value_bytes = big ? 20U : value.size();
    const std::uint64_t value_code =
        (static_cast<std::uint64_t>(stored_value_bytes) << 1U) |
        (big ? 1U : 0U);
    return varint_size(key.size()) + varint_size(value_code) + key.size() +
           stored_value_bytes;
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

enum class ValueStorage : std::uint8_t { Arena, Folded };

struct SealedEntry {
    std::size_t key_offset = 0;
    std::size_t key_length = 0;
    std::size_t value_offset = 0;
    std::size_t value_length = 0;
    ValueStorage value_storage = ValueStorage::Arena;
};

}  // namespace

bool SectionView::parse_entry(ByteView bytes, std::size_t& at, std::size_t end,
                              Entry& out) {
    const auto* data = byte_data(bytes);

    const auto key_length = decode_varint(bytes, at);
    if (!key_length || at > end) return false;

    const auto value_code = decode_varint(bytes, at);
    if (!value_code || at > end) return false;
    const bool big = (*value_code & 1U) != 0;
    const std::uint64_t value_length = *value_code >> 1U;

    if (*key_length > static_cast<std::uint64_t>(end - at)) return false;
    const std::size_t key_at = at;
    at += static_cast<std::size_t>(*key_length);
    out.key_ = {data + key_at, static_cast<std::size_t>(*key_length)};

    if (value_length > static_cast<std::uint64_t>(end - at)) return false;

    if (big) {
        if (value_length != 20U) return false;
        out.is_big_ = true;
        out.big_ref_.offset = dftracer::utils::read_u64_le(data + at);
        out.big_ref_.length = dftracer::utils::read_u64_le(data + at + 8U);
        out.big_ref_.crc = dftracer::utils::read_u32_le(data + at + 16U);
        out.value_ = {};
        at += 20U;
        return true;
    }

    out.is_big_ = false;
    out.value_ = {data + at, static_cast<std::size_t>(value_length)};
    out.big_ref_ = {};
    at += static_cast<std::size_t>(value_length);
    return true;
}

ByteView SectionBuilder::view_at(std::size_t offset, std::size_t length) const {
    return vector_view(arena_, offset, length);
}

ByteView SectionBuilder::first_key() const {
    if (entry_count_ == 0) return {};
    return view_at(first_key_offset_, first_key_length_);
}

ByteView SectionBuilder::last_key() const {
    if (entry_count_ == 0) return {};
    return view_at(last_key_offset_, last_key_length_);
}

void SectionBuilder::add(ByteView key, ByteView value) {
    Record record;
    record.key_offset = arena_.size();
    if (!key.empty()) {
        const auto* begin = byte_data(key);
        arena_.insert(arena_.end(), begin, begin + key.size());
    }
    record.key_length = key.size();

    record.value_offset = arena_.size();
    if (!value.empty()) {
        const auto* begin = byte_data(value);
        arena_.insert(arena_.end(), begin, begin + value.size());
    }
    record.value_length = value.size();

    record.ordinal = records_.size();
    records_.push_back(record);
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> SectionBuilder::seal(
    FoldFn fold, PlacerFn placer, void* placer_context) {
    std::sort(records_.begin(), records_.end(),
              [this](const Record& a, const Record& b) {
                  const int cmp =
                      compare_bytes(view_at(a.key_offset, a.key_length),
                                    view_at(b.key_offset, b.key_length));
                  if (cmp != 0) return cmp < 0;
                  return a.ordinal < b.ordinal;
              });

    dstd::Vector<SealedEntry> entries;
    entries.reserve(records_.size());
    dstd::Vector<std::uint8_t> folded_values;
    dstd::Vector<std::uint8_t> fold_out;

    for (std::size_t begin = 0; begin < records_.size();) {
        std::size_t end = begin + 1U;
        while (end < records_.size() &&
               compare_bytes(view_at(records_[begin].key_offset,
                                     records_[begin].key_length),
                             view_at(records_[end].key_offset,
                                     records_[end].key_length)) == 0) {
            ++end;
        }

        SealedEntry entry;
        entry.key_offset = records_[begin].key_offset;
        entry.key_length = records_[begin].key_length;

        if (fold != nullptr && end - begin > 1U) {
            const std::size_t folded_offset = folded_values.size();
            const ByteView first_value = view_at(records_[begin].value_offset,
                                                 records_[begin].value_length);
            append_bytes(folded_values, first_value);
            ByteView accumulated =
                vector_view(folded_values, folded_offset, first_value.size());

            for (std::size_t i = begin + 1U; i < end; ++i) {
                fold_out.clear();
                fold(
                    accumulated,
                    view_at(records_[i].value_offset, records_[i].value_length),
                    fold_out);
                folded_values.resize(folded_offset);
                append_bytes(folded_values,
                             vector_view(fold_out, 0, fold_out.size()));
                accumulated = vector_view(folded_values, folded_offset,
                                          folded_values.size() - folded_offset);
            }

            entry.value_offset = folded_offset;
            entry.value_length = folded_values.size() - folded_offset;
            entry.value_storage = ValueStorage::Folded;
        } else if (fold == nullptr && end - begin > 1U) {
            entry.value_offset = records_[end - 1U].value_offset;
            entry.value_length = records_[end - 1U].value_length;
        } else {
            entry.value_offset = records_[begin].value_offset;
            entry.value_length = records_[begin].value_length;
        }

        entries.push_back(entry);
        begin = end;
    }

    const auto key_of_entry = [this](const SealedEntry& entry) {
        return view_at(entry.key_offset, entry.key_length);
    };
    const auto value_of_entry = [&](const SealedEntry& entry) {
        const dstd::Vector<std::uint8_t>& storage =
            entry.value_storage == ValueStorage::Folded ? folded_values
                                                        : arena_;
        return vector_view(storage, entry.value_offset, entry.value_length);
    };

    if (placer == nullptr) {
        for (const SealedEntry& entry : entries) {
            if (value_of_entry(entry).size() > BIG_VALUE_BYTES) {
                return invalid_error();
            }
        }
    }

    struct Group {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t bytes = 0;
    };

    dstd::Vector<Group> groups;
    std::size_t group_begin = 0;
    std::size_t group_bytes = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::size_t size = encoded_entry_size(key_of_entry(entries[i]),
                                                    value_of_entry(entries[i]));
        if (group_bytes != 0 && group_bytes + size > SUBPAGE_BYTES) {
            groups.push_back({group_begin, i, group_bytes});
            group_begin = i;
            group_bytes = 0;
        }
        group_bytes += size;
    }
    if (!entries.empty()) {
        groups.push_back({group_begin, entries.size(), group_bytes});
    }

    std::size_t index_size = HEADER_BYTES;
    for (const Group& group : groups) {
        const ByteView first = key_of_entry(entries[group.begin]);
        index_size += 8U + varint_size(first.size()) + first.size();
    }

    std::size_t body_bytes = 0;
    for (const Group& group : groups) body_bytes += group.bytes;
    const std::size_t padded = align_up(index_size + body_bytes, SECTION_ALIGN);

    if (padded > std::numeric_limits<std::uint32_t>::max() ||
        entries.size() > std::numeric_limits<std::uint32_t>::max() ||
        groups.size() > std::numeric_limits<std::uint32_t>::max()) {
        return invalid_error();
    }

    dstd::Vector<std::uint8_t> out;
    out.reserve(padded);
    out.insert(out.end(), SECTION_MAGIC.begin(), SECTION_MAGIC.end());
    dftracer::utils::append_u32_le(out, static_cast<std::uint32_t>(padded));
    dftracer::utils::append_u32_le(out,
                                   static_cast<std::uint32_t>(entries.size()));
    dftracer::utils::append_u32_le(out,
                                   static_cast<std::uint32_t>(groups.size()));
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);

    std::size_t subpage_offset = index_size;
    for (const Group& group : groups) {
        const ByteView first = key_of_entry(entries[group.begin]);
        dftracer::utils::append_u32_le(
            out, static_cast<std::uint32_t>(subpage_offset));
        dftracer::utils::append_u32_le(out,
                                       static_cast<std::uint32_t>(group.bytes));
        encode_varint(out, first.size());
        append_bytes(out, first);
        subpage_offset += group.bytes;
    }

    for (const Group& group : groups) {
        for (std::size_t i = group.begin; i < group.end; ++i) {
            const ByteView key = key_of_entry(entries[i]);
            const ByteView value = value_of_entry(entries[i]);
            const bool big = value.size() > BIG_VALUE_BYTES;
            const std::size_t stored_value_bytes = big ? 20U : value.size();
            const std::uint64_t value_code =
                (static_cast<std::uint64_t>(stored_value_bytes) << 1U) |
                (big ? 1U : 0U);
            encode_varint(out, key.size());
            encode_varint(out, value_code);
            append_bytes(out, key);
            if (big) {
                const BigRef ref = placer(placer_context, value);
                dftracer::utils::append_u64_le(out, ref.offset);
                dftracer::utils::append_u64_le(out, ref.length);
                dftracer::utils::append_u32_le(out, ref.crc);
            } else {
                append_bytes(out, value);
            }
        }
    }

    out.resize(padded, 0);

    entry_count_ = entries.size();
    if (entries.empty()) {
        first_key_offset_ = 0;
        first_key_length_ = 0;
        last_key_offset_ = 0;
        last_key_length_ = 0;
    } else {
        first_key_offset_ = entries.front().key_offset;
        first_key_length_ = entries.front().key_length;
        last_key_offset_ = entries.back().key_offset;
        last_key_length_ = entries.back().key_length;
    }
    return out;
}

dstd::Expected<SectionView, dstd::Error> SectionView::open_index(
    ByteView bytes) {
    if (bytes.size() < HEADER_BYTES) return parse_error();
    const auto* data = byte_data(bytes);
    if (std::memcmp(data, SECTION_MAGIC.data(), SECTION_MAGIC.size()) != 0) {
        return parse_error();
    }

    const std::uint32_t section_length =
        dftracer::utils::read_u32_le(data + 4U);
    if (section_length != bytes.size() ||
        (section_length % SECTION_ALIGN) != 0) {
        return parse_error();
    }
    const std::uint32_t entry_count = dftracer::utils::read_u32_le(data + 8U);
    const std::uint32_t subpage_count =
        dftracer::utils::read_u32_le(data + 12U);
    if (data[16] != 0 || data[17] != 0 || data[18] != 0 || data[19] != 0) {
        return parse_error();
    }

    if (subpage_count >
        (bytes.size() - HEADER_BYTES) / MIN_SUBPAGE_INDEX_BYTES) {
        return parse_error();
    }

    std::size_t at = HEADER_BYTES;
    dstd::Vector<Subpage> subpages;
    subpages.reserve(subpage_count);
    for (std::uint32_t i = 0; i < subpage_count; ++i) {
        if (bytes.size() - at < 8U) return parse_error();
        const std::uint32_t offset = dftracer::utils::read_u32_le(data + at);
        at += 4U;
        const std::uint32_t length = dftracer::utils::read_u32_le(data + at);
        at += 4U;

        const auto key_length = decode_varint(bytes, at);
        if (!key_length || at > bytes.size()) return parse_error();
        if (*key_length > bytes.size() - at) return parse_error();
        const ByteView first_key = {data + at,
                                    static_cast<std::size_t>(*key_length)};
        at += static_cast<std::size_t>(*key_length);

        if (offset > bytes.size() || length > bytes.size() - offset) {
            return parse_error();
        }
        subpages.push_back({offset, length, first_key});
    }

    for (std::size_t i = 1; i < subpages.size(); ++i) {
        if (compare_bytes(subpages[i - 1].first_key, subpages[i].first_key) >=
            0) {
            return parse_error();
        }
    }

    if (entry_count == 0 && subpage_count == 0) {
        return SectionView(bytes, std::move(subpages), entry_count);
    }
    if (entry_count == 0 || subpage_count == 0) return parse_error();

    std::size_t expected_offset = at;
    for (const Subpage& subpage : subpages) {
        if (subpage.offset != expected_offset || subpage.length == 0) {
            return parse_error();
        }
        expected_offset = subpage.offset + subpage.length;
    }
    if (expected_offset > bytes.size()) return parse_error();
    for (std::size_t i = expected_offset; i < bytes.size(); ++i) {
        if (data[i] != 0) return parse_error();
    }

    return SectionView(bytes, std::move(subpages), entry_count);
}

bool SectionView::check_entries() const {
    std::uint64_t seen = 0;
    ByteView previous_key;
    bool have_previous = false;
    for (const Subpage& subpage : subpages_) {
        std::size_t entry_at = subpage.offset;
        const std::size_t subpage_end = subpage.offset + subpage.length;
        bool first = true;
        while (entry_at < subpage_end) {
            Entry entry;
            const std::size_t before = entry_at;
            if (!parse_entry(bytes_, entry_at, subpage_end, entry) ||
                entry_at > subpage_end) {
                return false;
            }
            if (first && compare_bytes(entry.key(), subpage.first_key) != 0) {
                return false;
            }
            if (have_previous &&
                compare_bytes(previous_key, entry.key()) >= 0) {
                return false;
            }
            previous_key = entry.key();
            have_previous = true;
            first = false;
            ++seen;
            if (entry_at == before) return false;
        }
        if (first) return false;
    }
    return seen == entry_count_;
}

dstd::Expected<SectionView, dstd::Error> SectionView::open(ByteView bytes) {
    auto view = open_index(bytes);
    if (!view) return dstd::Unexpected<dstd::Error>(view.error());
    if (!view->check_entries()) return parse_error();
    return view;
}

dstd::Optional<SectionView::Entry> SectionView::find(ByteView key) const {
    Cursor cursor = lower_bound(key);
    if (cursor.at_end()) return dstd::Optional<Entry>{};
    Entry entry = cursor.entry();
    if (compare_bytes(entry.key(), key) != 0) return dstd::Optional<Entry>{};
    return entry;
}

SectionView::Cursor SectionView::lower_bound(ByteView key) const {
    if (subpages_.empty()) return end();

    std::uint32_t low = 0;
    std::uint32_t high = static_cast<std::uint32_t>(subpages_.size());
    while (low < high) {
        const std::uint32_t mid = low + (high - low) / 2U;
        if (compare_bytes(subpages_[mid].first_key, key) <= 0) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    const std::uint32_t subpage = low == 0 ? 0 : low - 1U;
    const Subpage& selected = subpages_[subpage];
    const auto* data = byte_data(bytes_);
    std::size_t at = selected.offset;
    const std::size_t subpage_end = selected.offset + selected.length;
    while (at < subpage_end) {
        const std::size_t before = at;
        Entry entry;
        if (!parse_entry(bytes_, at, subpage_end, entry)) return end();
        if (compare_bytes(entry.key(), key) >= 0) {
            return Cursor(this, subpage, data + before);
        }
    }
    if (subpage + 1U < static_cast<std::uint32_t>(subpages_.size())) {
        const Subpage& next = subpages_[subpage + 1U];
        return Cursor(this, subpage + 1U, data + next.offset);
    }
    return end();
}

SectionView::Cursor SectionView::end() const {
    return Cursor(this, static_cast<std::uint32_t>(subpages_.size()), nullptr);
}

SectionView::SectionView(ByteView bytes, dstd::Vector<Subpage> subpages,
                         std::uint64_t entry_count)
    : bytes_(bytes),
      subpages_(std::move(subpages)),
      entry_count_(entry_count) {}

SectionView::Cursor::Cursor(const SectionView* view, std::uint32_t subpage,
                            const std::uint8_t* position)
    : view_(view), subpage_(subpage), position_(position) {
    if (subpage_ < static_cast<std::uint32_t>(view_->subpages_.size())) {
        const auto* data = byte_data(view_->bytes_);
        const Subpage& selected = view_->subpages_[subpage_];
        position_ = position == nullptr ? data + selected.offset : position;
        subpage_end_ = data + selected.offset + selected.length;
    }
}

bool SectionView::Cursor::at_end() const {
    return view_ == nullptr ||
           subpage_ >= static_cast<std::uint32_t>(view_->subpages_.size());
}

SectionView::Entry SectionView::Cursor::entry() const {
    Entry out;
    if (at_end()) return out;
    const auto* data = byte_data(view_->bytes_);
    std::size_t at = static_cast<std::size_t>(position_ - data);
    const std::size_t end = static_cast<std::size_t>(subpage_end_ - data);
    if (!parse_entry(view_->bytes_, at, end, out)) return Entry{};
    return out;
}

void SectionView::Cursor::next() {
    if (at_end()) return;
    const auto* data = byte_data(view_->bytes_);
    std::size_t at = static_cast<std::size_t>(position_ - data);
    const std::size_t end = static_cast<std::size_t>(subpage_end_ - data);
    Entry ignored;
    if (!parse_entry(view_->bytes_, at, end, ignored)) {
        subpage_ = static_cast<std::uint32_t>(view_->subpages_.size());
        position_ = nullptr;
        subpage_end_ = nullptr;
        return;
    }
    position_ = data + at;
    if (position_ == subpage_end_) {
        ++subpage_;
        if (subpage_ < static_cast<std::uint32_t>(view_->subpages_.size())) {
            const Subpage& next = view_->subpages_[subpage_];
            position_ = data + next.offset;
            subpage_end_ = position_ + next.length;
        } else {
            position_ = nullptr;
            subpage_end_ = nullptr;
        }
    }
}

}  // namespace dftracer::utils::index::store::engine
