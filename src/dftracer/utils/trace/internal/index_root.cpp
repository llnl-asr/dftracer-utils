// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/trace/internal/index_root.h>

#include <mutex>

namespace dftracer::utils::trace::internal {

namespace {

struct IndexRoots {
    std::mutex guard;
    dstd::StringViewMap<std::shared_ptr<IndexRoot>> roots;
    dstd::StringViewMap<std::shared_ptr<IndexRoot>> by_spelling;
};

}  // namespace

std::shared_ptr<IndexRoot> index_root(const std::string& index_path) {
    IndexRoots& known = dstd::Singleton<IndexRoots>::get();
    {
        const std::lock_guard<std::mutex> lock(known.guard);
        const auto it = known.by_spelling.find(index_path);
        if (it != known.by_spelling.end()) return it->second;
    }
    const auto canonical =
        dstd::fs::weakly_canonical(dstd::PathView(index_path));
    const std::lock_guard<std::mutex> lock(known.guard);
    auto& r =
        known.roots[canonical ? std::string(canonical->str()) : index_path];
    if (!r) r = std::make_shared<IndexRoot>();
    known.by_spelling[index_path] = r;
    return r;
}

}  // namespace dftracer::utils::trace::internal
