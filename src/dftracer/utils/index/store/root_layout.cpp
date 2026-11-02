#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>

namespace dftracer::utils::index {

namespace {

std::string join(std::string_view base, std::string_view leaf) {
    return (common::fs::path(std::string(base)) / std::string(leaf)).string();
}

}  // namespace

std::string store_dir(std::string_view root) { return join(root, STORE_DIR); }

std::string store_of(std::string_view path) {
    if (path.empty()) return {};
    return store_dir(store::internal::normalize_index_root(path));
}

std::string rollups_dir(std::string_view root) {
    return join(root, ROLLUPS_DIR);
}

std::string lookups_dir(std::string_view root) {
    return join(root, LOOKUPS_DIR);
}

std::string views_dir(std::string_view root) { return join(root, VIEWS_DIR); }

std::string view_root(std::string_view root, std::string_view slug) {
    return join(views_dir(root), slug);
}

std::string view_parts_dir(std::string_view view_root) {
    return join(view_root, VIEW_PARTS_DIR);
}

std::string shards_dir(std::string_view root) { return join(root, SHARDS_DIR); }

std::string shard_root(std::string_view root, std::size_t n) {
    return join(shards_dir(root), std::to_string(n));
}

std::string shard_manifest_path(std::string_view root) {
    return join(root, SHARD_MANIFEST);
}

std::string spill_dir(std::string_view root) { return join(root, SPILL_DIR); }

std::optional<std::string> nested_root_of(std::string_view path) {
    // Runs for every index path a query resolves, so it scans the text and
    // allocates only for a match. The deepest match wins.
    std::optional<std::string_view> found;
    std::size_t from = 0;
    while (from < path.size()) {
        const std::size_t at = path.find(INDEX_ROOT_NAME, from);
        if (at == std::string_view::npos) break;
        from = at + 1;
        if (at != 0 && path[at - 1] != '/') continue;
        std::size_t kind = at + INDEX_ROOT_NAME.size();
        if (kind >= path.size() || path[kind] != '/') continue;
        ++kind;
        std::string_view rest = path.substr(kind);
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
            path.substr(0, end == std::string_view::npos ? path.size() : end);
    }
    if (!found) return std::nullopt;
    return std::string(*found);
}

}  // namespace dftracer::utils::index
