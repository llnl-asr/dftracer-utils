// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_READER_ERROR_H
#define DFTRACER_UTILS_UTILITIES_READER_ERROR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/string.h>

#include <stdexcept>

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

    ReaderError(Type type, const dstd::String &message)
        : common::DFTUtilsException(common::ErrorCode::Reader,
                                    format_message(type, message)),
          type_(type) {}

    inline Type type() const { return type_; }

   private:
    Type type_;

    static dstd::String format_message(Type type, const dstd::String &message);
};
}  // namespace dftracer::utils::utilities::reader

#endif  // DFTRACER_UTILS_UTILITIES_READER_ERROR_H
