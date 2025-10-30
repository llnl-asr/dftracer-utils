#include <dftracer/utils/utilities/reader/internal/error.h>

namespace dftracer::utils::utilities::reader::internal {
std::string ReaderError::format_message(Type type, const std::string &message) {
    const char *prefix = "";
    switch (type) {
        case ReaderError::DATABASE_ERROR:
            prefix = "Database error";
            break;
        case ReaderError::FILE_IO_ERROR:
            prefix = "File I/O error";
            break;
        case ReaderError::COMPRESSION_ERROR:
            prefix = "Compression error";
            break;
        case ReaderError::INVALID_ARGUMENT:
            prefix = "Invalid argument";
            break;
        case ReaderError::INITIALIZATION_ERROR:
            prefix = "Initialization error";
            break;
        case ReaderError::READ_ERROR:
            prefix = "Read error";
            break;
        case ReaderError::UNKNOWN_ERROR:
            prefix = "Unknown error";
            break;
    }
    return std::string(prefix) + ": " + message;
}

}  // namespace dftracer::utils::utilities::reader::internal
