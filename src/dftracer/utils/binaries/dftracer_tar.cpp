#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/indexer/indexer_factory.h>
#include <dftracer/utils/indexer/tar_indexer.h>

#include <argparse/argparse.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

using namespace dftracer::utils;

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_tar",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "DFTracer utility for indexing and analyzing TAR.GZ archives");
    program.add_argument("file").help("TAR.GZ file to process").required();
    program.add_argument("-i", "--index")
        .help("Index file to use (auto-generated if not specified)")
        .default_value<std::string>("");
    program.add_argument("-c", "--checkpoint-size")
        .help("Checkpoint size for indexing in bytes")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(Indexer::DEFAULT_CHECKPOINT_SIZE));
    program.add_argument("-f", "--force-rebuild")
        .help("Force rebuild index")
        .flag();
    program.add_argument("--list-files")
        .help("List all files in the TAR archive")
        .flag();
    program.add_argument("--info").help("Show archive information").flag();
    program.add_argument("--build-only")
        .help("Only build the index, don't perform other operations")
        .flag();

    try {
        program.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

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
    FILE* test_file = fopen(archive_path.c_str(), "rb");
    if (!test_file) {
        DFTRACER_UTILS_LOG_ERROR("File '%s' does not exist or cannot be opened",
                                 archive_path.c_str());
        return 1;
    }
    fclose(test_file);

    try {
        // Create indexer using factory
        auto indexer = IndexerFactory::create(archive_path, index_path,
                                              checkpoint_size, force_rebuild);

        if (!indexer) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to create indexer for file '%s' - "
                "unsupported or unrecognized format",
                archive_path.c_str());
            return 1;
        }

        DFTRACER_UTILS_LOG_INFO("Detected format: %s",
                                indexer->get_format_name());
        DFTRACER_UTILS_LOG_INFO("Index file: %s",
                                indexer->get_idx_path().c_str());

        // Build index if needed
        if (force_rebuild || indexer->need_rebuild()) {
            DFTRACER_UTILS_LOG_INFO("%s", "Building index...");
            indexer->build();
            DFTRACER_UTILS_LOG_INFO("%s", "Index built successfully");
        } else {
            DFTRACER_UTILS_LOG_INFO("%s", "Index is up to date");
        }

        if (build_only) {
            return 0;
        }

        // Show basic info
        if (show_info || (!list_files && !build_only)) {
            std::cout << "Archive Information:" << std::endl;
            std::cout << "  Format: " << indexer->get_format_name()
                      << std::endl;
            std::cout << "  Path: " << indexer->get_archive_path() << std::endl;
            std::cout << "  Index: " << indexer->get_idx_path() << std::endl;
            std::cout << "  Total size: " << indexer->get_max_bytes()
                      << " bytes" << std::endl;
            std::cout << "  Total lines: " << indexer->get_num_lines()
                      << std::endl;
            std::cout << "  Checkpoints: " << indexer->get_checkpoints().size()
                      << std::endl;
        }

        // List files for TAR archives
        if (list_files &&
            std::strcmp(indexer->get_format_name(), "TAR.GZ") == 0) {
            // Try to cast to TarIndexer to access TAR-specific functionality
            auto* tar_indexer =
                dynamic_cast<dftracer::utils::TarIndexer*>(indexer.get());
            if (tar_indexer) {
                auto files = tar_indexer->list_files();
                std::cout << "\nFiles in archive (" << files.size()
                          << " total):" << std::endl;

                for (const auto& file : files) {
                    std::cout << "  " << file.file_name;
                    if (file.typeflag == '5') {
                        std::cout << " (directory)";
                    } else {
                        std::cout << " (" << file.file_size << " bytes)";
                    }
                    std::cout << std::endl;
                }
            } else {
                std::cout << "File listing not available for this format"
                          << std::endl;
            }
        } else if (list_files) {
            std::cout << "File listing not available for GZIP format"
                      << std::endl;
        }

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Error processing archive: %s", e.what());
        return 1;
    }

    return 0;
}
