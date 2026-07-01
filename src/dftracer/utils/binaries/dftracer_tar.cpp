#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/indexer/internal/tar/tar_indexer.h>
#include <fcntl.h>
#include <unistd.h>

#include <argparse/argparse.hpp>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer::internal;

static coro::CoroTask<int> run_tar(const std::string& archive_path,
                                   const std::string& index_path,
                                   std::size_t checkpoint_size,
                                   bool force_rebuild, bool list_files,
                                   bool show_info, bool build_only) {
    try {
        // Create indexer using factory
        auto indexer = IndexerFactory::create(archive_path, index_path,
                                              checkpoint_size, force_rebuild);

        if (!indexer) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to create indexer for file '%s' - "
                "unsupported or unrecognized format",
                archive_path.c_str());
            co_return 1;
        }

        DFTRACER_UTILS_LOG_INFO("Detected format: %s",
                                indexer->get_format_name());
        DFTRACER_UTILS_LOG_INFO("Index store: %s",
                                indexer->get_index_path().c_str());

        // Build index if needed
        if (force_rebuild || indexer->need_rebuild()) {
            DFTRACER_UTILS_LOG_INFO("%s", "Building index...");
            co_await indexer->build_async();
            DFTRACER_UTILS_LOG_INFO("%s", "Index built successfully");
        } else {
            DFTRACER_UTILS_LOG_INFO("%s", "Index is up to date");
        }

        if (build_only) {
            co_return 0;
        }

        // Show basic info
        if (show_info || (!list_files && !build_only)) {
            printf("Archive Information:\n");
            printf("  Format: %s\n", indexer->get_format_name());
            printf("  Path: %s\n", indexer->get_archive_path().c_str());
            printf("  Index Store: %s\n", indexer->get_index_path().c_str());
            printf("  Total size: %" PRIu64 " bytes\n",
                   static_cast<std::uint64_t>(indexer->get_max_bytes()));
            printf("  Total lines: %" PRIu64 "\n", indexer->get_num_lines());
            printf("  Checkpoints: %zu\n", indexer->get_checkpoints().size());
        }

        // List files for TAR archives
        if (list_files &&
            std::strcmp(indexer->get_format_name(), "TAR.GZ") == 0) {
            // Try to cast to TarIndexer to access TAR-specific functionality
            auto* tar_indexer = dynamic_cast<tar::TarIndexer*>(indexer.get());
            if (tar_indexer) {
                auto files = tar_indexer->list_files();
                printf("\nFiles in archive (%zu total):\n", files.size());

                for (const auto& file : files) {
                    printf("  %s", file.file_name.c_str());
                    if (file.typeflag == '5') {
                        printf(" (directory)");
                    } else {
                        printf(" (%" PRIu64 " bytes)", file.file_size);
                    }
                    printf("\n");
                }
            } else {
                printf("File listing not available for this format\n");
            }
        } else if (list_files) {
            printf("File listing not available for GZIP format\n");
        }

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Error processing archive: %s", e.what());
        co_return 1;
    }

    co_return 0;
}

int main(int argc, char** argv) {
    dftracer::utils::logger::init();

    argparse::ArgumentParser program("dftracer_tar",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "DFTracer utility for indexing and analyzing TAR.GZ archives");
    program.add_argument("file").help("TAR.GZ file to process").required();
    program.add_argument("-i", "--index")
        .help("Path to the .dftindex store to use (auto-generated if omitted)")
        .default_value<std::string>("");
    program.add_argument("-c", "--checkpoint-size")
        .help("Checkpoint size for indexing in bytes")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(Indexer::DEFAULT_CHECKPOINT_SIZE));
    program.add_argument("-f", "--force-rebuild")
        .help("Force rebuild the .dftindex store")
        .flag();
    program.add_argument("--list-files")
        .help("List all files in the TAR archive")
        .flag();
    program.add_argument("--info").help("Show archive information").flag();
    program.add_argument("--build-only")
        .help("Only build the index, don't perform other operations")
        .flag();
    cli::add_log_level_arg(program);

    try {
        program.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }
    cli::apply_log_level_arg(program);

    auto archive_path = program.get<std::string>("file");
    auto index_path = program.get<std::string>("index");
    auto checkpoint_size = program.get<std::size_t>("checkpoint-size");
    bool force_rebuild = program.get<bool>("force-rebuild");
    bool list_files = program.get<bool>("list-files");
    bool show_info = program.get<bool>("info");
    bool build_only = program.get<bool>("build-only");

    DFTRACER_UTILS_LOG_DEBUG("Archive file: %s", archive_path.c_str());
    DFTRACER_UTILS_LOG_DEBUG("Checkpoint size: %zu B (%zu MB)", checkpoint_size,
                             checkpoint_size / (1024 * 1024));
    DFTRACER_UTILS_LOG_DEBUG("Force rebuild: %s",
                             force_rebuild ? "true" : "false");

    // Check if file exists
    int test_fd = ::open(archive_path.c_str(), O_RDONLY);
    if (test_fd < 0) {
        DFTRACER_UTILS_LOG_ERROR("File '%s' does not exist or cannot be opened",
                                 archive_path.c_str());
        return 1;
    }
    ::close(test_fd);

    return run_tar(archive_path, index_path, checkpoint_size, force_rebuild,
                   list_files, show_info, build_only)
        .get();
}
