#ifndef DFTRACER_UTILS_CORE_SQLITE_ERROR_H
#define DFTRACER_UTILS_CORE_SQLITE_ERROR_H

#include <stdexcept>
#include <string>

namespace dftracer::utils::sqlite {

class SqliteError : public std::runtime_error {
   public:
    enum Type {
        DATABASE_ERROR,
        STATEMENT_ERROR,
        OPEN_ERROR,
        VFS_ERROR,
        UNKNOWN_ERROR
    };

    SqliteError(Type type, const std::string &message)
        : std::runtime_error(format_message(type, message)), type_(type) {}

    inline Type type() const { return type_; }

   private:
    Type type_;
    static std::string format_message(Type type, const std::string &message);
};

}  // namespace dftracer::utils::sqlite

#endif  // DFTRACER_UTILS_CORE_SQLITE_ERROR_H
