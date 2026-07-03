#ifndef DFTRACER_UTILS_UTILITIES_READER_ERROR_H
#define DFTRACER_UTILS_UTILITIES_READER_ERROR_H

#include <dftracer/utils/core/common/error.h>

#include <stdexcept>
#include <string>

namespace dftracer::utils::utilities::reader {

class ReaderError : public DFTUtilsException {
   public:
    enum Type {
        DATABASE_ERROR,
        FILE_IO_ERROR,
        COMPRESSION_ERROR,
        INVALID_ARGUMENT,
        INITIALIZATION_ERROR,
        READ_ERROR,
        UNKNOWN_ERROR,
    };

    ReaderError(Type type, const std::string &message)
        : DFTUtilsException(ErrorCode::READER, format_message(type, message)),
          type_(type) {}

    inline Type get_type() const { return type_; }

   private:
    Type type_;

    static std::string format_message(Type type, const std::string &message);
};
}  // namespace dftracer::utils::utilities::reader

#endif  // DFTRACER_UTILS_UTILITIES_READER_ERROR_H
