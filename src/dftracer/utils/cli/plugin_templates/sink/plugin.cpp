// SPDX-License-Identifier: MIT
// The @NAME@ sink plugin, made by `dftracer_plugin new`.
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstdint>
#include <cstdio>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

/// A sink writes whole frames out in its own format: bind fixes the output,
/// write takes one frame, and finalize or abort releases the handle.
namespace {

class LineWriter : public SinkWriter {
   public:
    explicit LineWriter(std::FILE* file) : file_(file) {}

    dstd::Result<void> write(FrameView batch) override {
        for (std::int64_t row = 0; row < batch.row_count(); ++row) {
            std::fputs("row\n", file_);
        }
        if (std::ferror(file_) != 0)
            return dstd::Unexpected(io_error("@NAME@: write failed"));
        return {};
    }

    dstd::Result<void> finalize() override {
        if (file_ == nullptr)
            return dstd::Unexpected(invalid_argument("@NAME@: already closed"));
        std::fclose(file_);
        file_ = nullptr;
        return {};
    }

    void abort() override {
        if (file_ == nullptr) return;
        std::fclose(file_);
        file_ = nullptr;
    }

   private:
    std::FILE* file_ = nullptr;
};

class LineSink : public Sink {
   public:
    dstd::Result<dstd::UniquePtr<SinkWriter>> bind(
        const BindView& view) override {
        const dstd::StringView path = view.options.get("path");
        if (path.empty())
            return dstd::Unexpected(invalid_argument(
                "@NAME@: bind needs the option path, e.g. path \"out.txt\""));
        const dstd::String file_path(path);
        std::FILE* file = std::fopen(file_path.c_str(), "wb");
        if (file == nullptr)
            return dstd::Unexpected(io_error("@NAME@: cannot open the output"));
        return dstd::UniquePtr<SinkWriter>(dstd::make_unique<LineWriter>(file));
    }
};

}  // namespace

DFTU_PLUGIN("@NAME@", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.sink("@NAME@",
                       dstd::UniquePtr<Sink>(dstd::make_unique<LineSink>()));
                return {};
            })
