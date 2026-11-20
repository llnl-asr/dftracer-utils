// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_SUMMARY_H
#define DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_SUMMARY_H

#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/extensions/extension_registry.h>
#include <dftracer/utils/trace/index/store/file.h>

#include <cstdint>

namespace dftracer::utils::trace::index::extensions {

/// Builds the evidence of one summary for one path over one granule. It knows
/// the algorithm and no data format: a feeder hands it typed values, and it
/// writes its payload. One thread at a time uses a builder.
///
/// A builder takes values of any type, since a JSON path may hold several: the
/// column type given to @ref Summary::builder is only a hint. A builder that
/// cannot summarize a value, such as a bloom filter given nulls, ignores it.
class SummaryBuilder {
   public:
    SummaryBuilder() = default;
    SummaryBuilder(const SummaryBuilder&) = default;
    SummaryBuilder& operator=(const SummaryBuilder&) = default;
    virtual ~SummaryBuilder() = default;

    virtual void add_i64(dstd::Span<const std::int64_t> values) = 0;
    virtual void add_u64(dstd::Span<const std::uint64_t> values) = 0;
    virtual void add_f64(dstd::Span<const double> values) = 0;
    /// The views are borrowed for the call.
    virtual void add_str(dstd::Span<const dstd::StringView> values) = 0;
    /// Records that carry the path as null.
    virtual void add_nulls(std::uint64_t count) = 0;

    /// Adds every row of a column: its nulls, then its values by type. False
    /// when the column type has no summary, and nothing is added. Integers of
    /// any width and floats widen to int64, uint64 and double.
    bool add(const dataframe::Series& column);

    /// The records the granule observed in all, whether or not they carry the
    /// path.
    virtual void set_observed(std::uint64_t records) = 0;

    /// Takes in what another builder of the same summary observed.
    virtual void merge_from(const SummaryBuilder& other) = 0;

    /// Appends the granule's payload to `out`; false when the summary stores
    /// nothing for the granule. The builder keeps its state.
    virtual bool finish_granule(ExtensionOut& out) = 0;
    /// Whether @ref finish_file writes a payload.
    virtual bool has_file_payload() const = 0;
    /// Appends the payload of everything merged into this builder; false when
    /// there is none.
    virtual bool finish_file(ExtensionOut& out) = 0;
};

/// A summary: what one index extension stores about a path, as an algorithm
/// over values. The format of the data sits in a feeder, never here.
class Summary {
   public:
    virtual ~Summary() = default;

    virtual dstd::StringView name() const = 0;
    virtual store::IndexExtension extension() const = 0;
    /// Data built with another version or params hash is not used.
    virtual std::uint32_t version() const = 0;
    virtual std::uint64_t params_hash() const = 0;

    /// A builder for a path of `type` (@ref dataframe::TypeId::Unknown when
    /// the type is not known); null when the summary cannot build for it, in
    /// which case nothing is recorded and nothing prunes.
    virtual dstd::UniquePtr<SummaryBuilder> builder(
        dataframe::TypeId type) const = 0;
    /// Prepares `leaf` against a path of `type`. A null query means no
    /// evidence for the leaf.
    virtual dstd::Result<dstd::UniquePtr<ExtensionQuery>> compile(
        const ExtensionLeaf& leaf, dataframe::TypeId type) const = 0;
};

}  // namespace dftracer::utils::trace::index::extensions

#endif  // DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_SUMMARY_H
