// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/utilities/fileio/chunk_writer.h>

namespace dftracer::utils::utilities::fileio {

ChunkWriter::ChunkWriter(ChunkWriterConfig config)
    : config_(std::move(config)) {}

ChunkWriter::~ChunkWriter() {
    if (open_) {
        DFTRACER_UTILS_LOG_WARN("ChunkWriter destroyed while open: {}",
                                config_.base_name);
    }
}

coro::CoroTask<dstd::Result<void>> ChunkWriter::open() {
    const dstd::PathView output_dir(config_.output_dir);
    auto checking = runtime::fs::exists(output_dir);
    auto checked = co_await std::move(checking);
    const bool found = co_await coro::or_fail(std::move(checked));
    if (!found) {
        auto creating = runtime::fs::create_directories(output_dir);
        auto created = co_await std::move(creating);
        co_await coro::or_fail(std::move(created));
    }
    GzipWriterOptions opts;
    opts.member_size = config_.member_size_bytes;
    opts.level = config_.compression_level;
    opts.compress = config_.compress;
    opts.memory_budget = config_.memory_budget;
    opts.part_size = config_.chunk_size_bytes;
    opts.part_name = [this](std::size_t idx) {
        return config_.output_dir + "/" + config_.base_name + "_chunk" +
               dstd::to_string(idx) + ".pfw" + (config_.compress ? ".gz" : "");
    };
    opts.on_part = [this](std::size_t part, dstd::StringView path,
                          std::uint64_t uc_bytes, std::uint64_t lines) {
        chunks_.push_back(ChunkInfo{
            .path = dstd::String(path),
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
    dstd::String first_part = opts.part_name(0);
    auto opening = GzipLineWriter::open(std::move(first_part), std::move(opts));
    writer_.emplace(co_await std::move(opening).or_fail());
    open_ = true;
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> ChunkWriter::write_line(
    dstd::ByteView line) {
    pending_.append(line.as<char>(), line.size());
    pending_.push_back('\n');
    total_events_++;
    if (pending_.size() >= config_.io_flush_bytes) {
        co_await writer_->append(pending_).or_fail();
        pending_.clear();
    }

    // Yield every 256 events to prevent stack overflow from synchronous
    // coroutine completion chains.
    if ((total_events_ & 0xff) == 0) {
        co_await coro::yield();
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> ChunkWriter::close() {
    if (!open_) co_return dstd::Result<void>{};
    open_ = false;
    if (!pending_.empty()) {
        co_await writer_->append(pending_).or_fail();
        pending_.clear();
    }
    auto closing = writer_->close();
    const WriteSummary summary = co_await std::move(closing).or_fail();
    total_bytes_ = static_cast<std::size_t>(summary.c_bytes);
    peak_bytes_ = summary.peak_bytes;
    writer_.reset();
    co_return dstd::Result<void>{};
}

}  // namespace dftracer::utils::utilities::fileio
