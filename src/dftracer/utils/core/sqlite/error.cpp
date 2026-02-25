#include <dftracer/utils/core/sqlite/error.h>

namespace dftracer::utils::sqlite {
std::string SqliteError::format_message(Type type, const std::string &message) {
    const char *prefix = "";
    switch (type) {
        case DATABASE_ERROR:
            prefix = "SQLite database error";
            break;
        case STATEMENT_ERROR:
            prefix = "SQLite statement error";
            break;
        case OPEN_ERROR:
            prefix = "SQLite open error";
            break;
        case VFS_ERROR:
            prefix = "SQLite VFS error";
            break;
        case UNKNOWN_ERROR:
            prefix = "SQLite unknown error";
            break;
    }
    return std::string(prefix) + ": " + message;
}
}  // namespace dftracer::utils::sqlite
