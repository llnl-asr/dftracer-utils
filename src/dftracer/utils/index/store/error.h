#ifndef DFTRACER_UTILS_INDEX_STORE_ERROR_H
#define DFTRACER_UTILS_INDEX_STORE_ERROR_H

#include <dftracer/utils/common/error.h>

#include <stdexcept>
#include <string>

namespace dftracer::utils::index::store {

class IndexerError : public DFTUtilsException {
   public:
    enum Type {
        DatabaseError,
        FileError,
        CompressionError,
        InvalidArgument,
        BuildError,
        UnknownError
    };

    IndexerError(Type type, const std::string &message)
        : DFTUtilsException(ErrorCode::Indexer, format_message(type, message)),
          type_(type) {}

    inline Type type() const { return type_; }

   private:
    Type type_;
    static std::string format_message(Type type, const std::string &message);
};

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_ERROR_H
