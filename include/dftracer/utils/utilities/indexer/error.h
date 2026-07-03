#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_ERROR_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_ERROR_H

#include <dftracer/utils/core/common/error.h>

#include <stdexcept>
#include <string>

namespace dftracer::utils::utilities::indexer {

class IndexerError : public DFTUtilsException {
   public:
    enum Type {
        DATABASE_ERROR,
        FILE_ERROR,
        COMPRESSION_ERROR,
        INVALID_ARGUMENT,
        BUILD_ERROR,
        UNKNOWN_ERROR
    };

    IndexerError(Type type, const std::string &message)
        : DFTUtilsException(ErrorCode::INDEXER, format_message(type, message)),
          type_(type) {}

    inline Type type() const { return type_; }

   private:
    Type type_;
    static std::string format_message(Type type, const std::string &message);
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_ERROR_H
