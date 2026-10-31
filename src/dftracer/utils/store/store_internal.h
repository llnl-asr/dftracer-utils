#ifndef DFTRACER_UTILS_STORE_STORE_INTERNAL_H
#define DFTRACER_UTILS_STORE_STORE_INTERNAL_H

#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/store/store.h>

namespace dftracer::utils::store {

namespace engine {

class Snapshot;
class Writer;

}  // namespace engine

/// The engine objects behind a Store, for the index layer, which reads copies
/// and commits its own batches. Not public: no other area may use it.
struct StoreAccess {
    /// Opens the store's writer when it has none.
    static dstd::Expected<dstd::SharedPtr<engine::Writer>, dstd::Error> writer(
        Store& store);
    /// True while the store holds a writer.
    static bool writer_open(const Store& store);
    /// The newest view, with the look for other processes' files and the
    /// published snapshot shortcut of snapshot().
    static dstd::Expected<dstd::SharedPtr<const engine::Snapshot>, dstd::Error>
    snapshot(Store& store);
    /// Looks at the data files of other processes again and drops the held
    /// snapshot.
    static void refresh(Store& store);
    /// Makes the next snapshot() reopen.
    static void bump_version(Store& store);
};

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_STORE_INTERNAL_H
