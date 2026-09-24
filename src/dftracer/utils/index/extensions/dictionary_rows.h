#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_DICTIONARY_ROWS_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_DICTIONARY_ROWS_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/trace/views/coverage.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store {
class IndexWrite;
}  // namespace dftracer::utils::index::store

namespace dftracer::utils::index::extensions {

/// Accumulates dictionary rows (schema dictionaries, keyed by dictionary and
/// key) and writes them to an index's core.dict. Shared by every path that
/// harvests dictionaries from a scan it is riding along.
///
/// Coverage unit is the whole FILE: a key is defined at its first use, so a
/// partially read file may reference a key whose row lives in a member never
/// visited. Rows land in a pending set and promote to sealed only when their
/// unit seals (publish-on-completion), so an aborted unit contributes nothing.
/// Over the byte budget it abandons rather than grow unbounded; the scan it
/// rides along is unaffected.
class DictionaryRows {
   public:
    using Fields = std::vector<std::pair<std::string, std::string>>;

    explicit DictionaryRows(std::uint64_t budget_bytes = 64ull << 20)
        : budget_(budget_bytes) {}

    /// First non-empty path wins; every row is written to this index.
    void set_index_path(std::string_view path) {
        if (index_path_.empty()) index_path_.assign(path);
    }

    void add(std::string_view dict, std::string_view key, const Fields& fields);

    void seal_unit(std::string_view file_path);
    void drop_unit() { pending_.clear(); }
    void merge(DictionaryRows& other);

    /// Writes rows whose source file `covered` read whole, skipping any the
    /// index already holds. Returns false when nothing was persisted.
    coro::CoroTask<bool> commit(
        const trace::views::detail::CoverageSet& covered);

    /// Write the sealed rows into a caller-owned write, for a fresh index
    /// build that owns the transaction - no delta check or coverage gate,
    /// since the caller has established the whole file.
    void write(index::store::IndexWrite& w) const;

    std::size_t entry_count() const noexcept { return sealed_.size(); }
    bool abandoned() const noexcept { return abandoned_; }
    std::uint64_t budget() const noexcept { return budget_; }

   private:
    // [dict][0x00][key] -> fields.
    using Rows = StringViewMap<Fields>;

    Rows pending_;  // current unit; published only when it seals
    Rows sealed_;
    // Source file per row, so commit writes only rows whose file the scan
    // read whole.
    StringViewMap<std::string> entry_file_;
    std::string index_path_;
    std::uint64_t budget_;
    std::uint64_t bytes_ = 0;
    bool abandoned_ = false;
};

}  // namespace dftracer::utils::index::extensions

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_DICTIONARY_ROWS_H
