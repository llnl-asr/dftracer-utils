// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_SUMMARIES_H
#define DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_SUMMARIES_H

#include <dftracer/utils/dataframe/timestamp_histogram.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/extensions/kinds/payloads.h>
#include <dftracer/utils/trace/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/trace/index/extensions/summary.h>
#include <dftracer/utils/trace/index/extensions/zone_bounds.h>

#include <cstdint>

// The built-in summaries: zonemap, bloom, counts and postings. Each is an
// algorithm over typed values; the feeders (@ref JsonFeeder, @ref
// ColumnFeeder) own the formats.
namespace dftracer::utils::trace::index::extensions {

/// Value range per granule: the bounds, null and NaN counts and exact flags of
/// @ref ZoneBounds. Stores no file payload.
class ZonemapBuilder final : public SummaryBuilder {
   public:
    void add_i64(dstd::Span<const std::int64_t> values) override;
    void add_u64(dstd::Span<const std::uint64_t> values) override;
    void add_f64(dstd::Span<const double> values) override;
    void add_str(dstd::Span<const dstd::StringView> values) override;
    void add_nulls(std::uint64_t count) override { bounds_.add_nulls(count); }
    void set_observed(std::uint64_t records) override { observed_ = records; }
    void merge_from(const SummaryBuilder& other) override;
    bool finish_granule(ExtensionOut& out) override;
    bool has_file_payload() const override { return false; }
    bool finish_file(ExtensionOut&) override { return false; }

    /// The bounds, which a feeder on a hot path adds to without a virtual call.
    ZoneBounds& bounds() { return bounds_; }
    const ZoneBounds& bounds() const { return bounds_; }
    /// Records the zone as present in `records` of the observed ones, and
    /// stores it even when no value was seen: the path is then absent.
    void set_present(std::uint64_t records) { present_ = records; }
    void set_histogram(dataframe::TimestampHistogram histogram) {
        histogram_ = std::move(histogram);
    }
    std::uint64_t observed() const { return observed_; }

   private:
    dstd::Optional<dataframe::TimestampHistogram> histogram_;
    dstd::Optional<std::uint64_t> present_;
    ZoneBounds bounds_;
    std::uint64_t observed_ = 0;
};

/// A bloom filter of the values, per granule and merged for the file.
class BloomBuilder final : public SummaryBuilder {
   public:
    explicit BloomBuilder(std::size_t expected_entries = 1024,
                          double false_positive_rate = 0.01)
        : filter_(expected_entries, false_positive_rate) {}

    void add_i64(dstd::Span<const std::int64_t> values) override;
    void add_u64(dstd::Span<const std::uint64_t> values) override;
    void add_f64(dstd::Span<const double> values) override;
    void add_str(dstd::Span<const dstd::StringView> values) override;
    void add_nulls(std::uint64_t) override {}
    void set_observed(std::uint64_t) override {}
    void merge_from(const SummaryBuilder& other) override;
    bool finish_granule(ExtensionOut& out) override;
    bool has_file_payload() const override { return true; }
    bool finish_file(ExtensionOut& out) override;

    /// A value by its canonical hash, as @ref dstd::hash::value_hash_int64
    /// and its siblings return it.
    void add_hashed(std::uint64_t value_hash) {
        filter_.add_hashed(value_hash);
    }
    ScalableBloomFilter& filter() { return filter_; }
    const ScalableBloomFilter& filter() const { return filter_; }

   private:
    bool write(ExtensionOut& out);

    ScalableBloomFilter filter_;
    dstd::Vector<unsigned char> blob_;
};

/// How often each value occurs in a granule, kept only while its compressed
/// form fits the cap.
class CountsBuilder final : public SummaryBuilder {
   public:
    using Map = dftracer::utils::dstd::StringViewMap<std::uint64_t>;

    explicit CountsBuilder(std::size_t cap_bytes = 4096) : cap_(cap_bytes) {}
    CountsBuilder(const CountsBuilder& other);
    CountsBuilder(CountsBuilder&& other) noexcept;
    CountsBuilder& operator=(const CountsBuilder& other);
    CountsBuilder& operator=(CountsBuilder&& other) noexcept;

    static constexpr std::uint32_t NO_SLOT = UINT32_MAX;

    void add_i64(dstd::Span<const std::int64_t> values) override;
    void add_u64(dstd::Span<const std::uint64_t> values) override;
    void add_f64(dstd::Span<const double> values) override;
    void add_str(dstd::Span<const dstd::StringView> values) override;
    void add_nulls(std::uint64_t) override {}
    void set_observed(std::uint64_t records) override { observed_ = records; }
    void merge_from(const SummaryBuilder& other) override;
    bool finish_granule(ExtensionOut& out) override;
    bool has_file_payload() const override { return false; }
    bool finish_file(ExtensionOut&) override { return false; }

    /// The largest compressed size kept; 0 keeps no counts.
    void set_cap(std::size_t cap_bytes) { cap_ = cap_bytes; }
    std::uint64_t observed() const { return observed_; }
    /// Counts `value`; true when it is new to the granule.
    bool add_text(dstd::StringView value);
    /// As @ref add_text. `slot` is where `value` sits in the map: NO_SLOT
    /// finds it by the text and sets it. Keep a slot for one value.
    bool add_text(dstd::StringView value, std::uint32_t& slot);

    std::uint64_t distinct_count() const {
        return counts_ ? counts_->size() : 0;
    }
    const dstd::Optional<Map>& counts() const { return counts_; }

    /// [u32 LE entry_count] [u16 LE key_size, key bytes, u64 LE count]*
    dstd::Vector<std::uint8_t> serialize() const;
    /// zlib of @ref serialize behind its length; nothing when it passes
    /// `cap_bytes`.
    dstd::Optional<dstd::Vector<std::uint8_t>> compress(
        std::size_t cap_bytes) const;
    static Map deserialize(const std::uint8_t* data, std::size_t len);
    static Map decompress(const std::uint8_t* data, std::size_t len);

   private:
    dstd::Optional<Map> counts_;
    /// Skips the hash lookup when the same value comes back to back. Not
    /// copied: a copy would point into the original's nodes.
    const dstd::String* last_key_ = nullptr;
    std::uint64_t* last_counter_ = nullptr;
    std::uint64_t observed_ = 0;
    std::size_t cap_ = 4096;
};

/// The distinct values, by canonical hash, per granule and for the file: the
/// posting lists the store keeps value to chunks.
class PostingsBuilder final : public SummaryBuilder {
   public:
    void add_i64(dstd::Span<const std::int64_t> values) override;
    void add_u64(dstd::Span<const std::uint64_t> values) override;
    void add_f64(dstd::Span<const double> values) override;
    void add_str(dstd::Span<const dstd::StringView> values) override;
    void add_nulls(std::uint64_t) override {}
    void set_observed(std::uint64_t) override {}
    void merge_from(const SummaryBuilder& other) override;
    bool finish_granule(ExtensionOut& out) override;
    bool has_file_payload() const override { return true; }
    bool finish_file(ExtensionOut& out) override;

    void add_hashed(std::uint64_t value_hash) { hashes_.insert(value_hash); }
    /// The hashes, ascending.
    dstd::Vector<std::uint64_t> hashes() const;
    /// The hashes of a payload @ref finish_granule wrote; empty for bytes
    /// that are not one.
    static dstd::Vector<std::uint64_t> decode(dstd::StringView payload);

   private:
    bool write(ExtensionOut& out);

    dstd::HashSet<std::uint64_t> hashes_;
};

/// A filter leaf prepared against zones. The planner hands it zones it has
/// decoded; @ref may_match reads a stored payload.
class ZonemapQuery final : public ExtensionQuery {
   public:
    explicit ZonemapQuery(const ExtensionLeaf& leaf);

    bool may_match(dstd::StringView payload) const override;
    bool file_may_match(dstd::StringView) const override { return true; }

    /// False when the zone proves that no value of its granule satisfies the
    /// leaf: it records no record of the path, or its exact bounds exclude
    /// every listed value. A bound that is not exact never does.
    bool may_match_zone(const kinds::Zone& zone) const;
    /// Whether every value in the zone satisfies the leaf, a range leaf. The
    /// caller checks that the zone covers every record of its granule.
    bool all_match_zone(const kinds::Zone& zone) const;
    /// Whether a granule with no zone may match.
    bool may_match_missing() const {
        return op_ != ExtensionOp::In || !items_.empty();
    }

   private:
    ExtensionOp op_;
    ZoneLiteral literal_;
    dstd::Vector<ZoneLiteral> items_;
};

/// An equality or `in` leaf as the canonical hashes a bloom filter may hold.
class BloomQuery final : public ExtensionQuery {
   public:
    explicit BloomQuery(dstd::Vector<std::uint64_t> hashes)
        : hashes_(std::move(hashes)) {}

    bool may_match(dstd::StringView payload) const override;
    bool file_may_match(dstd::StringView payload) const override;

    bool may_match_filter(const ScalableBloomFilter& filter) const;
    bool may_match_missing() const { return !hashes_.empty(); }
    const dstd::Vector<std::uint64_t>& hashes() const { return hashes_; }

   private:
    dstd::Vector<std::uint64_t> hashes_;
};

/// An equality, not-equal, `in` or `not in` leaf against value counts.
class CountsQuery final : public ExtensionQuery {
   public:
    CountsQuery(ExtensionOp op, dstd::Vector<dstd::String> texts)
        : op_(op), texts_(std::move(texts)) {}

    bool may_match(dstd::StringView payload) const override;
    bool file_may_match(dstd::StringView) const override { return true; }

    /// False when `counts` proves that no value of the granule satisfies the
    /// leaf.
    bool may_match_counts(const CountsBuilder::Map& counts) const;
    /// Whether every value in `counts` satisfies the leaf. The caller checks
    /// that the counts cover every record of the granule.
    bool all_match_counts(const CountsBuilder::Map& counts) const;
    bool may_match_missing() const {
        return op_ != ExtensionOp::In || !texts_.empty();
    }

   private:
    bool listed(dstd::StringView value) const;

    ExtensionOp op_;
    dstd::Vector<dstd::String> texts_;
};

/// An equality or `in` leaf as the canonical hashes of a posting list.
class PostingsQuery final : public ExtensionQuery {
   public:
    explicit PostingsQuery(dstd::Vector<std::uint64_t> hashes)
        : hashes_(std::move(hashes)) {}

    bool may_match(dstd::StringView payload) const override;
    bool file_may_match(dstd::StringView payload) const override;

    const dstd::Vector<std::uint64_t>& hashes() const { return hashes_; }

   private:
    bool probe(dstd::StringView payload) const;

    dstd::Vector<std::uint64_t> hashes_;
};

/// Parameters the bloom summary is built with.
struct BloomParams {
    std::size_t expected_entries = 1024;
    double false_positive_rate = 0.01;
};

class ZonemapSummary final : public Summary {
   public:
    explicit ZonemapSummary(std::uint64_t params_hash)
        : params_hash_(params_hash) {}
    dstd::StringView name() const override { return "zonemap"; }
    store::IndexExtension extension() const override;
    std::uint32_t version() const override;
    std::uint64_t params_hash() const override { return params_hash_; }
    dstd::UniquePtr<SummaryBuilder> builder(
        dataframe::TypeId type) const override;
    dstd::Result<dstd::UniquePtr<ExtensionQuery>> compile(
        const ExtensionLeaf& leaf, dataframe::TypeId type) const override;
    /// @ref compile with the query's own type; null when the leaf has no
    /// evidence here.
    dstd::UniquePtr<ZonemapQuery> compile_zone(const ExtensionLeaf& leaf) const;

   private:
    std::uint64_t params_hash_;
};

class BloomSummary final : public Summary {
   public:
    BloomSummary(BloomParams params, std::uint64_t params_hash)
        : params_(params), params_hash_(params_hash) {}
    dstd::StringView name() const override { return "bloom"; }
    store::IndexExtension extension() const override;
    std::uint32_t version() const override;
    std::uint64_t params_hash() const override { return params_hash_; }
    dstd::UniquePtr<SummaryBuilder> builder(
        dataframe::TypeId type) const override;
    dstd::Result<dstd::UniquePtr<ExtensionQuery>> compile(
        const ExtensionLeaf& leaf, dataframe::TypeId type) const override;
    /// A path of a numeric type never equals a string, so only a path that
    /// may hold strings (@ref dataframe::TypeId::Unknown or String) also
    /// probes a number literal by its text.
    dstd::UniquePtr<BloomQuery> compile_bloom(const ExtensionLeaf& leaf,
                                              dataframe::TypeId type) const;

   private:
    BloomParams params_;
    std::uint64_t params_hash_;
};

class CountsSummary final : public Summary {
   public:
    CountsSummary(std::size_t cap_bytes, std::uint64_t params_hash)
        : cap_(cap_bytes), params_hash_(params_hash) {}
    dstd::StringView name() const override { return "counts"; }
    store::IndexExtension extension() const override;
    std::uint32_t version() const override;
    std::uint64_t params_hash() const override { return params_hash_; }
    dstd::UniquePtr<SummaryBuilder> builder(
        dataframe::TypeId type) const override;
    dstd::Result<dstd::UniquePtr<ExtensionQuery>> compile(
        const ExtensionLeaf& leaf, dataframe::TypeId type) const override;
    dstd::UniquePtr<CountsQuery> compile_counts(
        const ExtensionLeaf& leaf) const;

   private:
    std::size_t cap_;
    std::uint64_t params_hash_;
};

class PostingsSummary final : public Summary {
   public:
    explicit PostingsSummary(std::uint64_t params_hash)
        : params_hash_(params_hash) {}
    dstd::StringView name() const override { return "postings"; }
    store::IndexExtension extension() const override;
    std::uint32_t version() const override;
    std::uint64_t params_hash() const override { return params_hash_; }
    dstd::UniquePtr<SummaryBuilder> builder(
        dataframe::TypeId type) const override;
    dstd::Result<dstd::UniquePtr<ExtensionQuery>> compile(
        const ExtensionLeaf& leaf, dataframe::TypeId type) const override;
    dstd::UniquePtr<PostingsQuery> compile_postings(
        const ExtensionLeaf& leaf, dataframe::TypeId type) const;

   private:
    std::uint64_t params_hash_;
};

}  // namespace dftracer::utils::trace::index::extensions

#endif  // DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_SUMMARIES_H
