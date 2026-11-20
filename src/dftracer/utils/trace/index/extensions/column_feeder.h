// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_COLUMN_FEEDER_H
#define DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_COLUMN_FEEDER_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/extensions/extension_registry.h>
#include <dftracer/utils/trace/index/extensions/summary.h>
#include <dftracer/utils/trace/index/store/file.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::trace::index {
class FileBuild;
}

namespace dftracer::utils::trace::index::extensions {

/// The feeder for typed columns: a chunk is one frame, and each column of it
/// is a path. A reader that has no JSON line, such as a plugin file format,
/// feeds a frame per chunk and gets the same summaries the @ref JsonFeeder
/// builds from equal data.
class ColumnFeeder {
   public:
    /// One stored payload: a summary's granule payload, or its file payload
    /// when `granule` is empty.
    struct Payload {
        store::IndexExtension extension = store::IndexExtension::Zonemap;
        dstd::String path;
        dstd::Optional<std::uint64_t> granule;
        dstd::String bytes;
    };

    explicit ColumnFeeder(
        dstd::Vector<dstd::SharedPtr<const Summary>> summaries);

    /// Whether the summary of kind `ext` is built for path `path`; a summary
    /// the filter refuses records nothing for that path. Unset builds every
    /// summary for every path.
    using PathFilter =
        dstd::Function<bool(dstd::StringView, store::IndexExtension)>;
    void set_path_filter(PathFilter filter) { filter_ = std::move(filter); }

    /// Also hands each frame to `builders`, whose extensions take the columns
    /// they asked for through step_column. `builders` must outlive the feeder.
    void set_plugin_builders(ExtensionBuilders* builders) {
        builders_ = builders;
    }

    /// Feeds chunk `granule`: every column of `frame` to every summary. A
    /// column whose type a summary cannot build for records nothing for that
    /// summary, and so never prunes. The rows of the frame are the records the
    /// chunk observed.
    void feed(std::uint64_t granule, const dataframe::DataFrame& frame);

    /// Ends the file. The granule payloads in feed order, then the file
    /// payloads.
    dstd::Vector<Payload> finish();

    /// Writes the payloads of `finish` into `build`, with the manifest entry
    /// of each summary.
    void write(trace::index::FileBuild& build,
               dstd::Span<const Payload> payloads) const;

   private:
    struct FileState {
        dstd::Vector<dstd::UniquePtr<SummaryBuilder>> builders;
    };

    dstd::Vector<dstd::SharedPtr<const Summary>> summaries_;
    ExtensionBuilders* builders_ = nullptr;
    PathFilter filter_;
    dstd::Vector<Payload> granules_;
    dstd::Map<dstd::String, FileState> files_;
};

}  // namespace dftracer::utils::trace::index::extensions

#endif  // DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_COLUMN_FEEDER_H
