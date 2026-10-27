#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/index/store/index_backend.h>

#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::store {

namespace {

std::mutex& backend_mutex() {
    static std::mutex value;
    return value;
}

/// The backend index_backend() reads once the environment answered for this
/// process; empty until then.
std::optional<IndexBackend>& cached_backend() {
    static std::optional<IndexBackend> value;
    return value;
}

/// The backends the live test-only guards force, innermost last; empty when
/// none is live.
std::vector<IndexBackend>& forced_backends() {
    static std::vector<IndexBackend> value;
    return value;
}

IndexBackend parse_backend(const std::string& value) {
    if (value == constants::indexer::INDEX_BACKEND_ROCKSDB) {
        return IndexBackend::RocksDb;
    }
    if (value == constants::indexer::INDEX_BACKEND_STORE) {
        return IndexBackend::Store;
    }
    throw std::invalid_argument(std::string("unknown ") +
                                constants::indexer::INDEX_BACKEND_ENV +
                                " value: \"" + value + "\"");
}

}  // namespace

IndexBackend index_backend() {
    std::lock_guard<std::mutex> lock(backend_mutex());
    if (!forced_backends().empty()) return forced_backends().back();
    if (const auto cached = cached_backend()) return *cached;
    const std::string_view asked =
        Env::get<std::string_view>(constants::indexer::INDEX_BACKEND_ENV)
            .value_or(constants::indexer::INDEX_BACKEND_ROCKSDB);
    const IndexBackend backend = parse_backend(std::string(asked));
    cached_backend() = backend;
    return backend;
}

ScopedIndexBackend::ScopedIndexBackend(IndexBackend backend) {
    std::lock_guard<std::mutex> lock(backend_mutex());
    forced_backends().push_back(backend);
}

ScopedIndexBackend::~ScopedIndexBackend() {
    std::lock_guard<std::mutex> lock(backend_mutex());
    forced_backends().pop_back();
}

}  // namespace dftracer::utils::index::store
