// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_ROWSET_FOLD_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_ROWSET_FOLD_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/variant.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>

#include <cstdint>

namespace dftracer::utils::index {
class FileBuild;
}

namespace dftracer::utils::index::extensions {

/// Evaluates the indexable row sets of a file's source on every record the
/// index build decodes, and writes each as an Arrow IPC frame. Rows of an
/// unsealed unit are dropped with it. Over the byte budget it keeps no rows,
/// so the row sets run as query-time sides instead.
class RowSetFold : public trace::views::detail::Fold {
   public:
    using Value =
        dstd::Variant<dstd::Monostate, bool, std::int64_t, std::uint64_t,
                      double, dftracer::utils::dstd::String>;

    RowSetFold(dftracer::utils::dstd::StringIntern& intern,
               dstd::Vector<IndexedRowSet> rowsets,
               std::uint64_t budget_bytes = 64ull << 20);

    bool accepts(const trace::views::detail::ScanShape&) const override {
        return true;
    }
    bool needs_args() const override { return true; }
    // The schema refuses a row set that reads a never_index path.
    bool reads_never_indexed() const override { return false; }
    // keep std: trace::views::detail::Fold::slice returns std::unique_ptr
    std::unique_ptr<trace::views::detail::Fold> slice() const override;
    void step(const trace::views::detail::FoldBatch& batch) override;
    void seal_unit(const trace::views::detail::ScanUnit& unit) override;
    void drop_unit(const trace::views::detail::ScanUnit& unit) override;
    void merge(trace::views::detail::Fold& slice) override;
    coro::CoroTask<bool> finalize(
        const trace::views::detail::CoverageSet&) override {
        co_return true;
    }

    /// Writes each row set's sealed rows into `build` and finishes the
    /// extension. The source is part of the schema's hash, which the profile
    /// records.
    void write(index::FileBuild& build) const;

    bool abandoned() const noexcept { return abandoned_; }
    /// Sealed rows over every row set.
    std::size_t entry_count() const noexcept {
        std::size_t n = 0;
        for (const auto& s : sealed_)
            if (!s.columns.empty()) n += s.columns.front().size();
        return n;
    }

   private:
    // Cheap tests a record must pass before its filter runs.
    struct Guard {
        dstd::Optional<trace::RecordPhase> phase;
        dstd::Optional<std::uint32_t> name;
        // The filter without the phase term the guard checks, for records
        // whose phase is a field of the event rather than a value.
        dstd::Optional<duql::Query> rest;
    };
    struct Rows {
        dstd::Vector<dstd::Vector<Value>> columns;
    };

    // Appends `from`'s rows to row set `r`'s sealed rows, each distinct row
    // once when the row set is distinct, and empties `from`.
    void absorb(std::size_t r, Rows& from);

    dftracer::utils::dstd::StringIntern* intern_;
    dstd::Vector<IndexedRowSet> rowsets_;
    dstd::Vector<Guard> guards_;
    std::uint64_t budget_;
    std::uint64_t bytes_ = 0;
    bool abandoned_ = false;
    dstd::Vector<Rows> pending_;
    dstd::Vector<Rows> sealed_;
    // Per distinct row set, the keys of its sealed rows.
    dstd::Vector<dftracer::utils::dstd::StringViewSet> seen_;
    duql::ValueMap scratch_;
};

}  // namespace dftracer::utils::index::extensions

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_ROWSET_FOLD_H
