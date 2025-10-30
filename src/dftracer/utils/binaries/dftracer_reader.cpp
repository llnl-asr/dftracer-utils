#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/composites/composites.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <dftracer/utils/utilities/reader/internal/stream.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <dftracer/utils/utilities/reader/internal/stream_type.h>

#include <algorithm>
#include <argparse/argparse.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer::internal;
using namespace dftracer::utils::utilities::reader::internal;

int main(int argc, char **argv) {
    DFTRACER_UTILS_LOGGER_INIT();
    auto default_checkpoint_size_str =
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE) + " B (" +
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE / (1024 * 1024)) +
        " MB)";
    argparse::ArgumentParser program("dft_reader",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "DFTracer utility for reading and indexing compressed files (GZIP, "
        "TAR.GZ)");
    program.add_argument("file")
        .help("Compressed file to process (GZIP, TAR.GZ)")
        .required();
    program.add_argument("-i", "--index")
        .help("Index file to use")
        .default_value<std::string>("");
    program.add_argument("-s", "--start")
        .help("Start position in bytes")
        .default_value<std::int64_t>(-1)
        .scan<'d', std::int64_t>();
    program.add_argument("-e", "--end")
        .help("End position in bytes")
        .default_value<std::int64_t>(-1)
        .scan<'d', std::int64_t>();
    program.add_argument("-c", "--checkpoint-size")
        .help("Checkpoint size for indexing in bytes (default: " +
              default_checkpoint_size_str + ")")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(Indexer::DEFAULT_CHECKPOINT_SIZE));
    program.add_argument("-f", "--force-rebuild")
        .help("Force rebuild index")
        .flag();
    program.add_argument("--check").help("Check if index is valid").flag();
    program.add_argument("--read-buffer-size")
        .help("Size of the read buffer in bytes (default: 1MB)")
        .default_value<std::size_t>(1 * 1024 * 1024)
        .scan<'d', std::size_t>();
    program.add_argument("--mode")
        .help("Set the reading mode (bytes, line_bytes, lines)")
        .default_value<std::string>("bytes")
        .choices("bytes", "line_bytes", "lines");
    program.add_argument("--index-dir")
        .help("Directory to store index files (default: system temp directory)")
        .default_value<std::string>("");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception &err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::cerr << program;
        return 1;
    }

    std::string gz_path = program.get<std::string>("file");
    std::string index_path = program.get<std::string>("--index");
    int64_t start = program.get<int64_t>("--start");
    int64_t end = program.get<int64_t>("--end");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    bool force_rebuild = program.get<bool>("--force-rebuild");
    bool check_rebuild = program.get<bool>("--check");
    std::string read_mode = program.get<std::string>("--mode");
    std::size_t read_buffer_size =
        program.get<std::size_t>("--read-buffer-size");
    std::string index_dir = program.get<std::string>("--index-dir");

    DFTRACER_UTILS_LOG_DEBUG("Processing file: %s", gz_path.c_str());
    DFTRACER_UTILS_LOG_DEBUG("Start position: %lld", (long long)start);
    DFTRACER_UTILS_LOG_DEBUG("End position: %lld", (long long)end);
    DFTRACER_UTILS_LOG_DEBUG("Mode: %s", read_mode.c_str());
    DFTRACER_UTILS_LOG_DEBUG("Checkpoint size: %zu B (%zu MB)", checkpoint_size,
                             checkpoint_size / (1024 * 1024));
    DFTRACER_UTILS_LOG_DEBUG("Force rebuild: %s",
                             force_rebuild ? "true" : "false");

    if (checkpoint_size <= 0) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s",
            "Checkpoint size must be positive (greater than 0 and in MB)");
        return 1;
    }

    FILE *test_file = fopen(gz_path.c_str(), "rb");
    if (!test_file) {
        DFTRACER_UTILS_LOG_ERROR("File '%s' does not exist or cannot be opened",
                                 gz_path.c_str());
        return 1;
    }
    fclose(test_file);

    std::string idx_path;
    if (!index_path.empty()) {
        idx_path = index_path;
    } else {
        idx_path = utilities::composites::dft::internal::determine_index_path(
            gz_path, index_dir);
    }

#if DFTRACER_UTILS_LOGGER_DEBUG_ENABLED
    ArchiveFormat format = IndexerFactory::detect_format(gz_path);

    DFTRACER_UTILS_LOG_DEBUG("Detected format: %s",
                             format == ArchiveFormat::TAR_GZ ? "TAR.GZ"
                             : format == ArchiveFormat::GZIP ? "GZIP"
                                                             : "UNKNOWN");
#endif

    // Create indexer first
    std::shared_ptr<Indexer> indexer;
    try {
        // check if idx file exists
        if (!fs::exists(idx_path)) {
            if (check_rebuild) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Index file '%s' does not exist, cannot check",
                    idx_path.c_str());
                return 1;
            }
            DFTRACER_UTILS_LOG_DEBUG("Index file '%s' does not exist",
                                     idx_path.c_str());
            DFTRACER_UTILS_LOG_DEBUG("%s", "Will create new index file");
            force_rebuild = true;
        }

        // Use IndexerFactory to create appropriate indexer
        indexer = IndexerFactory::create(gz_path, idx_path, checkpoint_size,
                                         force_rebuild);

        if (check_rebuild) {
            if (!indexer->need_rebuild()) {
                DFTRACER_UTILS_LOG_DEBUG(
                    "%s", "Index is up to date, no rebuild needed");
                return 0;
            }
        }

        if (force_rebuild) {
            if (fs::exists(idx_path)) {
                DFTRACER_UTILS_LOG_DEBUG("Removing existing index: %s",
                                         idx_path.c_str());
                fs::remove(idx_path);
            }
            // Recreate indexer after removing old index
            indexer = IndexerFactory::create(gz_path, idx_path, checkpoint_size,
                                             true);
            DFTRACER_UTILS_LOG_INFO("Building index for file: %s",
                                    gz_path.c_str());
            indexer->build();
        }
    } catch (const std::runtime_error &e) {
        DFTRACER_UTILS_LOG_ERROR("Indexer error: %s", e.what());
        return 1;
    }

    // read operations
    try {
        // Use ReaderFactory to create appropriate reader, sharing
        // ownership of indexer
        auto reader = ReaderFactory::create(indexer);

        if (read_mode.find("bytes") == std::string::npos) {
            std::size_t start_line =
                (start == -1) ? 1 : static_cast<std::size_t>(start);
            std::size_t end_line = static_cast<std::size_t>(end);
            if (end == -1) {
                end_line = reader->get_num_lines();
            }

            DFTRACER_UTILS_LOG_DEBUG("Reading lines from %zu to %zu",
                                     start_line, end_line);

            auto stream =
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::MULTI_LINES)
                                   .range_type(RangeType::LINE_RANGE)
                                   .from(start_line)
                                   .to(end_line)
                                   .buffer_size(read_buffer_size));

#if DFTRACER_UTILS_LOGGER_DEBUG_ENABLED
            std::size_t line_count = 0;
#endif

            while (!stream->done()) {
                auto chunk = stream->read();
                if (chunk.empty()) break;
                std::fwrite(chunk.data(), 1, chunk.size(), stdout);
#if DFTRACER_UTILS_LOGGER_DEBUG_ENABLED
                line_count += std::count(chunk.begin(), chunk.end(), '\n');
#endif
            }

            DFTRACER_UTILS_LOG_DEBUG("Successfully read %zu lines from range",
                                     line_count);
        } else {
            std::size_t start_bytes_ =
                (start == -1) ? 0 : static_cast<std::size_t>(start);
            std::size_t end_bytes_ =
                end == -1 ? std::numeric_limits<std::size_t>::max()
                          : static_cast<size_t>(end);

            auto max_bytes = reader->get_max_bytes();
            if (end_bytes_ > max_bytes) {
                end_bytes_ = max_bytes;
            }
            DFTRACER_UTILS_LOG_DEBUG("%s",
                                     "Performing byte range read operation");
            DFTRACER_UTILS_LOG_DEBUG("Using read buffer size: %zu bytes",
                                     read_buffer_size);

            StreamType stream_type = (read_mode == "bytes")
                                         ? StreamType::BYTES
                                         : StreamType::MULTI_LINES_BYTES;

            auto stream = reader->stream(StreamConfig()
                                             .stream_type(stream_type)
                                             .range_type(RangeType::BYTE_RANGE)
                                             .from(start_bytes_)
                                             .to(end_bytes_)
                                             .buffer_size(read_buffer_size));

#if DFTRACER_UTILS_LOGGER_DEBUG_ENABLED == 1
            std::size_t total_bytes = 0;
#endif

            while (!stream->done()) {
                auto chunk = stream->read();
                if (chunk.empty()) break;
                std::fwrite(chunk.data(), 1, chunk.size(), stdout);
#if DFTRACER_UTILS_LOGGER_DEBUG_ENABLED == 1
                total_bytes += chunk.size();
#endif
            }

            DFTRACER_UTILS_LOG_DEBUG("Successfully read %zu bytes from range",
                                     total_bytes);
        }
        fflush(stdout);
    } catch (const std::runtime_error &e) {
        DFTRACER_UTILS_LOG_ERROR("Reader error: %s", e.what());
        return 1;
    }

    return 0;
}
