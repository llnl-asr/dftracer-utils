// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_GZIP_SINK_H
#define DFTRACER_UTILS_PYTHON_GZIP_SINK_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/fileio/gzip_line_writer.h>
#include <dftracer/utils/python/py_error_exception.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstddef>
#include <utility>

namespace dftracer::utils::python {

// Gzips an export into line-ended members of about 4 MiB, so the output is a
// multi-member re-indexable trace. Pieces may end mid-line; the bytes after
// the last newline wait for the next piece or the end of the stream.
class GzipSink : public trace::views::ExportSink {
   public:
    GzipSink(dstd::StringView path, int level) {
        fileio::GzipWriterOptions opts;
        opts.member_size = 4 * 1024 * 1024;
        opts.level = level;
        writer_.emplace(dftracer::utils::python::py_unwrap(
            fileio::GzipLineWriterBlocking::open(dstd::String(path),
                                                 std::move(opts))));
    }
    // Without close(), an export that failed part way still removes its
    // output, and that error is already the one reported.
    ~GzipSink() override {
        if (writer_) writer_->close(tail_).discard();
    }
    void close() {
        auto closed = writer_->close(tail_);
        writer_.reset();
        dftracer::utils::python::py_unwrap(std::move(closed));
    }
    void write(dstd::StringView data) override {
        const std::size_t nl = data.rfind('\n');
        if (nl == dstd::StringView::npos) {
            tail_.append(data);
            return;
        }
        if (tail_.empty()) {
            dftracer::utils::python::py_unwrap(
                writer_->append(data.substr(0, nl + 1)));
        } else {
            tail_.append(data.substr(0, nl + 1));
            dftracer::utils::python::py_unwrap(writer_->append(tail_));
            tail_.clear();
        }
        tail_.append(data.substr(nl + 1));
    }

   private:
    dstd::Optional<fileio::GzipLineWriterBlocking> writer_;
    dstd::String tail_;
};

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_GZIP_SINK_H
