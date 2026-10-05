#ifndef DFTRACER_UTILS_PYTHON_GZIP_SINK_H
#define DFTRACER_UTILS_PYTHON_GZIP_SINK_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>

#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils::python {

// Gzips an export into line-ended members of about 4 MiB, so the output is a
// multi-member re-indexable trace. Pieces may end mid-line; the bytes after
// the last newline wait for the next piece or the end of the stream.
class GzipSink : public trace::views::ExportSink {
   public:
    GzipSink(const std::string& path, int level) {
        utilities::fileio::GzipWriterOptions opts;
        opts.member_size = 4 * 1024 * 1024;
        opts.level = level;
        writer_.emplace(unwrap(utilities::fileio::GzipLineWriterBlocking::open(
            path, std::move(opts))));
    }
    ~GzipSink() override {
        if (writer_) (void)writer_->close(tail_);
    }
    void write(std::string_view data) override {
        const std::size_t nl = data.rfind('\n');
        if (nl == std::string_view::npos) {
            tail_.append(data);
            return;
        }
        if (tail_.empty()) {
            unwrap(writer_->append(data.substr(0, nl + 1)));
        } else {
            tail_.append(data.substr(0, nl + 1));
            unwrap(writer_->append(tail_));
            tail_.clear();
        }
        tail_.append(data.substr(nl + 1));
    }

   private:
    std::optional<utilities::fileio::GzipLineWriterBlocking> writer_;
    std::string tail_;
};

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_GZIP_SINK_H
