// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/hash/value_hash.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/trace/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/trace/index/extensions/kinds/payloads.h>
#include <dftracer/utils/trace/index/extensions/summaries.h>
#include <dftracer/utils/trace/index/store/layout.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace dftracer::utils::trace::index::extensions {

namespace {

using dataframe::TypeId;

bool is_summarized_type(TypeId type) {
    switch (type) {
        case TypeId::Unknown:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float32:
        case TypeId::Float64:
        case TypeId::String:
            return true;
        default:
            return false;
    }
}

// The hashes under which a leaf value may sit in a bloom filter or a posting
// list: its canonical hash and, where the path may hold strings, the hash of
// its text.
void value_hashes(const ExtensionValue& v, TypeId type,
                  dstd::Vector<std::uint64_t>& out) {
    const bool numeric_path = type != TypeId::Unknown && type != TypeId::String;
    switch (v.kind) {
        case ExtensionValue::Kind::Bool:
            out.push_back(dstd::hash::value_hash_bool(v.b));
            return;
        case ExtensionValue::Kind::Int64:
            out.push_back(dstd::hash::value_hash_int64(v.i));
            if (!numeric_path)
                out.push_back(
                    dstd::hash::value_hash_string(dstd::to_string(v.i)));
            return;
        case ExtensionValue::Kind::UInt64:
            out.push_back(dstd::hash::value_hash_uint64(v.u));
            // A fold stores a uint64 above int64 as the double it rounds to.
            if (v.u > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max()))
                out.push_back(
                    dstd::hash::value_hash_double(static_cast<double>(v.u)));
            if (!numeric_path)
                out.push_back(
                    dstd::hash::value_hash_string(dstd::to_string(v.u)));
            return;
        case ExtensionValue::Kind::Double:
            out.push_back(dstd::hash::value_hash_double(v.f));
            if (!numeric_path)
                out.push_back(
                    dstd::hash::value_hash_string(canonical_number_text(v.f)));
            return;
        case ExtensionValue::Kind::String:
            if (v.str) out.push_back(dstd::hash::value_hash_string(*v.str));
            return;
    }
}

// The leaf's equality operands, or false when it is not an equality leaf.
bool equality_hashes(const ExtensionLeaf& leaf, TypeId type,
                     dstd::Vector<std::uint64_t>& out) {
    if (leaf.op == ExtensionOp::Eq) {
        value_hashes(leaf.value, type, out);
        return true;
    }
    if (leaf.op == ExtensionOp::In) {
        for (const auto& item : leaf.items) value_hashes(item, type, out);
        return true;
    }
    return false;
}

dstd::String value_text(const ExtensionValue& v) {
    switch (v.kind) {
        case ExtensionValue::Kind::Bool:
            return v.b ? "1" : "0";
        case ExtensionValue::Kind::Int64:
            return dstd::to_string(v.i);
        case ExtensionValue::Kind::UInt64:
            return dstd::to_string(v.u);
        case ExtensionValue::Kind::Double:
            return canonical_number_text(v.f);
        case ExtensionValue::Kind::String:
            return v.str ? *v.str : dstd::String();
    }
    return {};
}

ZoneLiteral zone_literal(const ExtensionValue& v) {
    switch (v.kind) {
        case ExtensionValue::Kind::Bool:
            return ZoneLiteral::from_number(
                duql::Number{std::int64_t{v.b ? 1 : 0}});
        case ExtensionValue::Kind::Int64:
            return ZoneLiteral::from_number(duql::Number{v.i});
        case ExtensionValue::Kind::UInt64:
            return ZoneLiteral::from_number(duql::Number{v.u});
        case ExtensionValue::Kind::Double:
            return ZoneLiteral::from_number(duql::Number{v.f});
        case ExtensionValue::Kind::String:
            return ZoneLiteral::from_text(v.str ? *v.str : dstd::String());
    }
    return {};
}

}  // namespace

namespace {

bool zone_op_of(ExtensionOp op, ZoneOp& out) {
    switch (op) {
        case ExtensionOp::Gt:
            out = ZoneOp::Gt;
            return true;
        case ExtensionOp::Ge:
            out = ZoneOp::Ge;
            return true;
        case ExtensionOp::Lt:
            out = ZoneOp::Lt;
            return true;
        case ExtensionOp::Le:
            out = ZoneOp::Le;
            return true;
        default:
            return false;
    }
}

}  // namespace

ZonemapQuery::ZonemapQuery(const ExtensionLeaf& leaf) : op_(leaf.op) {
    if (op_ == ExtensionOp::In) {
        for (const auto& item : leaf.items)
            items_.push_back(zone_literal(item));
    } else {
        literal_ = zone_literal(leaf.value);
    }
}

bool ZonemapQuery::may_match(dstd::StringView payload) const {
    const auto zone = kinds::decode_zone(payload);
    return !zone || may_match_zone(*zone);
}

bool ZonemapQuery::may_match_zone(const kinds::Zone& zone) const {
    if (zone.present && *zone.present == 0) return false;
    if (op_ == ExtensionOp::Eq) return zone.may_equal(literal_);
    if (op_ == ExtensionOp::In) {
        for (const auto& item : items_)
            if (zone.may_equal(item)) return true;
        return false;
    }
    ZoneOp zop;
    return !zone_op_of(op_, zop) || zone.may_match(zop, literal_);
}

bool ZonemapQuery::all_match_zone(const kinds::Zone& zone) const {
    ZoneOp zop;
    return zone_op_of(op_, zop) && zone.all_match(zop, literal_);
}

bool BloomQuery::may_match_filter(const ScalableBloomFilter& filter) const {
    for (std::uint64_t h : hashes_)
        if (filter.possibly_contains_hashed(h)) return true;
    return false;
}

bool BloomQuery::may_match(dstd::StringView payload) const {
    const auto filter = kinds::decode_bloom(payload);
    return !filter || may_match_filter(*filter);
}

bool BloomQuery::file_may_match(dstd::StringView payload) const {
    return may_match(payload);
}

bool CountsQuery::listed(dstd::StringView value) const {
    for (const auto& t : texts_)
        if (t == value) return true;
    return false;
}

bool CountsQuery::may_match_counts(const CountsBuilder::Map& counts) const {
    switch (op_) {
        case ExtensionOp::Eq:
        case ExtensionOp::In:
            for (const auto& t : texts_)
                if (counts.count(t) > 0) return true;
            return false;
        case ExtensionOp::Ne:
            // A granule whose only value is the operand holds no event != it.
            return !(counts.size() == 1 && counts.count(texts_.front()) > 0);
        case ExtensionOp::NotIn:
            for (const auto& entry : counts)
                if (!listed(entry.first)) return true;
            return false;
        default:
            return true;
    }
}

bool CountsQuery::all_match_counts(const CountsBuilder::Map& counts) const {
    switch (op_) {
        case ExtensionOp::Eq:
            return counts.size() == 1 && counts.count(texts_.front()) > 0;
        case ExtensionOp::Ne:
            return counts.count(texts_.front()) == 0;
        case ExtensionOp::In:
            if (counts.empty()) return false;
            for (const auto& entry : counts)
                if (!listed(entry.first)) return false;
            return true;
        case ExtensionOp::NotIn:
            if (counts.empty()) return false;
            for (const auto& t : texts_)
                if (counts.count(t) > 0) return false;
            return true;
        default:
            return false;
    }
}

bool CountsQuery::may_match(dstd::StringView payload) const {
    const auto counts = kinds::decode_counts(payload);
    if (!counts || !counts->compressed) return true;
    return may_match_counts(CountsBuilder::decompress(
        counts->compressed->data(), counts->compressed->size()));
}

bool PostingsQuery::may_match(dstd::StringView payload) const {
    return probe(payload);
}

bool PostingsQuery::file_may_match(dstd::StringView payload) const {
    return probe(payload);
}

bool PostingsQuery::probe(dstd::StringView payload) const {
    const auto listed = PostingsBuilder::decode(payload);
    for (std::uint64_t h : hashes_)
        if (std::binary_search(listed.begin(), listed.end(), h)) return true;
    return false;
}

void ZonemapBuilder::add_i64(dstd::Span<const std::int64_t> values) {
    for (const auto v : values) bounds_.add(v);
}

void ZonemapBuilder::add_u64(dstd::Span<const std::uint64_t> values) {
    for (const auto v : values) bounds_.add(v);
}

void ZonemapBuilder::add_f64(dstd::Span<const double> values) {
    for (const auto v : values) bounds_.add(v);
}

void ZonemapBuilder::add_str(dstd::Span<const dstd::StringView> values) {
    for (const auto v : values) bounds_.add(v);
}

void ZonemapBuilder::merge_from(const SummaryBuilder& other) {
    const auto& o = static_cast<const ZonemapBuilder&>(other);
    bounds_.merge_from(o.bounds_);
}

bool ZonemapBuilder::finish_granule(ExtensionOut& out) {
    const auto& b = bounds_;
    const std::uint64_t seen = b.value_count() + b.nan_count();
    const bool mixed = b.type() == BoundType::Mixed && seen > 0;
    if (!b.has_bounds() && !mixed && b.null_count() == 0 &&
        b.nan_count() == 0 && !present_)
        return false;
    kinds::Zone zone;
    static_cast<ZoneBounds&>(zone) = b;
    zone.observed = observed_;
    zone.present = present_ ? *present_ : seen;
    zone.histogram = histogram_;
    out.append(kinds::encode_zone(zone));
    return !out.over;
}

void BloomBuilder::add_i64(dstd::Span<const std::int64_t> values) {
    for (const auto v : values)
        filter_.add_hashed(dstd::hash::value_hash_int64(v));
}

void BloomBuilder::add_u64(dstd::Span<const std::uint64_t> values) {
    for (const auto v : values)
        filter_.add_hashed(dstd::hash::value_hash_uint64(v));
}

void BloomBuilder::add_f64(dstd::Span<const double> values) {
    for (const auto v : values)
        filter_.add_hashed(dstd::hash::value_hash_double(v));
}

void BloomBuilder::add_str(dstd::Span<const dstd::StringView> values) {
    for (const auto v : values) filter_.add(v);
}

void BloomBuilder::merge_from(const SummaryBuilder& other) {
    filter_.merge_from(static_cast<const BloomBuilder&>(other).filter_);
}

bool BloomBuilder::write(ExtensionOut& out) {
    filter_.serialize_into(blob_);
    out.append(kinds::encode_bloom(
        blob_, static_cast<std::uint64_t>(filter_.entry_count())));
    return !out.over;
}

bool BloomBuilder::finish_granule(ExtensionOut& out) { return write(out); }

bool BloomBuilder::finish_file(ExtensionOut& out) { return write(out); }

CountsBuilder::CountsBuilder(const CountsBuilder& other)
    : SummaryBuilder(other),
      counts_(other.counts_),
      observed_(other.observed_),
      cap_(other.cap_) {}

CountsBuilder::CountsBuilder(CountsBuilder&& other) noexcept
    : SummaryBuilder(other),
      counts_(std::move(other.counts_)),
      observed_(other.observed_),
      cap_(other.cap_) {
    other.last_key_ = nullptr;
    other.last_counter_ = nullptr;
}

CountsBuilder& CountsBuilder::operator=(const CountsBuilder& other) {
    if (this != &other) {
        counts_ = other.counts_;
        observed_ = other.observed_;
        cap_ = other.cap_;
        last_key_ = nullptr;
        last_counter_ = nullptr;
    }
    return *this;
}

CountsBuilder& CountsBuilder::operator=(CountsBuilder&& other) noexcept {
    if (this != &other) {
        counts_ = std::move(other.counts_);
        observed_ = other.observed_;
        cap_ = other.cap_;
        last_key_ = nullptr;
        last_counter_ = nullptr;
        other.last_key_ = nullptr;
        other.last_counter_ = nullptr;
    }
    return *this;
}

bool CountsBuilder::add_text(dstd::StringView value) {
    if (last_key_ != nullptr && *last_key_ == value) {
        ++*last_counter_;
        return false;
    }
    if (!counts_) counts_.emplace();
    auto it = counts_->find(value);
    if (it != counts_->end()) {
        ++it->second;
        last_key_ = &it->first;
        last_counter_ = &it->second;
        return false;
    }
    auto [added, inserted] = counts_->emplace(value, 1);
    last_key_ = &added->first;
    last_counter_ = &added->second;
    return true;
}

bool CountsBuilder::add_text(dstd::StringView value, std::uint32_t& slot) {
    if (slot != NO_SLOT) {
        ++(counts_->begin() + slot)->second;
        return false;
    }
    const bool added = add_text(value);
    slot = static_cast<std::uint32_t>(counts_->find(value) - counts_->begin());
    return added;
}

void CountsBuilder::add_i64(dstd::Span<const std::int64_t> values) {
    for (const auto v : values) add_text(dstd::to_string(v));
}

void CountsBuilder::add_u64(dstd::Span<const std::uint64_t> values) {
    for (const auto v : values) add_text(dstd::to_string(v));
}

void CountsBuilder::add_f64(dstd::Span<const double> values) {
    for (const auto v : values) add_text(canonical_number_text(v));
}

void CountsBuilder::add_str(dstd::Span<const dstd::StringView> values) {
    for (const auto v : values) add_text(v);
}

void CountsBuilder::merge_from(const SummaryBuilder& other) {
    const auto& o = static_cast<const CountsBuilder&>(other);
    if (!o.counts_) return;
    if (!counts_) counts_.emplace();
    for (const auto& [k, v] : *o.counts_) (*counts_)[k] += v;
    last_key_ = nullptr;
    last_counter_ = nullptr;
}

bool CountsBuilder::finish_granule(ExtensionOut& out) {
    if (cap_ == 0) return false;
    const auto blob = compress(cap_);
    if (!blob) return false;
    out.append(kinds::encode_counts({observed_, *blob}));
    return !out.over;
}

dstd::Vector<std::uint8_t> CountsBuilder::serialize() const {
    if (!counts_ || counts_->empty()) return {};
    dstd::Vector<std::uint8_t> buf;
    buf.reserve(4 + counts_->size() * 20);
    dstd::append_u32_le(buf, static_cast<std::uint32_t>(counts_->size()));
    for (const auto& [key, count] : *counts_) {
        const auto key_size = static_cast<std::uint16_t>(
            std::min<std::size_t>(key.size(), 0xFFFF));
        dstd::append_u16_le(buf, key_size);
        buf.insert(buf.end(), key.data(), key.data() + key_size);
        dstd::append_u64_le(buf, count);
    }
    return buf;
}

dstd::Optional<dstd::Vector<std::uint8_t>> CountsBuilder::compress(
    std::size_t cap_bytes) const {
    auto raw = serialize();
    if (raw.empty()) return dstd::nullopt;
    fileio::compress::GzipMemberCompressor compressor;
    if (!compressor.valid()) return dstd::nullopt;
    auto member = compressor.compress_member(raw.data(), raw.size());
    if (!member) return dstd::nullopt;
    // The decoded length leads, so the reader sizes its buffer exactly.
    if (4 + member->size() > cap_bytes) return dstd::nullopt;
    dstd::Vector<std::uint8_t> out(4 + member->size());
    dstd::write_u32_le(out.data(), static_cast<std::uint32_t>(raw.size()));
    std::copy(member->begin(), member->end(), out.begin() + 4);
    return out;
}

CountsBuilder::Map CountsBuilder::deserialize(const std::uint8_t* data,
                                              std::size_t len) {
    Map result;
    if (!data || len < 4) return result;
    std::size_t pos = 0;
    const std::uint32_t num = dstd::read_u32_le(data + pos);
    pos += 4;
    for (std::uint32_t i = 0; i < num && pos + 2 <= len; ++i) {
        const std::uint16_t key_size = dstd::read_u16_le(data + pos);
        pos += 2;
        if (pos + key_size + 8 > len) break;
        dstd::String key(reinterpret_cast<const char*>(data + pos), key_size);
        pos += key_size;
        const std::uint64_t count = dstd::read_u64_le(data + pos);
        pos += 8;
        result[std::move(key)] = count;
    }
    return result;
}

CountsBuilder::Map CountsBuilder::decompress(const std::uint8_t* data,
                                             std::size_t len) {
    if (!data || len < 4) return {};
    const std::uint32_t decoded_size = dstd::read_u32_le(data);
    fileio::compress::GzipMemberDecompressor decompressor;
    if (!decompressor.valid()) return {};
    dstd::Vector<std::uint8_t> decompressed(decoded_size);
    auto res = decompressor.decompress(data + 4, len - 4, decompressed.data(),
                                       decompressed.size());
    if (!res || res->out_bytes != decoded_size) return {};
    return deserialize(decompressed.data(), res->out_bytes);
}

void PostingsBuilder::add_i64(dstd::Span<const std::int64_t> values) {
    for (const auto v : values) hashes_.insert(dstd::hash::value_hash_int64(v));
}

void PostingsBuilder::add_u64(dstd::Span<const std::uint64_t> values) {
    for (const auto v : values)
        hashes_.insert(dstd::hash::value_hash_uint64(v));
}

void PostingsBuilder::add_f64(dstd::Span<const double> values) {
    for (const auto v : values)
        hashes_.insert(dstd::hash::value_hash_double(v));
}

void PostingsBuilder::add_str(dstd::Span<const dstd::StringView> values) {
    for (const auto v : values)
        hashes_.insert(dstd::hash::value_hash_string(v));
}

void PostingsBuilder::merge_from(const SummaryBuilder& other) {
    const auto& o = static_cast<const PostingsBuilder&>(other);
    hashes_.insert(o.hashes_.begin(), o.hashes_.end());
}

dstd::Vector<std::uint64_t> PostingsBuilder::hashes() const {
    dstd::Vector<std::uint64_t> out(hashes_.begin(), hashes_.end());
    std::sort(out.begin(), out.end());
    return out;
}

bool PostingsBuilder::write(ExtensionOut& out) {
    if (hashes_.empty()) return false;
    dstd::String bytes;
    for (std::uint64_t h : hashes()) dstd::append_u64_be(bytes, h);
    out.append(bytes);
    return !out.over;
}

bool PostingsBuilder::finish_granule(ExtensionOut& out) { return write(out); }

bool PostingsBuilder::finish_file(ExtensionOut& out) { return write(out); }

dstd::Vector<std::uint64_t> PostingsBuilder::decode(dstd::StringView payload) {
    dstd::Vector<std::uint64_t> out;
    if (payload.size() % 8 != 0) return out;
    out.reserve(payload.size() / 8);
    for (std::size_t i = 0; i < payload.size(); i += 8)
        out.push_back(dstd::read_u64_be(payload.substr(i)));
    return out;
}

store::IndexExtension ZonemapSummary::extension() const {
    return store::IndexExtension::Zonemap;
}
store::IndexExtension BloomSummary::extension() const {
    return store::IndexExtension::Bloom;
}
store::IndexExtension CountsSummary::extension() const {
    return store::IndexExtension::Counts;
}
store::IndexExtension PostingsSummary::extension() const {
    return store::IndexExtension::Postings;
}

std::uint32_t ZonemapSummary::version() const {
    return store::layout::ext_version(extension());
}
std::uint32_t BloomSummary::version() const {
    return store::layout::ext_version(extension());
}
std::uint32_t CountsSummary::version() const {
    return store::layout::ext_version(extension());
}
std::uint32_t PostingsSummary::version() const {
    return store::layout::ext_version(extension());
}

dstd::UniquePtr<SummaryBuilder> ZonemapSummary::builder(TypeId type) const {
    if (!is_summarized_type(type)) return nullptr;
    return dstd::make_unique<ZonemapBuilder>();
}

dstd::UniquePtr<SummaryBuilder> BloomSummary::builder(TypeId type) const {
    if (!is_summarized_type(type)) return nullptr;
    return dstd::make_unique<BloomBuilder>(params_.expected_entries,
                                           params_.false_positive_rate);
}

dstd::UniquePtr<SummaryBuilder> CountsSummary::builder(TypeId type) const {
    if (!is_summarized_type(type)) return nullptr;
    return dstd::make_unique<CountsBuilder>(cap_);
}

dstd::UniquePtr<SummaryBuilder> PostingsSummary::builder(TypeId type) const {
    if (!is_summarized_type(type)) return nullptr;
    return dstd::make_unique<PostingsBuilder>();
}

dstd::UniquePtr<ZonemapQuery> ZonemapSummary::compile_zone(
    const ExtensionLeaf& leaf) const {
    switch (leaf.op) {
        case ExtensionOp::Eq:
        case ExtensionOp::In:
        case ExtensionOp::Gt:
        case ExtensionOp::Ge:
        case ExtensionOp::Lt:
        case ExtensionOp::Le:
            return dstd::make_unique<ZonemapQuery>(leaf);
        default:
            return nullptr;
    }
}

dstd::UniquePtr<BloomQuery> BloomSummary::compile_bloom(
    const ExtensionLeaf& leaf, TypeId type) const {
    dstd::Vector<std::uint64_t> hashes;
    if (!equality_hashes(leaf, type, hashes)) return nullptr;
    return dstd::make_unique<BloomQuery>(std::move(hashes));
}

dstd::UniquePtr<CountsQuery> CountsSummary::compile_counts(
    const ExtensionLeaf& leaf) const {
    dstd::Vector<dstd::String> texts;
    switch (leaf.op) {
        case ExtensionOp::Eq:
        case ExtensionOp::Ne:
            texts.push_back(value_text(leaf.value));
            break;
        case ExtensionOp::In:
        case ExtensionOp::NotIn:
            for (const auto& item : leaf.items)
                texts.push_back(value_text(item));
            break;
        default:
            return nullptr;
    }
    return dstd::make_unique<CountsQuery>(leaf.op, std::move(texts));
}

dstd::UniquePtr<PostingsQuery> PostingsSummary::compile_postings(
    const ExtensionLeaf& leaf, TypeId type) const {
    dstd::Vector<std::uint64_t> hashes;
    if (!equality_hashes(leaf, type, hashes)) return nullptr;
    return dstd::make_unique<PostingsQuery>(std::move(hashes));
}

dstd::Result<dstd::UniquePtr<ExtensionQuery>> ZonemapSummary::compile(
    const ExtensionLeaf& leaf, TypeId) const {
    dstd::UniquePtr<ExtensionQuery> q = compile_zone(leaf);
    return q;
}

dstd::Result<dstd::UniquePtr<ExtensionQuery>> BloomSummary::compile(
    const ExtensionLeaf& leaf, TypeId type) const {
    dstd::UniquePtr<ExtensionQuery> q = compile_bloom(leaf, type);
    return q;
}

dstd::Result<dstd::UniquePtr<ExtensionQuery>> CountsSummary::compile(
    const ExtensionLeaf& leaf, TypeId) const {
    dstd::UniquePtr<ExtensionQuery> q = compile_counts(leaf);
    return q;
}

dstd::Result<dstd::UniquePtr<ExtensionQuery>> PostingsSummary::compile(
    const ExtensionLeaf& leaf, TypeId type) const {
    dstd::UniquePtr<ExtensionQuery> q = compile_postings(leaf, type);
    return q;
}

}  // namespace dftracer::utils::trace::index::extensions
