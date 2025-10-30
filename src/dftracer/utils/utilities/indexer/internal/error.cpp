#include <dftracer/utils/utilities/indexer/internal/error.h>

namespace dftracer::utils::utilities::indexer::internal {
std::string IndexerError::format_message(Type type,
                                         const std::string &message) {
    const char *prefix = "";
    switch (type) {
        case DATABASE_ERROR:
            prefix = "Database error";
            break;
        case FILE_ERROR:
            prefix = "File error";
            break;
        case COMPRESSION_ERROR:
            prefix = "Compression error";
            break;
        case INVALID_ARGUMENT:
            prefix = "Invalid argument";
            break;
        case BUILD_ERROR:
            prefix = "Build error";
            break;
        case UNKNOWN_ERROR:
            prefix = "Unknown error";
            break;
    }
    return std::string(prefix) + ": " + message;
}
}  // namespace dftracer::utils::utilities::indexer::internal
