// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/internal/index_root.h>

#include <mutex>

namespace dftracer::utils::trace::internal {

namespace {

using RootMap = dstd::StringViewMap<dstd::SharedPtr<IndexRoot>>;

struct IndexRoots {
    std::mutex guard;
    dstd::Optional<RootMap> roots;
    dstd::Optional<RootMap> by_spelling;
};

RootMap& held(dstd::Optional<RootMap>& slot) {
    if (!slot) slot.emplace();
    return *slot;
}

}  // namespace

dstd::SharedPtr<IndexRoot> index_root(const dstd::String& index_path) {
    IndexRoots& known = dstd::Singleton<IndexRoots>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, [] {
            IndexRoots& roots = dstd::Singleton<IndexRoots>::get();
            const std::lock_guard<std::mutex> lock(roots.guard);
            roots.roots.reset();
            roots.by_spelling.reset();
        });
        return true;
    }();
    {
        const std::lock_guard<std::mutex> lock(known.guard);
        RootMap& spellings = held(known.by_spelling);
        const auto it = spellings.find(index_path);
        if (it != spellings.end()) return it->second;
    }
    const auto canonical =
        dstd::fs::weakly_canonical(dstd::PathView(index_path));
    const std::lock_guard<std::mutex> lock(known.guard);
    auto& r = held(
        known.roots)[canonical ? dstd::String(canonical->str()) : index_path];
    if (!r) r = dstd::make_shared<IndexRoot>();
    held(known.by_spelling)[index_path] = r;
    return r;
}

}  // namespace dftracer::utils::trace::internal
