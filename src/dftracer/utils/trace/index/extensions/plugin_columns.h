// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_PLUGIN_COLUMNS_H
#define DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_PLUGIN_COLUMNS_H

#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::trace::views::detail {
struct FoldEvent;
}

namespace dftracer::utils::trace::index::extensions {

/// The typed column of each requested path over one chunk, built from the
/// events the scan already decoded: a value goes into a per-path buffer, and
/// no Series exists until the chunk ends. The paths are the dotted names a
/// filter uses: `name`, `cat`, `pid`, `tid`, `ts`, `dur`, `fhash`, `hhash` and
/// `args.<key>` (a path as it is for a file indexed by path). One thread uses
/// an instance at a time.
///
/// A path the record schema lists in `index.never_index` has no value in the
/// index. A column asked for by name has no data, so a chunk with records fails
/// (@ref Taken::never) rather than pass an empty column that would read as "no
/// record has a value". `*` does not ask for it by name: it skips the path, and
/// the chunk is not failed.
class PluginColumns {
   public:
    /// One column of a chunk.
    struct Column {
        dstd::String path;
        dataframe::Series series;
    };
    /// What @ref take returns. A path whose values had no single type in the
    /// chunk is in `mixed` and has no column.
    struct Taken {
        dstd::Vector<Column> columns;
        dstd::Vector<dstd::String> mixed;
        /// Paths asked for by name that the schema marks never_index; they
        /// have no column, and the chunk must store nothing.
        dstd::Vector<dstd::String> never;
    };

    /// `paths` are the requested paths; `*` asks for every path.
    explicit PluginColumns(dstd::Span<const dstd::String> paths);

    /// The schema's never_index paths, sorted. Borrowed: they must outlive the
    /// instance. Call before @ref add.
    void set_never_index(dstd::Span<const dstd::String> never);

    /// Adds `events` as the next records of the chunk. A metadata event is a
    /// record with no value.
    void add(dstd::Span<const trace::views::detail::FoldEvent> events,
             const dftracer::utils::dstd::StringIntern& intern);

    /// The columns of the records added since the last call, each with one row
    /// per record; a path no record carried has none. Starts a new chunk.
    Taken take();

   private:
    enum class Kind : std::uint8_t { None, Int, Double, String, Mixed };

    struct Acc {
        dstd::String path;
        // One entry per row up to the last value: an int64, a uint64 (an
        // integer above int64 sets `big`), the bits of a double or an interned
        // string id.
        dstd::Vector<std::uint64_t> bits;
        dstd::Vector<std::uint8_t> valid;
        Kind kind = Kind::None;
        bool negative = false;
        bool big = false;
        bool never = false;
    };

    static constexpr std::int32_t UNSET = -2;
    static constexpr std::int32_t NOT_WANTED = -1;
    enum Fixed : std::size_t {
        FxName,
        FxCat,
        FxFhash,
        FxHhash,
        FxPid,
        FxTid,
        FxTs,
        FxDur,
        FxCount
    };

    // The accumulator of `path`, created for `*`; NOT_WANTED when no one asked.
    std::int32_t index_of(dstd::StringView path);
    std::int32_t fixed_index(Fixed which);
    std::int32_t arg_index(std::uint32_t key, bool by_path);
    void put(std::int32_t acc, Kind kind, std::uint64_t bits, bool negative,
             bool big);
    void put_uint(std::int32_t acc, std::uint64_t value);
    void put_id(std::int32_t acc, std::uint32_t id);

    bool is_never(dstd::StringView path) const;

    dstd::Span<const dstd::String> never_;
    bool all_ = false;
    dstd::StringViewMap<std::int32_t> wanted_;
    dstd::Vector<Acc> accs_;
    std::int32_t fixed_[FxCount];
    dstd::HashMap<std::uint32_t, std::int32_t> args_[2];
    const dftracer::utils::dstd::StringIntern* intern_ = nullptr;
    std::uint64_t rows_ = 0;
};

}  // namespace dftracer::utils::trace::index::extensions

#endif  // DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_PLUGIN_COLUMNS_H
