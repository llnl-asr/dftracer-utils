// SPDX-License-Identifier: MIT
// The @NAME@ format plugin, made by `dftracer_plugin new`.
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/plugin/file_format.h>

#include <cstdint>
#include <cstdlib>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

/// A file format claims files the engine has no reader for. This one reads a
/// `.ids` file of one decimal number per line into one int64 column, `id`.
///
/// A format is two classes: the format (probe and open) and one open file per
/// path (schema, chunks and scan).
namespace {

class IdCursor : public FormatCursor {
   public:
    explicit IdCursor(dstd::Vector<std::int64_t> rows)
        : rows_(std::move(rows)) {}

    dstd::Result<dstd::Optional<OwnedDataFrame>> next(std::int64_t) override {
        if (done_) return dstd::Optional<OwnedDataFrame>();
        done_ = true;
        FrameBuilder builder;
        builder.add("id", Column::from_int64(std::move(rows_)));
        auto built = builder.build();
        if (!built)
            return dstd::Unexpected(
                invalid_argument("@NAME@ could not build a frame"));
        return dstd::Optional<OwnedDataFrame>(std::move(*built));
    }

   private:
    dstd::Vector<std::int64_t> rows_;
    bool done_ = false;
};

class IdFile : public FormatFile {
   public:
    explicit IdFile(dstd::Vector<std::int64_t> rows) : rows_(std::move(rows)) {}

    void schema(SchemaBuilder& out) const override {
        dftu_schema_add_field(out.raw(), "id", DFTU_TYPE_INT64, 0,
                              DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
    }

    std::uint64_t chunks() const override { return 1; }

    dstd::Optional<std::int64_t> chunk_rows(std::uint64_t) const override {
        return static_cast<std::int64_t>(rows_.size());
    }

    dstd::Result<ScanResult<FormatCursor>> scan(std::uint64_t,
                                                const ScanView& view) override {
        for (const dstd::StringView& name : view.projection) {
            if (name != "id")
                return dstd::Unexpected(
                    invalid_argument("@NAME@ has one column, id"));
        }
        ScanResult<FormatCursor> result;
        dstd::Vector<std::int64_t> rows = rows_;
        result.cursor = dstd::UniquePtr<FormatCursor>(
            dstd::make_unique<IdCursor>(std::move(rows)));
        result.filters.assign(view.filters.size(), DFTU_PUSHED_NO);
        return result;
    }

   private:
    dstd::Vector<std::int64_t> rows_;
};

class IdFormat : public FileFormat {
   public:
    std::int32_t probe(const std::uint8_t*, std::uint64_t,
                       dstd::StringView path) override {
        return path.ends_with(".ids") ? 100 : 0;
    }

    dstd::Result<dstd::UniquePtr<FormatFile>> open(
        dstd::StringView path) override {
        auto text = dstd::fs::read_file(dstd::PathView(path));
        if (!text)
            return dstd::Unexpected(io_error("@NAME@: cannot read the file"));
        dstd::Vector<std::int64_t> rows;
        const dstd::StringView body(*text);
        std::size_t at = 0;
        while (at < body.size()) {
            const std::size_t nl = body.find('\n', at);
            const std::size_t end =
                nl == dstd::StringView::npos ? body.size() : nl;
            const dstd::StringView line = body.substr(at, end - at);
            at = nl == dstd::StringView::npos ? body.size() : nl + 1;
            if (line.empty()) continue;
            rows.push_back(
                std::strtoll(dstd::String(line).c_str(), nullptr, 10));
        }
        return dstd::UniquePtr<FormatFile>(
            dstd::make_unique<IdFile>(std::move(rows)));
    }
};

}  // namespace

DFTU_PLUGIN("@NAME@", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.file_format<IdFormat>("@NAME@",
                                        dstd::make_unique<IdFormat>());
                return {};
            })
