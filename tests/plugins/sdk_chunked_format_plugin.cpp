// SPDX-License-Identifier: MIT
// A file format written with the C++ SDK that gives its chunks tokens, so the
// Indexer can index a file it reads and a scan can skip chunks. The file is the
// line "SDKC" and then one integer per line, in the column "id"; every
// ROWS_PER_CHUNK rows are a chunk, and the token of a chunk is the byte offset
// of its first row. When the environment variable SDK_CHUNKED_LOG names a file,
// each chunk scan() opens is appended to it as a line, so a test sees which
// chunks a scan read.

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/plugin/file_format.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

constexpr std::int64_t ROWS_PER_CHUNK = 4;

class SdkCursor : public FormatCursor {
   public:
    explicit SdkCursor(dstd::Vector<std::int64_t> ids) : ids_(std::move(ids)) {}

    dstd::Result<dstd::Optional<OwnedDataFrame>> next(std::int64_t) override {
        if (done_) return dstd::Optional<OwnedDataFrame>();
        done_ = true;
        FrameBuilder builder;
        builder.add("id", Column::from_int64(std::move(ids_)));
        auto built = builder.build();
        if (!built)
            return dstd::Unexpected(
                invalid_argument("sdk_chunked: cannot build a frame"));
        return dstd::Optional<OwnedDataFrame>(std::move(*built));
    }

   private:
    dstd::Vector<std::int64_t> ids_;
    bool done_ = false;
};

class SdkFile : public FormatFile {
   public:
    SdkFile(dstd::Vector<std::int64_t> ids, dstd::Vector<std::uint64_t> offsets)
        : ids_(std::move(ids)), offsets_(std::move(offsets)) {}

    void schema(SchemaBuilder& out) const override {
        out.add("id", TypeId::Int64, false);
    }

    std::uint64_t chunks() const override {
        const std::int64_t n =
            (static_cast<std::int64_t>(ids_.size()) + ROWS_PER_CHUNK - 1) /
            ROWS_PER_CHUNK;
        return static_cast<std::uint64_t>(n < 1 ? 1 : n);
    }

    dstd::Optional<std::int64_t> chunk_rows(
        std::uint64_t chunk) const override {
        const std::int64_t first =
            static_cast<std::int64_t>(chunk) * ROWS_PER_CHUNK;
        const std::int64_t left =
            static_cast<std::int64_t>(ids_.size()) - first;
        return left < 0 ? 0 : (left < ROWS_PER_CHUNK ? left : ROWS_PER_CHUNK);
    }

    dstd::Result<std::uint64_t> chunk_token(std::uint64_t chunk) {
        return offsets_[static_cast<std::size_t>(chunk * ROWS_PER_CHUNK)];
    }

    dstd::Result<ScanResult<FormatCursor>> scan(std::uint64_t chunk,
                                                const ScanView& view) override {
        if (const char* log = std::getenv("SDK_CHUNKED_LOG")) {
            if (std::FILE* f = std::fopen(log, "a")) {
                std::fprintf(f, "%lld\n", static_cast<long long>(chunk));
                std::fclose(f);
            }
        }
        const std::size_t first =
            static_cast<std::size_t>(chunk * ROWS_PER_CHUNK);
        const std::size_t end = first + ROWS_PER_CHUNK < ids_.size()
                                    ? first + ROWS_PER_CHUNK
                                    : ids_.size();
        dstd::Vector<std::int64_t> ids;
        for (std::size_t i = first; i < end; ++i) ids.push_back(ids_[i]);
        ScanResult<FormatCursor> result;
        result.filters.assign(view.filters.size(), Pushed::No);
        result.cursor = dstd::UniquePtr<FormatCursor>(
            dstd::make_unique<SdkCursor>(std::move(ids)));
        return result;
    }

   private:
    dstd::Vector<std::int64_t> ids_;
    dstd::Vector<std::uint64_t> offsets_;
};

class SdkFormat : public FileFormat {
   public:
    using File = SdkFile;

    std::int32_t probe(dstd::ByteView head, dstd::CStringView) override {
        const dstd::StringView text(reinterpret_cast<const char*>(head.data()),
                                    head.size());
        return text.starts_with("SDKC\n") ? 90 : 0;
    }

    dstd::Result<dstd::UniquePtr<FormatFile>> open(
        dstd::StringView path) override {
        auto text = dstd::fs::read_file(dstd::PathView(path));
        if (!text)
            return dstd::Unexpected(io_error("sdk_chunked: cannot read file"));
        const dstd::StringView body(*text);
        dstd::Vector<std::int64_t> ids;
        dstd::Vector<std::uint64_t> offsets;
        std::size_t at = 5;
        while (at < body.size()) {
            const std::size_t newline = body.find('\n', at);
            const std::size_t end =
                newline == dstd::StringView::npos ? body.size() : newline;
            const dstd::Optional<std::int64_t> id =
                dstd::from_chars<std::int64_t>(body.substr(at, end - at));
            if (!id)
                return dstd::Unexpected(
                    invalid_argument("sdk_chunked: a row is not an integer"));
            ids.push_back(*id);
            offsets.push_back(at);
            at = end + 1;
        }
        if (offsets.empty()) offsets.push_back(at);
        dstd::UniquePtr<FormatFile> file =
            dstd::make_unique<SdkFile>(std::move(ids), std::move(offsets));
        return file;
    }
};

}  // namespace

DFTU_PLUGIN("sdk_chunked", "1.0",
            [](PluginBuilder& p, const Config&) -> dstd::Result<void> {
                p.file_format<SdkFormat>("sdk_chunked",
                                         dstd::make_unique<SdkFormat>());
                return {};
            })
