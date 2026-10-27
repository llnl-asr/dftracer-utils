#ifndef DFTRACER_UTILS_INDEX_STORE_INDEX_BACKEND_H
#define DFTRACER_UTILS_INDEX_STORE_INDEX_BACKEND_H

#include <cstdint>

namespace dftracer::utils::index::store {

/// The engine that holds an index root.
enum class IndexBackend : std::uint8_t { RocksDb, Store };

/// The backend of this process, read once from constants::indexer::
/// INDEX_BACKEND_ENV and cached. Throws std::invalid_argument, naming the
/// value, for a value that names neither backend.
IndexBackend index_backend();

/// Test-only: makes index_backend() answer `backend` until the guard dies, and
/// the answer it gave without the guard from then on. Only a test needs this:
/// a process has one backend, which the environment picks.
class ScopedIndexBackend {
   public:
    explicit ScopedIndexBackend(IndexBackend backend);
    ~ScopedIndexBackend();
    ScopedIndexBackend(const ScopedIndexBackend&) = delete;
    ScopedIndexBackend& operator=(const ScopedIndexBackend&) = delete;
};

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_INDEX_BACKEND_H
