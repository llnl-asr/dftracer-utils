// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_CATALOG_FOLD_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_CATALOG_FOLD_H

#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vendor/unordered_dense.h>
#include <dftracer/utils/index/store/types.h>
#include <dftracer/utils/trace/views/fold.h>

#include <cstdint>

namespace dftracer::utils::index::extensions {

/// Folds one occurrence of a path, with the PathType `tag` it held in a
/// record, into the path's catalog entry. The one rule behind every catalog
/// the build and the scans write.
void observe_catalog_path(index::store::PathStat& stat, std::uint8_t tag);

/// Writes each file's path catalog from the schema leaves of a full scan, so
/// the first query on a trace leaves the catalog a full index build would.
/// Needs no metadata records: they never enter the catalog.
class CatalogFold : public trace::views::detail::Fold {
   public:
    /// `params_hash` is the CATALOG hash of the config a full build uses.
    CatalogFold(dftracer::utils::dstd::StringIntern& intern,
                std::uint64_t params_hash)
        : intern_(&intern), params_hash_(params_hash) {}

    /// A filtered or windowed scan misses records, so its catalog would
    /// differ from the build's.
    bool accepts(const trace::views::detail::ScanShape& shape) const override {
        return !shape.filtered && !shape.windowed;
    }
    bool wants_schema() const override { return true; }
    bool reads_never_indexed() const override { return false; }
    bool wants_metadata() const override { return false; }

    // keep std: trace::views::detail::Fold::slice returns std::unique_ptr
    std::unique_ptr<trace::views::detail::Fold> slice() const override {
        // keep std: Fold::slice returns std::unique_ptr
        return std::make_unique<CatalogFold>(*intern_, params_hash_);
    }

    void step(const trace::views::detail::FoldBatch& batch) override;
    void seal_unit(const trace::views::detail::ScanUnit&) override {}
    void drop_unit(const trace::views::detail::ScanUnit&) override {}
    void merge(trace::views::detail::Fold& slice) override;
    /// Writes the catalog of each covered file whose index has none current.
    /// A locked index logs a warning and writes nothing.
    coro::CoroTask<bool> finalize(
        const trace::views::detail::CoverageSet& covered) override;

   private:
    struct FileState {
        dstd::String index_path;
        ankerl::unordered_dense::map<std::uint32_t, index::store::PathStat>
            paths;
    };

    dftracer::utils::dstd::StringIntern* intern_;
    std::uint64_t params_hash_;
    dstd::Map<dstd::String, FileState> files_;
};

}  // namespace dftracer::utils::index::extensions

#endif
