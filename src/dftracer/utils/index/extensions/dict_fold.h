#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_DICT_FOLD_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_DICT_FOLD_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/index/extensions/dictionary_rows.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/trace/views/fold.h>

#include <memory>

namespace dftracer::utils::index::extensions {

/// The dictionary fold: harvests the rows of `dictionaries` from the metadata
/// events the scan already delivers under emit_all_metadata and writes them
/// to the index's core.dict, so the query builds this index artifact as a
/// byproduct of the one pass it runs anyway.
class DictFold : public trace::views::detail::Fold {
   public:
    DictFold(dftracer::utils::StringIntern& intern,
             std::vector<index::Dictionary> dictionaries,
             std::uint64_t budget_bytes = 64ull << 20)
        : intern_(&intern),
          dictionaries_(std::move(dictionaries)),
          dict_(budget_bytes) {}

    bool accepts(const trace::views::detail::ScanShape& shape) const override {
        return shape.include_metadata && shape.emit_all_metadata &&
               !shape.filtered;
    }
    bool needs_args() const override { return true; }

    std::unique_ptr<trace::views::detail::Fold> slice() const override {
        return std::make_unique<DictFold>(*intern_, dictionaries_,
                                          dict_.budget());
    }

    void step(const trace::views::detail::FoldBatch& batch) override;
    void seal_unit(const trace::views::detail::ScanUnit& unit) override {
        dict_.seal_unit(unit.file_path);
    }
    void drop_unit(const trace::views::detail::ScanUnit& /*unit*/) override {
        dict_.drop_unit();
    }
    void merge(trace::views::detail::Fold& slice) override {
        dict_.merge(static_cast<DictFold&>(slice).dict_);
    }
    coro::CoroTask<bool> finalize(
        const trace::views::detail::CoverageSet& covered) override {
        return dict_.commit(covered);
    }

    /// Write the sealed rows and the file's `core.dict` manifest entry
    /// into a caller-owned write, for the streaming index build. Seal the
    /// units first; no coverage gate.
    void write(index::store::IndexWrite& w, int file_id) const {
        dict_.write(w);
        index::store::records::put_manifest(
            w, file_id, index::store::IndexExtension::DICT, 0);
    }

    std::size_t entry_count() const noexcept { return dict_.entry_count(); }
    bool abandoned() const noexcept { return dict_.abandoned(); }

   private:
    dftracer::utils::StringIntern* intern_;
    std::vector<index::Dictionary> dictionaries_;
    DictionaryRows dict_;
};

}  // namespace dftracer::utils::index::extensions

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_DICT_FOLD_H
