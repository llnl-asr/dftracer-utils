#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/utilities/fileio/chunk_writer.h>

namespace dftracer::utils::utilities::fileio {

ChunkWriter::ChunkWriter(ChunkWriterConfig config)
    : config_(std::move(config)) {}

ChunkWriter::~ChunkWriter() {
    if (open_) {
        DFTRACER_UTILS_LOG_WARN("ChunkWriter destroyed while open: %s",
                                config_.base_name.c_str());
    }
}

coro::CoroTask<void> ChunkWriter::open() {
    if (!fs::exists(config_.output_dir)) {
        fs::create_directories(config_.output_dir);
    }
    GzipWriterOptions opts;
    opts.member_size = config_.member_size_bytes;
    opts.level = config_.compression_level;
    opts.compress = config_.compress;
    opts.part_size = config_.chunk_size_bytes;
    opts.part_name = [this](std::size_t idx) {
        return config_.output_dir + "/" + config_.base_name + "_chunk" +
               std::to_string(idx) + ".pfw" + (config_.compress ? ".gz" : "");
    };
    if (config_.json_array_wrapper) {
        opts.part_header = "[\n";
        opts.part_footer = "]\n";
    }
    opts.on_part = [this](std::size_t part, const std::string& path,
                          std::uint64_t uc_bytes, std::uint64_t lines) {
        chunks_.push_back(ChunkInfo{
            .path = path,
            .bytes_written = static_cast<std::size_t>(uc_bytes),
            .events_written = static_cast<std::size_t>(lines),
            .chunk_index = static_cast<int>(part),
        });
        if (config_.on_chunk_complete) {
            config_.on_chunk_complete(part, path,
                                      static_cast<std::size_t>(lines),
                                      static_cast<std::size_t>(uc_bytes));
        }
    };
    writer_.emplace(unwrap(
        co_await GzipLineWriter::open(opts.part_name(0), std::move(opts))));
    open_ = true;
}

coro::CoroTask<void> ChunkWriter::write_line(ByteView line) {
    pending_.append(line.as<char>(), line.size());
    pending_.push_back('\n');
    total_events_++;
    if (pending_.size() >= config_.io_flush_bytes) {
        unwrap(co_await writer_->append(pending_));
        pending_.clear();
    }

    // Yield every 256 events to prevent stack overflow from synchronous
    // coroutine completion chains.
    if ((total_events_ & 0xff) == 0) {
        co_await coro::yield();
    }
}

coro::CoroTask<void> ChunkWriter::close() {
    if (!open_) co_return;
    open_ = false;
    if (!pending_.empty()) {
        unwrap(co_await writer_->append(pending_));
        pending_.clear();
    }
    auto summary = unwrap(co_await writer_->close());
    total_bytes_ = static_cast<std::size_t>(summary.c_bytes);
    writer_.reset();
}

}  // namespace dftracer::utils::utilities::fileio
