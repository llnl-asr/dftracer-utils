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

    dstd::Result<void> write(const dftu_dataframe* batch) override {
        for (std::int64_t row = 0; row < dftu_dataframe_num_rows(batch);
             ++row) {
            std::fputs("row\n", file_);
        }
        if (std::ferror(file_) != 0)
            return dstd::Unexpected(
                make_error(DFTU_E_IO, "@NAME@: write failed"));
        return {};
    }

    dstd::Result<void> finalize() override {
        if (file_ == nullptr)
            return dstd::Unexpected(
                make_error(DFTU_E_INVALID, "@NAME@: already closed"));
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
        const dftu_value* path = dftu_obj_get(view.options, "path");
        if (path == nullptr || path->kind != DFTU_VAL_STR)
            return dstd::Unexpected(make_error(
                DFTU_E_INVALID,
                "@NAME@: bind needs the option path, e.g. path \"out.txt\""));
        std::FILE* file = std::fopen(path->as.str, "wb");
        if (file == nullptr)
            return dstd::Unexpected(
                make_error(DFTU_E_IO, "@NAME@: cannot open the output"));
        return dstd::UniquePtr<SinkWriter>(dstd::make_unique<LineWriter>(file));
    }
};

}  // namespace

DFTU_PLUGIN_INFO("@NAME@", "1.0")

extern "C" DFTU_PLUGIN_EXPORT dftu_status
dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
                dftu_plugin** out) {
    return plugin(ctx, h, config)
        .sink("@NAME@", dstd::UniquePtr<Sink>(dstd::make_unique<LineSink>()))
        .build(out);
}
