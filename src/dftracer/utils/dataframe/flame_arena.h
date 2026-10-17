#ifndef DFTRACER_UTILS_DATAFRAME_FLAME_ARENA_H
#define DFTRACER_UTILS_DATAFRAME_FLAME_ARENA_H

#include <dftracer/utils/dstd/transparent_string_hash.h>  // StringViewMap

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// The folded call-tree (flamegraph) arena: the name-path rollup shared by the
// columnar flamegraph() op, the server /viz/calltree builder, and the CLI/MPI
// tools. Identical name-paths fold into one node. This is the mergeable partial
// the distributed paths reduce (merge_flame_arena), sitting on the same
// containment_walk core as the per-event call_tree() column op.

namespace dftracer::utils::dataframe {

/// One node of the folded tree. `total` is inclusive time; `self` is total
/// minus the time attributed to nested children. `kids` maps a child name to
/// its arena index (keyed by a view into that child's owned `name`), so a fold
/// finds-or-creates in O(1); `children` preserves the child index list.
struct FlameNode {
    std::string name;
    double total = 0;
    double self = 0;
    std::uint64_t count = 0;
    dftracer::utils::StringViewMap<std::uint32_t> kids;
    std::vector<std::uint32_t> children;
};

/// Fold one event (`name`, `dur`) as a child of `parent` in `arena`: find or
/// create the name-path child, add its stats, and return its arena index (the
/// handle a containment_walk pushes for the event's nested children). Node 0 is
/// the caller's root sentinel, so its self is left untouched.
inline std::uint32_t fold_flame_node(std::vector<FlameNode>& arena,
                                     std::string_view name, double dur,
                                     std::uint32_t parent) {
    std::uint32_t mi;
    auto it = arena[parent].kids.find(name);
    if (it == arena[parent].kids.end()) {
        mi = static_cast<std::uint32_t>(arena.size());
        arena.emplace_back();
        arena[mi].name.assign(name);
        arena[parent].kids.emplace(arena[mi].name, mi);
        arena[parent].children.push_back(mi);
    } else {
        mi = it->second;
    }
    arena[mi].total += dur;
    arena[mi].self += dur;
    arena[mi].count += 1;
    if (parent != 0) arena[parent].self -= dur;
    return mi;
}

/// Merge partial tree `src` (subtree `si`) into `dst` (node `di`), summing
/// stats per name-path. `src` node names stay alive for the whole merge.
inline void merge_flame_arena(std::vector<FlameNode>& dst, std::uint32_t di,
                              const std::vector<FlameNode>& src,
                              std::uint32_t si) {
    dst[di].total += src[si].total;
    dst[di].self += src[si].self;
    dst[di].count += src[si].count;
    for (std::uint32_t sc : src[si].children) {
        std::uint32_t dc;
        auto it = dst[di].kids.find(src[sc].name);
        if (it == dst[di].kids.end()) {
            dc = static_cast<std::uint32_t>(dst.size());
            dst.emplace_back();
            dst[dc].name = src[sc].name;
            dst[di].kids.emplace(dst[dc].name, dc);
            dst[di].children.push_back(dc);
        } else {
            dc = it->second;
        }
        merge_flame_arena(dst, dc, src, sc);
    }
}

namespace flame_detail {
template <class T>
inline void put(std::vector<std::uint8_t>& b, const T& v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}
template <class T>
inline T get(const std::uint8_t*& p, const std::uint8_t* end) {
    T v{};
    if (p + sizeof(T) <= end) std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}
}  // namespace flame_detail

/// Serialize a partial arena to a self-contained byte blob for a distributed
/// merge (MPI gather / Dask reduce). Only the fields merge_flame_arena reads
/// are written (name/total/self/count/children); the kids map is rebuilt on
/// merge. Homogeneous-endianness transport (a cluster of like machines).
inline std::vector<std::uint8_t> serialize_flame_arena(
    const std::vector<FlameNode>& arena) {
    std::vector<std::uint8_t> b;
    flame_detail::put<std::uint32_t>(b, 0x464c414du);  // 'FLAM'
    flame_detail::put<std::uint32_t>(b,
                                     static_cast<std::uint32_t>(arena.size()));
    for (const FlameNode& n : arena) {
        flame_detail::put<std::uint32_t>(
            b, static_cast<std::uint32_t>(n.name.size()));
        b.insert(b.end(), n.name.begin(), n.name.end());
        flame_detail::put<double>(b, n.total);
        flame_detail::put<double>(b, n.self);
        flame_detail::put<std::uint64_t>(b, n.count);
        flame_detail::put<std::uint32_t>(
            b, static_cast<std::uint32_t>(n.children.size()));
        for (std::uint32_t c : n.children)
            flame_detail::put<std::uint32_t>(b, c);
    }
    return b;
}

/// Inverse of serialize_flame_arena. Returns an arena mergeable via
/// merge_flame_arena (kids left empty; the merge does not read src.kids). An
/// empty/short blob yields an empty arena.
inline std::vector<FlameNode> deserialize_flame_arena(const std::uint8_t* data,
                                                      std::size_t len) {
    std::vector<FlameNode> arena;
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + len;
    if (flame_detail::get<std::uint32_t>(p, end) != 0x464c414du) return arena;
    const std::uint32_t n = flame_detail::get<std::uint32_t>(p, end);
    arena.resize(n);
    for (std::uint32_t i = 0; i < n && p <= end; ++i) {
        FlameNode& nd = arena[i];
        const std::uint32_t nl = flame_detail::get<std::uint32_t>(p, end);
        if (p + nl <= end) nd.name.assign(reinterpret_cast<const char*>(p), nl);
        p += nl;
        nd.total = flame_detail::get<double>(p, end);
        nd.self = flame_detail::get<double>(p, end);
        nd.count = flame_detail::get<std::uint64_t>(p, end);
        const std::uint32_t nc = flame_detail::get<std::uint32_t>(p, end);
        nd.children.reserve(nc);
        for (std::uint32_t c = 0; c < nc; ++c)
            nd.children.push_back(flame_detail::get<std::uint32_t>(p, end));
    }
    return arena;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_FLAME_ARENA_H
