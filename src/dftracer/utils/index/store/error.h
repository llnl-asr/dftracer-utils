// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_ERROR_H
#define DFTRACER_UTILS_INDEX_STORE_ERROR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/string.h>

#include <stdexcept>

namespace dftracer::utils::index::store {

class IndexerError : public common::DFTUtilsException {
   public:
    enum Type {
        DatabaseError,
        FileError,
        CompressionError,
        InvalidArgument,
        BuildError,
        UnknownError
    };

    IndexerError(Type type, const dstd::String &message)
        : common::DFTUtilsException(common::ErrorCode::Indexer,
                                    format_message(type, message)),
          type_(type) {}

    inline Type type() const { return type_; }

   private:
    Type type_;
    static dstd::String format_message(Type type, const dstd::String &message);
};

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_ERROR_H
