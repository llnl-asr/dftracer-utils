#include <dftracer/utils/index/store/error.h>

namespace dftracer::utils::index::store {
std::string IndexerError::format_message(Type type,
                                         const std::string &message) {
    const char *prefix = "";
    switch (type) {
        case DatabaseError:
            prefix = "Database error";
            break;
        case FileError:
            prefix = "File error";
            break;
        case CompressionError:
            prefix = "Compression error";
            break;
        case InvalidArgument:
            prefix = "Invalid argument";
            break;
        case BuildError:
            prefix = "Build error";
            break;
        case UnknownError:
            prefix = "Unknown error";
            break;
    }
    return std::string(prefix) + ": " + message;
}
}  // namespace dftracer::utils::index::store
