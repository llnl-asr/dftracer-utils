#include <dftracer/utils/utilities/reader/error.h>

namespace dftracer::utils::utilities::reader {
std::string ReaderError::format_message(Type type, const std::string &message) {
    const char *prefix = "";
    switch (type) {
        case ReaderError::DatabaseError:
            prefix = "Database error";
            break;
        case ReaderError::FileIoError:
            prefix = "File I/O error";
            break;
        case ReaderError::CompressionError:
            prefix = "Compression error";
            break;
        case ReaderError::InvalidArgument:
            prefix = "Invalid argument";
            break;
        case ReaderError::InitializationError:
            prefix = "Initialization error";
            break;
        case ReaderError::ReadError:
            prefix = "Read error";
            break;
        case ReaderError::UnknownError:
            prefix = "Unknown error";
            break;
    }
    return std::string(prefix) + ": " + message;
}

}  // namespace dftracer::utils::utilities::reader
