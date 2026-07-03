#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/generator.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/fileio/chunk_writer.h>
#include <fcntl.h>

#include <memory>

namespace dftracer::utils::utilities::fileio {

ChunkWriter::ChunkWriter(ChunkWriterConfig config)
    : config_(std::move(config)) {
    write_buffer_.reserve(WRITE_BUFFER_SIZE);
}

ChunkWriter::~ChunkWriter() {
    if (open_) {
        DFTRACER_UTILS_LOG_WARN("ChunkWriter destroyed while open: %s",
                                chunk_path(chunk_index_).c_str());
    }
}

std::string ChunkWriter::chunk_path(int index) const {
    std::string name = config_.base_name + "_chunk" + std::to_string(index) +
                       ".pfw" + (config_.compress ? ".gz" : "");
    return config_.output_dir + "/" + name;
}

coro::CoroTask<void> ChunkWriter::open() {
    if (!fs::exists(config_.output_dir)) {
        fs::create_directories(config_.output_dir);
    }
    co_await open_next_chunk();
}

coro::CoroTask<void> ChunkWriter::open_next_chunk() {
    std::string path = chunk_path(chunk_index_);

    ssize_t result =
        co_await io::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (result < 0) {
        throw DFTUtilsException(ErrorCode::IO,
                                "Cannot open chunk file: " + path);
    }
    fd_ = static_cast<int>(result);
    open_ = true;
    current_chunk_bytes_ = 0;
    current_chunk_events_ = 0;
    write_buffer_.clear();

    if (config_.compress) {
        compressor_ = std::make_unique<
            compression::zlib::ManualStreamingCompressorUtility>(
            config_.compression_level,
            compression::zlib::CompressionFormat::GZIP);
    }

    if (config_.json_array_wrapper) {
        co_await flush_raw("[\n", 2);
    }
}

coro::CoroTask<void> ChunkWriter::write_line(ByteView line) {
    if (current_chunk_events_ > 0) {
        write_buffer_.push_back('\n');
    }
    write_buffer_.insert(write_buffer_.end(), line.as<char>(),
                         line.as<char>() + line.size());
    current_chunk_bytes_ += line.size() + 1;
    current_chunk_events_++;
    total_events_++;

    if (write_buffer_.size() >= WRITE_BUFFER_SIZE) {
        co_await flush_buffer();
    }

    if (current_chunk_bytes_ >= config_.chunk_size_bytes) {
        co_await finalize_current_chunk();
        chunk_index_++;
        co_await open_next_chunk();
    }

    // Yield every 256 events to prevent stack overflow from synchronous
    // coroutine completion chains. This is internal to ChunkWriter so
    // callers don't need to manage yielding.
    if ((total_events_ & 0xff) == 0) {
        co_await coro::yield();
    }
}

coro::CoroTask<void> ChunkWriter::write_bytes(ByteView data) {
    write_buffer_.insert(write_buffer_.end(), data.as<char>(),
                         data.as<char>() + data.size());
    current_chunk_bytes_ += data.size();

    if (write_buffer_.size() >= WRITE_BUFFER_SIZE) {
        co_await flush_buffer();
    }
}

coro::CoroTask<void> ChunkWriter::flush_buffer() {
    if (write_buffer_.empty()) co_return;

    if (compressor_) {
        using GenType = coro::AsyncGenerator<ByteView>;
        auto gen = std::make_unique<GenType>(compressor_->compress(
            ByteView(write_buffer_.data(), write_buffer_.size())));
        while (true) {
            auto chunk = co_await gen->next();
            if (!chunk) break;
            const char* data = chunk->as<char>();
            std::size_t size = chunk->size();
            chunk.reset();
            co_await io::write(fd_, data, size);
            total_bytes_ += size;
        }
    } else {
        co_await io::write(fd_, write_buffer_.data(), write_buffer_.size());
        total_bytes_ += write_buffer_.size();
    }
    write_buffer_.clear();
}

coro::CoroTask<void> ChunkWriter::flush_raw(const char* data, std::size_t len) {
    if (compressor_) {
        using GenType = coro::AsyncGenerator<ByteView>;
        auto gen = std::make_unique<GenType>(
            compressor_->compress(ByteView(data, len)));
        while (true) {
            auto chunk = co_await gen->next();
            if (!chunk) break;
            const char* cdata = chunk->as<char>();
            std::size_t csize = chunk->size();
            chunk.reset();
            co_await io::write(fd_, cdata, csize);
            total_bytes_ += csize;
        }
    } else {
        co_await io::write(fd_, data, len);
        total_bytes_ += len;
    }
}

coro::CoroTask<void> ChunkWriter::finalize_current_chunk() {
    co_await flush_buffer();

    if (config_.json_array_wrapper) {
        if (current_chunk_events_ > 0) {
            co_await flush_raw("\n]\n", 3);
        } else {
            co_await flush_raw("]\n", 2);
        }
    }

    if (compressor_) {
        using GenType = coro::AsyncGenerator<ByteView>;
        auto fin = std::make_unique<GenType>(compressor_->finalize_stream());
        while (true) {
            auto chunk = co_await fin->next();
            if (!chunk) break;
            const char* data = chunk->as<char>();
            std::size_t size = chunk->size();
            chunk.reset();
            co_await io::write(fd_, data, size);
            total_bytes_ += size;
        }
        compressor_.reset();
    }

    co_await io::close(fd_);
    fd_ = -1;

    auto path = chunk_path(chunk_index_);
    chunks_.push_back(ChunkInfo{
        .path = path,
        .bytes_written = current_chunk_bytes_,
        .events_written = current_chunk_events_,
        .chunk_index = chunk_index_,
    });

    if (config_.on_chunk_complete) {
        config_.on_chunk_complete(static_cast<std::size_t>(chunk_index_), path,
                                  current_chunk_events_, current_chunk_bytes_);
    }
}

coro::CoroTask<void> ChunkWriter::close() {
    if (!open_) co_return;
    co_await finalize_current_chunk();
    open_ = false;
}

}  // namespace dftracer::utils::utilities::fileio
