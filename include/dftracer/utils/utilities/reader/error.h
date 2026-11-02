// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_READER_ERROR_H
#define DFTRACER_UTILS_UTILITIES_READER_ERROR_H

#include <dftracer/utils/common/error.h>

#include <stdexcept>
#include <string>

namespace dftracer::utils::utilities::reader {

class ReaderError : public common::DFTUtilsException {
   public:
    enum Type {
        DatabaseError,
        FileIoError,
        CompressionError,
        InvalidArgument,
        InitializationError,
        ReadError,
        UnknownError,
    };

    ReaderError(Type type, const std::string &message)
        : common::DFTUtilsException(common::ErrorCode::Reader,
                                    format_message(type, message)),
          type_(type) {}

    inline Type get_type() const { return type_; }

   private:
    Type type_;

    static std::string format_message(Type type, const std::string &message);
};
}  // namespace dftracer::utils::utilities::reader

#endif  // DFTRACER_UTILS_UTILITIES_READER_ERROR_H
