// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_FILE_FORMAT_REGISTRY_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_FILE_FORMAT_REGISTRY_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::dataframe {

/// A Source over one open file of a registered format, which the index reads
/// chunk by chunk: the format splits the file in independently scannable
/// chunks, and may give each a token that names it by what is in the file.
class FormatFileSource : public Source {
   public:
    /// The registered name of the format that opened the file.
    virtual dstd::StringView format_name() const = 0;

    /// The version text the plugin that registered the format declares; empty
    /// when it declares none.
    virtual dstd::StringView plugin_version() const = 0;

    /// The file's path, as it was given to open.
    virtual dstd::StringView path() const = 0;

    /// How many chunks the file has, at least 1.
    virtual std::uint64_t chunk_count() const = 0;

    /// The format's token of each chunk, in chunk order, strictly increasing;
    /// empty when the format gives none, and then the file is one chunk to the
    /// index.
    virtual dstd::Span<const std::uint64_t> chunk_tokens() const = 0;

    /// A Source that reads only `chunks` of the file, in the order given. It
    /// never consults an index, so it reads exactly those chunks whatever its
    /// scan is asked for. A chunk at or past @ref chunk_count is an error of
    /// its scan.
    virtual dstd::SharedPtr<const Source> only_chunks(
        dstd::Vector<std::uint64_t> chunks) const = 0;
};

/// A file format registered under a name. @ref claim_reader probes
/// every registered format and opens the file through the winner.
class FileFormatFactory {
   public:
    virtual ~FileFormatFactory() = default;

    /// The registered name, `<plugin>.<name>`.
    virtual dstd::StringView name() const = 0;

    /// The version text the plugin that registered the format declares; empty
    /// when it declares none.
    virtual dstd::StringView version() const = 0;

    /// How well `head[0..n)` matches this format: 0 (not mine) to 100
    /// (certain); the registry clamps anything outside that range. `head` and
    /// `path` are borrowed for the call. Consulted from any thread.
    virtual int probe(const std::uint8_t* head, std::uint64_t n,
                      dstd::StringView path) const = 0;

    /// A Source over the file at `path`, or the error that stopped it. Every
    /// call opens the file afresh, so one format backs many plans. A scan of
    /// the Source that carries filters asks the index at `index_dir` (empty:
    /// the one beside the file) which chunks may match, and reads only those.
    virtual dstd::Result<dstd::SharedPtr<const FormatFileSource>> open(
        dstd::StringView path, dstd::StringView index_dir) = 0;

    /// How many files opened through this format are still open. Zero for a
    /// format that keeps no count.
    virtual std::size_t open_file_count() const { return 0; }
};

/// One bound output of a @ref SinkFactory: the writing side of a sink.
class SinkWriter {
   public:
    virtual ~SinkWriter() = default;

    /// Write one frame. `batch` is borrowed for the call.
    virtual dstd::Result<void> write(const DataFrame& batch) = 0;

    /// Finish the output. Called once, after the last write().
    virtual dstd::Result<void> finalize() = 0;

    /// Give up on the output. Called once instead of finalize(), and never
    /// beside it.
    virtual void abort() = 0;
};

/// A sink registered under a name: binds one output to a set of columns.
class SinkFactory {
   public:
    virtual ~SinkFactory() = default;

    /// The registered name, `<plugin>.<name>`.
    virtual dstd::StringView name() const = 0;

    /// Bind one output to the `names` columns of `types` (same order), plus
    /// the `options` name/value pairs the caller gave. Both vectors are
    /// borrowed for the call. Nothing calls this yet.
    virtual dstd::Result<dstd::UniquePtr<SinkWriter>> bind(
        dstd::Span<const dstd::String> names, dstd::Span<const TypeId> types,
        dstd::Span<const dstd::Pair<dstd::String, dstd::String>> options) = 0;
};

/// Registers `format` under its name(). A null format or a name already taken
/// is refused, never replaced.
dstd::Result<void> register_file_format(
    dstd::SharedPtr<FileFormatFactory> format);

/// Removes the format registered as `name`; false when there is none.
bool unregister_file_format(dstd::StringView name);

/// The format registered as `name`, or null.
dstd::SharedPtr<FileFormatFactory> find_file_format(dstd::StringView name);

/// Registers `sink` under its name(), with the same refusals as
/// @ref register_file_format.
dstd::Result<void> register_sink(dstd::SharedPtr<SinkFactory> sink);

/// Removes the sink registered as `name`; false when there is none.
bool unregister_sink(dstd::StringView name);

/// The sink registered as `name`, or null.
dstd::SharedPtr<SinkFactory> find_sink(dstd::StringView name);

/// The names of the sinks registered now, in sorted order.
dstd::Vector<dstd::String> registered_sink_names();

/// Whether any file format is registered.
bool has_file_formats();

/// The name of the built-in reader of trace files: gzip or plain JSON lines.
inline constexpr const char* BUILTIN_READER = "jsonl";

/// The reader of one file: the built-in trace reader (`plugin` null) or a
/// registered plugin file format.
struct Reader {
    dstd::String name;
    dstd::SharedPtr<FileFormatFactory> plugin;

    bool is_builtin() const { return !plugin; }
};

/// The reader of `path`. A non-empty `forced` names the reader and nothing is
/// probed: @ref BUILTIN_READER or a registered format, and an InvalidArgument
/// error listing the known names for any other. Otherwise every reader, the
/// built-in one included, scores the first 4 KiB of the file and the highest
/// score wins. The
/// built-in reader scores 50 for a gzip file and for a text file whose first
/// lines are JSON objects, and 10 for an empty one; a plugin format is free to
/// score above or below. A NotFound error when no reader claims the file, an
/// InvalidArgument error naming both readers when two tie at the top, and the
/// error that stopped reading the head.
dstd::Result<Reader> claim_reader(dstd::StringView path,
                                  dstd::StringView forced = {});

/// As @ref claim_reader, for the entry points that read trace files in bulk
/// (the Indexer and the View): while no plugin format is registered and
/// nothing is forced, the reader is the built-in one and the file is not read,
/// so a file that is not a trace fails when it is read, not when it is picked.
dstd::Result<Reader> claim_trace_reader(dstd::StringView path,
                                        dstd::StringView forced = {});

/// The names of the readers known now, sorted: @ref BUILTIN_READER and every
/// registered file format.
dstd::Vector<dstd::String> reader_names();

/// How the built-in reader opens a file as a plan: the file at `path` with its
/// index at `index_dir` (empty: the one beside the file).
using TraceOpen = LazyFrame (*)(dstd::StringView path,
                                dstd::StringView index_dir);

/// Sets how @ref LazyFrame::scan_file opens a file the built-in reader claims.
/// The trace library sets it; until then such a scan fails with an error.
void set_trace_open(TraceOpen open);

/// What @ref set_trace_open set, or null.
TraceOpen trace_open();

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_FILE_FORMAT_REGISTRY_H
