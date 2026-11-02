// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>

namespace dftracer::utils::index {

namespace {

std::string join(dstd::StringView base, dstd::StringView leaf) {
    return std::string((dstd::PathView(base) / leaf).str());
}

}  // namespace

std::string store_dir(dstd::StringView root) { return join(root, STORE_DIR); }

std::string store_of(dstd::StringView path) {
    if (path.empty()) return {};
    return store_dir(store::internal::normalize_index_root(path));
}

std::string rollups_dir(dstd::StringView root) {
    return join(root, ROLLUPS_DIR);
}

std::string lookups_dir(dstd::StringView root) {
    return join(root, LOOKUPS_DIR);
}

std::string views_dir(dstd::StringView root) { return join(root, VIEWS_DIR); }

std::string view_root(dstd::StringView root, dstd::StringView slug) {
    return join(views_dir(root), slug);
}

std::string view_parts_dir(dstd::StringView view_root) {
    return join(view_root, VIEW_PARTS_DIR);
}

std::string shards_dir(dstd::StringView root) { return join(root, SHARDS_DIR); }

std::string shard_root(dstd::StringView root, std::size_t n) {
    return join(shards_dir(root), std::to_string(n));
}

std::string shard_manifest_path(dstd::StringView root) {
    return join(root, SHARD_MANIFEST);
}

std::string spill_dir(dstd::StringView root) { return join(root, SPILL_DIR); }

dstd::Optional<std::string> nested_root_of(dstd::StringView path) {
    // Runs for every index path a query resolves, so it scans the text and
    // allocates only for a match. The deepest match wins.
    dstd::Optional<dstd::StringView> found;
    std::size_t from = 0;
    while (from < path.size()) {
        const std::size_t at = path.find(INDEX_ROOT_NAME, from);
        if (at == dstd::StringView::npos) break;
        from = at + 1;
        if (at != 0 && path[at - 1] != '/') continue;
        std::size_t kind = at + INDEX_ROOT_NAME.size();
        if (kind >= path.size() || path[kind] != '/') continue;
        ++kind;
        dstd::StringView rest = path.substr(kind);
        std::size_t name = 0;
        if (rest.starts_with(VIEWS_DIR) && rest.size() > VIEWS_DIR.size() &&
            rest[VIEWS_DIR.size()] == '/') {
            name = kind + VIEWS_DIR.size() + 1;
        } else if (rest.starts_with(SHARDS_DIR) &&
                   rest.size() > SHARDS_DIR.size() &&
                   rest[SHARDS_DIR.size()] == '/') {
            name = kind + SHARDS_DIR.size() + 1;
        } else {
            continue;
        }
        const std::size_t end = path.find('/', name);
        if (end == name || name >= path.size()) continue;
        found =
            path.substr(0, end == dstd::StringView::npos ? path.size() : end);
    }
    if (!found) return dstd::nullopt;
    return std::string(*found);
}

}  // namespace dftracer::utils::index
