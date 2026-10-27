#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/store/segment.h>
#include <dftracer/utils/store/varint.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace dftracer::utils::store {
namespace {

constexpr std::size_t HEADER_BYTES = 20;
constexpr std::size_t MIN_GRANULE_INDEX_BYTES = 9;
constexpr std::array<std::uint8_t, 4> SEGMENT_MAGIC = {'S', 'E', 'C', 'T'};

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

}  // namespace

bool SegmentView::parse_entry(ByteView bytes, std::size_t& at, std::size_t end,
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

ByteView SegmentBuilder::first_key() const {
    if (entry_count_ == 0) return {};
    return first_key_;
}

ByteView SegmentBuilder::last_key() const {
    if (entry_count_ == 0) return {};
    return last_key_;
}

ByteView SegmentBuilder::keep(ByteView bytes) {
    if (bytes.empty()) return {};
    if (!arena_) arena_ = dstd::make_unique<StringArena>();
    const std::string_view kept =
        arena_->push(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return {reinterpret_cast<const std::byte*>(kept.data()), kept.size()};
}

void SegmentBuilder::add(ByteView key, ByteView value) {
    const ByteView kept_key = keep(key);
    add_ref(kept_key, keep(value));
}

void SegmentBuilder::add_ref(ByteView key, ByteView value) {
    records_.push_back(Record{key, value, records_.size()});
}

dstd::Expected<std::size_t, dstd::Error> SegmentBuilder::plan(
    FoldFn fold, PlacerFn placer, void* placer_context) {
    const auto before = [](const Record& a, const Record& b) {
        const int cmp = compare_bytes(a.key, b.key);
        if (cmp != 0) return cmp < 0;
        return a.ordinal < b.ordinal;
    };
    // A caller that adds in key order, as the writer does, needs no sort.
    if (!std::is_sorted(records_.begin(), records_.end(), before)) {
        std::sort(records_.begin(), records_.end(), before);
    }

    entries_.clear();
    groups_.clear();
    folded_values_.clear();
    entries_.reserve(records_.size());
    dstd::Vector<std::uint8_t> fold_out;

    for (std::size_t begin = 0; begin < records_.size();) {
        std::size_t end = begin + 1U;
        while (end < records_.size() &&
               compare_bytes(records_[begin].key, records_[end].key) == 0) {
            ++end;
        }

        PlannedEntry entry;
        entry.key = records_[begin].key;

        if (fold != nullptr && end - begin > 1U) {
            const std::size_t folded_offset = folded_values_.size();
            const ByteView first_value = records_[begin].value;
            append_bytes(folded_values_, first_value);
            ByteView accumulated =
                vector_view(folded_values_, folded_offset, first_value.size());

            for (std::size_t i = begin + 1U; i < end; ++i) {
                fold_out.clear();
                fold(accumulated, records_[i].value, fold_out);
                folded_values_.resize(folded_offset);
                append_bytes(folded_values_,
                             vector_view(fold_out, 0, fold_out.size()));
                accumulated =
                    vector_view(folded_values_, folded_offset,
                                folded_values_.size() - folded_offset);
            }

            entry.value_offset = folded_offset;
            entry.value_length = folded_values_.size() - folded_offset;
            entry.folded = true;
        } else if (fold == nullptr && end - begin > 1U) {
            entry.value = records_[end - 1U].value;
        } else {
            entry.value = records_[begin].value;
        }

        entries_.push_back(entry);
        begin = end;
    }

    // Each big value goes to its own run now, so write_to() only writes.
    for (PlannedEntry& entry : entries_) {
        const ByteView value = value_of(entry);
        if (value.size() <= BIG_VALUE_BYTES) continue;
        if (placer == nullptr) return invalid_error();
        entry.big = placer(placer_context, value);
        entry.is_big = true;
    }

    std::size_t group_begin = 0;
    std::size_t group_bytes = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const std::size_t size =
            encoded_entry_size(entries_[i].key, value_of(entries_[i]));
        if (group_bytes != 0 && group_bytes + size > GRANULE_BYTES) {
            groups_.push_back({group_begin, i, group_bytes});
            group_begin = i;
            group_bytes = 0;
        }
        group_bytes += size;
    }
    if (!entries_.empty()) {
        groups_.push_back({group_begin, entries_.size(), group_bytes});
    }

    index_bytes_ = HEADER_BYTES;
    for (const PlannedGroup& group : groups_) {
        const ByteView first = entries_[group.begin].key;
        index_bytes_ += 8U + varint_size(first.size()) + first.size();
    }

    std::size_t body_bytes = 0;
    for (const PlannedGroup& group : groups_) body_bytes += group.bytes;
    planned_bytes_ = align_up(index_bytes_ + body_bytes, SEGMENT_ALIGN);

    if (planned_bytes_ > std::numeric_limits<std::uint32_t>::max() ||
        entries_.size() > std::numeric_limits<std::uint32_t>::max() ||
        groups_.size() > std::numeric_limits<std::uint32_t>::max()) {
        return invalid_error();
    }

    entry_count_ = entries_.size();
    if (entries_.empty()) {
        first_key_ = {};
        last_key_ = {};
    } else {
        first_key_ = entries_.front().key;
        last_key_ = entries_.back().key;
    }
    return planned_bytes_;
}

ByteView SegmentBuilder::value_of(const PlannedEntry& entry) const {
    if (entry.folded) {
        return vector_view(folded_values_, entry.value_offset,
                           entry.value_length);
    }
    return entry.value;
}

void SegmentBuilder::write_to(std::uint8_t* out) const {
    std::uint8_t* at = out;
    const auto put_bytes = [&at](ByteView bytes) {
        if (bytes.empty()) return;
        std::memcpy(at, bytes.data(), bytes.size());
        at += bytes.size();
    };
    const auto put_u32 = [&at](std::uint32_t value) {
        dftracer::utils::write_u32_le(at, value);
        at += 4;
    };
    const auto put_varint = [&at](std::uint64_t value) {
        while (value >= 0x80U) {
            *at++ = static_cast<std::uint8_t>(value | 0x80U);
            value >>= 7U;
        }
        *at++ = static_cast<std::uint8_t>(value);
    };

    put_bytes(ByteView{reinterpret_cast<const std::byte*>(SEGMENT_MAGIC.data()),
                       SEGMENT_MAGIC.size()});
    put_u32(static_cast<std::uint32_t>(planned_bytes_));
    put_u32(static_cast<std::uint32_t>(entries_.size()));
    put_u32(static_cast<std::uint32_t>(groups_.size()));
    put_u32(0);

    std::size_t granule_offset = index_bytes_;
    for (const PlannedGroup& group : groups_) {
        const ByteView first = entries_[group.begin].key;
        put_u32(static_cast<std::uint32_t>(granule_offset));
        put_u32(static_cast<std::uint32_t>(group.bytes));
        put_varint(first.size());
        put_bytes(first);
        granule_offset += group.bytes;
    }

    for (const PlannedGroup& group : groups_) {
        for (std::size_t i = group.begin; i < group.end; ++i) {
            const PlannedEntry& entry = entries_[i];
            const ByteView value = value_of(entry);
            const std::size_t stored_value_bytes =
                entry.is_big ? 20U : value.size();
            const std::uint64_t value_code =
                (static_cast<std::uint64_t>(stored_value_bytes) << 1U) |
                (entry.is_big ? 1U : 0U);
            put_varint(entry.key.size());
            put_varint(value_code);
            put_bytes(entry.key);
            if (entry.is_big) {
                dftracer::utils::write_u64_le(at, entry.big.offset);
                dftracer::utils::write_u64_le(at + 8, entry.big.length);
                dftracer::utils::write_u32_le(at + 16, entry.big.crc);
                at += 20;
            } else {
                put_bytes(value);
            }
        }
    }
    const std::size_t written = static_cast<std::size_t>(at - out);
    if (written < planned_bytes_) {
        std::memset(at, 0, planned_bytes_ - written);
    }
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> SegmentBuilder::seal(
    FoldFn fold, PlacerFn placer, void* placer_context) {
    auto planned = plan(fold, placer, placer_context);
    if (!planned) return dstd::Unexpected<dstd::Error>(planned.error());
    dstd::Vector<std::uint8_t> out(*planned);
    write_to(out.data());
    return out;
}

dstd::Expected<SegmentView, dstd::Error> SegmentView::open_index(
    ByteView bytes) {
    if (bytes.size() < HEADER_BYTES) return parse_error();
    const auto* data = byte_data(bytes);
    if (std::memcmp(data, SEGMENT_MAGIC.data(), SEGMENT_MAGIC.size()) != 0) {
        return parse_error();
    }

    const std::uint32_t segment_length =
        dftracer::utils::read_u32_le(data + 4U);
    if (segment_length != bytes.size() ||
        (segment_length % SEGMENT_ALIGN) != 0) {
        return parse_error();
    }
    const std::uint32_t entry_count = dftracer::utils::read_u32_le(data + 8U);
    const std::uint32_t granule_count =
        dftracer::utils::read_u32_le(data + 12U);
    if (data[16] != 0 || data[17] != 0 || data[18] != 0 || data[19] != 0) {
        return parse_error();
    }

    if (granule_count >
        (bytes.size() - HEADER_BYTES) / MIN_GRANULE_INDEX_BYTES) {
        return parse_error();
    }

    std::size_t at = HEADER_BYTES;
    dstd::Vector<Granule> granules;
    granules.reserve(granule_count);
    for (std::uint32_t i = 0; i < granule_count; ++i) {
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
        granules.push_back({offset, length, first_key});
    }

    for (std::size_t i = 1; i < granules.size(); ++i) {
        if (compare_bytes(granules[i - 1].first_key, granules[i].first_key) >=
            0) {
            return parse_error();
        }
    }

    if (entry_count == 0 && granule_count == 0) {
        return SegmentView(bytes, std::move(granules), entry_count);
    }
    if (entry_count == 0 || granule_count == 0) return parse_error();

    std::size_t expected_offset = at;
    for (const Granule& granule : granules) {
        if (granule.offset != expected_offset || granule.length == 0) {
            return parse_error();
        }
        expected_offset = granule.offset + granule.length;
    }
    if (expected_offset > bytes.size()) return parse_error();
    for (std::size_t i = expected_offset; i < bytes.size(); ++i) {
        if (data[i] != 0) return parse_error();
    }

    return SegmentView(bytes, std::move(granules), entry_count);
}

bool SegmentView::check_entries() const {
    std::uint64_t seen = 0;
    ByteView previous_key;
    bool have_previous = false;
    for (const Granule& granule : granules_) {
        std::size_t entry_at = granule.offset;
        const std::size_t granule_end = granule.offset + granule.length;
        bool first = true;
        while (entry_at < granule_end) {
            Entry entry;
            const std::size_t before = entry_at;
            if (!parse_entry(bytes_, entry_at, granule_end, entry) ||
                entry_at > granule_end) {
                return false;
            }
            if (first && compare_bytes(entry.key(), granule.first_key) != 0) {
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

dstd::Expected<SegmentView, dstd::Error> SegmentView::open(ByteView bytes) {
    auto view = open_index(bytes);
    if (!view) return dstd::Unexpected<dstd::Error>(view.error());
    if (!view->check_entries()) return parse_error();
    return view;
}

dstd::Optional<SegmentView::Entry> SegmentView::find(ByteView key) const {
    Cursor cursor = lower_bound(key);
    if (cursor.at_end()) return dstd::Optional<Entry>{};
    Entry entry = cursor.entry();
    if (compare_bytes(entry.key(), key) != 0) return dstd::Optional<Entry>{};
    return entry;
}

SegmentView::GranuleSpan SegmentView::granule(std::size_t i) const {
    const Granule& held = granules_[i];
    return {static_cast<std::uint32_t>(held.offset),
            static_cast<std::uint32_t>(held.length)};
}

std::uint32_t SegmentView::head_length(ByteView prefix) {
    if (prefix.size() < HEADER_BYTES) return 0;
    const auto* data = byte_data(prefix);
    if (std::memcmp(data, SEGMENT_MAGIC.data(), SEGMENT_MAGIC.size()) != 0) {
        return 0;
    }
    if (data[16] != 0 || data[17] != 0 || data[18] != 0 || data[19] != 0) {
        return 0;
    }
    const std::uint32_t granule_count =
        dftracer::utils::read_u32_le(data + 12U);
    std::size_t at = HEADER_BYTES;
    for (std::uint32_t i = 0; i < granule_count; ++i) {
        if (prefix.size() - at < 8U) return 0;
        at += 8U;
        const auto key_length = decode_varint(prefix, at);
        if (!key_length || at > prefix.size()) return 0;
        if (*key_length > prefix.size() - at) return 0;
        at += static_cast<std::size_t>(*key_length);
    }
    return static_cast<std::uint32_t>(at);
}

dstd::Expected<dstd::Optional<SegmentView::GranuleSpan>, dstd::Error>
SegmentView::find_granule(ByteView head, std::uint32_t segment_length,
                          ByteView key) {
    if (head.size() < HEADER_BYTES) return parse_error();
    const auto* data = byte_data(head);
    if (std::memcmp(data, SEGMENT_MAGIC.data(), SEGMENT_MAGIC.size()) != 0) {
        return parse_error();
    }
    if (dftracer::utils::read_u32_le(data + 4U) != segment_length) {
        return parse_error();
    }
    if (data[16] != 0 || data[17] != 0 || data[18] != 0 || data[19] != 0) {
        return parse_error();
    }
    const std::uint32_t granule_count =
        dftracer::utils::read_u32_le(data + 12U);

    std::size_t at = HEADER_BYTES;
    std::uint32_t first_offset = 0;
    std::uint32_t expected_offset = 0;
    ByteView previous_first;
    bool have_previous = false;
    dstd::Optional<GranuleSpan> found;
    for (std::uint32_t i = 0; i < granule_count; ++i) {
        if (head.size() - at < 8U) return parse_error();
        const std::uint32_t offset = dftracer::utils::read_u32_le(data + at);
        const std::uint32_t length =
            dftracer::utils::read_u32_le(data + at + 4U);
        at += 8U;
        const auto key_length = decode_varint(head, at);
        if (!key_length || at > head.size()) return parse_error();
        if (*key_length > head.size() - at) return parse_error();
        const ByteView first_key = {data + at,
                                    static_cast<std::size_t>(*key_length)};
        at += static_cast<std::size_t>(*key_length);

        if (i == 0) first_offset = offset;
        if (length == 0 || offset > segment_length ||
            length > segment_length - offset) {
            return parse_error();
        }
        if (i != 0 && offset != expected_offset) return parse_error();
        expected_offset = offset + length;
        if (have_previous && compare_bytes(previous_first, first_key) >= 0) {
            return parse_error();
        }
        previous_first = first_key;
        have_previous = true;
        if (compare_bytes(first_key, key) <= 0) {
            found = GranuleSpan{offset, length};
        }
    }
    if (at != head.size()) return parse_error();
    if (granule_count != 0 && first_offset != at) return parse_error();
    return found;
}

dstd::Expected<dstd::Optional<SegmentView::Entry>, dstd::Error>
SegmentView::find_in_granule(ByteView granule, ByteView key) {
    std::size_t at = 0;
    while (at < granule.size()) {
        Entry entry;
        if (!parse_entry(granule, at, granule.size(), entry) ||
            at > granule.size()) {
            return parse_error();
        }
        const int cmp = compare_bytes(entry.key(), key);
        if (cmp == 0) return dstd::Optional<Entry>(entry);
        if (cmp > 0) break;
    }
    return dstd::Optional<Entry>{};
}

bool append_segment_spans(ByteView segment, std::uint32_t offset,
                          dstd::Vector<SegmentSpan>& out) {
    auto opened = SegmentView::open_index(segment);
    if (!opened) return false;
    const std::uint32_t length = static_cast<std::uint32_t>(segment.size());
    if (static_cast<std::uint64_t>(offset) + length >
        std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const SegmentView& view = *opened;
    if (length <= PAGE_BYTES || view.granule_count() == 0) {
        out.push_back({offset, length});
        return true;
    }
    out.push_back({offset, view.granule(0).offset, true});
    for (std::size_t i = 0; i < view.granule_count(); ++i) {
        const SegmentView::GranuleSpan held = view.granule(i);
        const std::uint32_t granule_length =
            i + 1U < view.granule_count() ? held.length : length - held.offset;
        out.push_back({offset + held.offset, granule_length});
    }
    return true;
}

SegmentView::Cursor SegmentView::lower_bound(ByteView key) const {
    if (granules_.empty()) return end();

    std::uint32_t low = 0;
    std::uint32_t high = static_cast<std::uint32_t>(granules_.size());
    while (low < high) {
        const std::uint32_t mid = low + (high - low) / 2U;
        if (compare_bytes(granules_[mid].first_key, key) <= 0) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    const std::uint32_t granule = low == 0 ? 0 : low - 1U;
    const Granule& selected = granules_[granule];
    const auto* data = byte_data(bytes_);
    std::size_t at = selected.offset;
    const std::size_t granule_end = selected.offset + selected.length;
    while (at < granule_end) {
        const std::size_t before = at;
        Entry entry;
        if (!parse_entry(bytes_, at, granule_end, entry)) return end();
        if (compare_bytes(entry.key(), key) >= 0) {
            return Cursor(this, granule, data + before);
        }
    }
    if (granule + 1U < static_cast<std::uint32_t>(granules_.size())) {
        const Granule& next = granules_[granule + 1U];
        return Cursor(this, granule + 1U, data + next.offset);
    }
    return end();
}

SegmentView::Cursor SegmentView::end() const {
    return Cursor(this, static_cast<std::uint32_t>(granules_.size()), nullptr);
}

SegmentView::SegmentView(ByteView bytes, dstd::Vector<Granule> granules,
                         std::uint64_t entry_count)
    : bytes_(bytes),
      granules_(std::move(granules)),
      entry_count_(entry_count) {}

SegmentView::Cursor::Cursor(const SegmentView* view, std::uint32_t granule,
                            const std::uint8_t* position)
    : view_(view), granule_(granule), position_(position) {
    if (granule_ < static_cast<std::uint32_t>(view_->granules_.size())) {
        const auto* data = byte_data(view_->bytes_);
        const Granule& selected = view_->granules_[granule_];
        position_ = position == nullptr ? data + selected.offset : position;
        granule_end_ = data + selected.offset + selected.length;
    }
}

bool SegmentView::Cursor::at_end() const {
    return view_ == nullptr ||
           granule_ >= static_cast<std::uint32_t>(view_->granules_.size());
}

SegmentView::Entry SegmentView::Cursor::entry() const {
    Entry out;
    if (at_end()) return out;
    const auto* data = byte_data(view_->bytes_);
    std::size_t at = static_cast<std::size_t>(position_ - data);
    const std::size_t end = static_cast<std::size_t>(granule_end_ - data);
    if (!parse_entry(view_->bytes_, at, end, out)) return Entry{};
    return out;
}

void SegmentView::Cursor::next() {
    if (at_end()) return;
    const auto* data = byte_data(view_->bytes_);
    std::size_t at = static_cast<std::size_t>(position_ - data);
    const std::size_t end = static_cast<std::size_t>(granule_end_ - data);
    Entry ignored;
    if (!parse_entry(view_->bytes_, at, end, ignored)) {
        granule_ = static_cast<std::uint32_t>(view_->granules_.size());
        position_ = nullptr;
        granule_end_ = nullptr;
        return;
    }
    position_ = data + at;
    if (position_ == granule_end_) {
        ++granule_;
        if (granule_ < static_cast<std::uint32_t>(view_->granules_.size())) {
            const Granule& next = view_->granules_[granule_];
            position_ = data + next.offset;
            granule_end_ = position_ + next.length;
        } else {
            position_ = nullptr;
            granule_end_ = nullptr;
        }
    }
}

}  // namespace dftracer::utils::store
