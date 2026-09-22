#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/trace_viewer_detail.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::python::trace_viewer_detail {

namespace {

class FileSink : public dftracer::utils::trace::views::ExportSink {
   public:
    explicit FileSink(FILE* f) : f_(f) {}
    void write(std::string_view data) override {
        std::fwrite(data.data(), 1, data.size(), f_);
    }

   private:
    FILE* f_;
};

// Streaming gzip sink: buffers writes and flushes whole-line gzip members once
// past MEMBER_TARGET, so the aggregation output is a multi-member re-indexable
// trace. finish() flushes the remainder.
class GzipSink : public dftracer::utils::trace::views::ExportSink {
   public:
    GzipSink(FILE* f, int level) : f_(f), comp_(level) {}
    void write(std::string_view data) override {
        buf_.append(data);
        while (buf_.size() >= MEMBER_TARGET) {
            std::size_t cut = buf_.rfind('\n', buf_.size());
            if (cut == std::string::npos || cut + 1 < MEMBER_TARGET) break;
            flush(cut + 1);
        }
    }
    void finish() {
        if (!buf_.empty()) flush(buf_.size());
    }

   private:
    static constexpr std::size_t MEMBER_TARGET = 4 * 1024 * 1024;
    void flush(std::size_t n) {
        if (comp_.compress_member_into(scratch_, buf_.data(), n))
            std::fwrite(scratch_.data(), 1, scratch_.size(), f_);
        buf_.erase(0, n);
    }
    FILE* f_;
    dftracer::utils::utilities::fileio::compress::GzipMemberCompressor comp_;
    std::string buf_;
    std::vector<std::uint8_t> scratch_;
};

}  // namespace

// The single export terminal: writes a dftracer trace whose content follows the
// plan (aggregation when group_by/agg is set, else the matching events).
// Gzip-and-re-indexable by default; compress=False writes plain NDJSON; index
// builds the index inline (events only).
PyObject* tv_export(TraceViewerObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"path",  "compress",  "index", "member_size",
                                   "level", "part_size", nullptr};
    const char* path = nullptr;
    int compress = 1;
    int index = 0;
    long long member_size = 0;
    int level = 6;
    long long part_size = 0;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "s|ppLiL", const_cast<char**>(kwlist), &path, &compress,
            &index, &member_size, &level, &part_size))
        return nullptr;

    const ViewerPlan& p = *plan_of(self);
    const bool aggregated = !p.group_by.empty() || !p.agg.empty();
    std::string path_s(path);

    Runtime* rt = resolve_runtime(self);
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = p;

    if (aggregated) {
        FILE* f = std::fopen(path, "wb");
        if (!f) {
            PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
            return nullptr;
        }
        bool ok = run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            if (compress) {
                GzipSink sink(f, level);
                rt->submit(v.export_counters(sink)).get();
                sink.finish();
            } else {
                FileSink sink(f);
                rt->submit(v.export_counters(sink)).get();
            }
        });
        std::fclose(f);
        if (!ok) return nullptr;
        Py_RETURN_NONE;
    }

    if (!run_blocking([&] {
            using dftracer::utils::trace::views::TraceWriteOptions;
            View v = build_view_from_data(files, index_dir, plan);
            TraceWriteOptions opts;
            opts.output_path = path_s;
            opts.member_size = (std::size_t)member_size;
            opts.compress = compress != 0;
            opts.level = level;
            opts.build_index = index != 0;
            opts.part_size = (std::size_t)part_size;
            rt->submit(v.export_trace(std::move(opts))).get();
        }))
        return nullptr;
    Py_RETURN_NONE;
}

}  // namespace dftracer::utils::python::trace_viewer_detail
