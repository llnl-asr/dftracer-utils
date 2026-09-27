#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_ROWSET_FOLD_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_ROWSET_FOLD_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dftracer::utils::index::extensions {

/// Evaluates the indexable row sets of a file's source on every record the
/// index build decodes, and writes each as an Arrow IPC frame. Rows of an
/// unsealed unit are dropped with it. Over the byte budget it keeps no rows,
/// so the row sets run as query-time sides instead.
class RowSetFold : public trace::views::detail::Fold {
   public:
    using Value = std::variant<std::monostate, bool, std::int64_t,
                               std::uint64_t, double, std::string>;

    RowSetFold(dftracer::utils::StringIntern& intern,
               std::vector<IndexedRowSet> rowsets,
               std::uint64_t budget_bytes = 64ull << 20);

    bool accepts(const trace::views::detail::ScanShape&) const override {
        return true;
    }
    bool needs_args() const override { return true; }
    std::unique_ptr<trace::views::detail::Fold> slice() const override;
    void step(const trace::views::detail::FoldBatch& batch) override;
    void seal_unit(const trace::views::detail::ScanUnit& unit) override;
    void drop_unit(const trace::views::detail::ScanUnit& unit) override;
    void merge(trace::views::detail::Fold& slice) override;
    coro::CoroTask<bool> finalize(
        const trace::views::detail::CoverageSet&) override {
        co_return true;
    }

    /// Writes each row set's sealed rows and the file's manifest entry. The
    /// source is part of the schema's hash, which the profile records.
    void write(index::store::IndexWrite& w, int file_id) const;

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
        std::optional<trace::RecordPhase> phase;
        std::optional<std::uint32_t> name;
        // The filter without the phase term the guard checks, for records
        // whose phase is a field of the event rather than a value.
        std::optional<duql::Query> rest;
    };
    struct Rows {
        std::vector<std::vector<Value>> columns;
    };

    // Appends `from`'s rows to row set `r`'s sealed rows, each distinct row
    // once when the row set is distinct, and empties `from`.
    void absorb(std::size_t r, Rows& from);

    dftracer::utils::StringIntern* intern_;
    std::vector<IndexedRowSet> rowsets_;
    std::vector<Guard> guards_;
    std::uint64_t budget_;
    std::uint64_t bytes_ = 0;
    bool abandoned_ = false;
    std::vector<Rows> pending_;
    std::vector<Rows> sealed_;
    // Per distinct row set, the keys of its sealed rows.
    std::vector<dftracer::utils::StringViewSet> seen_;
    duql::ValueMap scratch_;
};

}  // namespace dftracer::utils::index::extensions

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_ROWSET_FOLD_H
