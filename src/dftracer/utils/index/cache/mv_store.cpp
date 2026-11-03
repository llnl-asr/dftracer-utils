// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/duql/subsumption.h>
#include <dftracer/utils/index/cache/mv_store.h>
#include <dftracer/utils/index/cache/rollup_store.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <fcntl.h>
#include <simdjson.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dftracer::utils::index::cache {

namespace json = json;

namespace {

constexpr const char* MANIFEST_NAME = "manifest";
constexpr const char* LAST_USED_NAME = "last_used";
constexpr int MANIFEST_VERSION = 0;
constexpr std::size_t SLUG_MAX = 80;

struct BaseId {
    dstd::String path;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

dstd::Optional<BaseId> stat_base(dstd::CStringView path) {
    dftracer::utils::common::InputFileStat st;
    if (!dftracer::utils::common::stat_input(path, st)) return dstd::nullopt;
    return BaseId{dstd::String(path.view()), st.size, st.mtime};
}

// Base-file identities of the plan, sorted by path so the identity hash and
// the freshness comparison are order-independent.
dstd::Vector<BaseId> plan_bases(const trace::views::detail::ViewPlan& plan) {
    dstd::Vector<BaseId> out;
    for (const auto& f : plan.files)
        if (auto b = stat_base(f.file_path)) out.push_back(*b);
    std::sort(out.begin(), out.end(),
              [](const BaseId& a, const BaseId& b) { return a.path < b.path; });
    return out;
}

bool allowed(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

void replace_all(dstd::String& s, const dstd::String& from,
                 const dstd::String& to) {
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != dstd::String::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Render a predicate's canonical form to a filesystem-safe, readable token.
// Lossy by design (truncated, operators reduced to words); the identity suffix
// and manifest carry the exact query.
dstd::String sanitize_predicate(dstd::StringView canonical) {
    dstd::String s(canonical);
    replace_all(s, " not in ", "-nin-");
    replace_all(s, " not like ", "-notlike-");
    replace_all(s, " not ilike ", "-notilike-");
    replace_all(s, " ilike ", "-ilike-");
    replace_all(s, " like ", "-like-");
    replace_all(s, " >= ", "-ge-");
    replace_all(s, " <= ", "-le-");
    replace_all(s, " == ", "-eq-");
    replace_all(s, " != ", "-ne-");
    replace_all(s, " > ", "-gt-");
    replace_all(s, " < ", "-lt-");
    replace_all(s, " in ", "-in-");
    replace_all(s, " and ", "__");
    replace_all(s, " or ", "--or--");
    replace_all(s, "not (", "not-");

    dstd::String out;
    out.reserve(s.size());
    for (char c : s) {
        if (allowed(c)) {
            out.push_back(c);
        } else if (!out.empty() && out.back() != '-') {
            out.push_back('-');  // one dash per run of disallowed chars
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    std::size_t start = 0;
    while (start < out.size() && out[start] == '-') ++start;
    out = out.substr(start);
    if (out.size() > SLUG_MAX) out.resize(SLUG_MAX);
    return out.empty() ? "all" : out;
}

dstd::String window_tag(const trace::views::detail::ViewPlan& plan) {
    if (!plan.time_range) return {};
    return "__ts-" + dstd::to_string(plan.time_range->first) + "-" +
           dstd::to_string(plan.time_range->second);
}

dstd::String phase_tag(const trace::views::detail::ViewPlan& plan) {
    switch (plan.phase) {
        case trace::views::Phase::Events:
            return "__ph-E";
        case trace::views::Phase::Counters:
            return "__ph-C";
        case trace::views::Phase::Aggregated:
            return "__ph-A";
        case trace::views::Phase::Metadata:
            return "__ph-M";
        case trace::views::Phase::Any:
            return {};
    }
    return {};
}

// Stable identity of an MV: base files (path+size+mtime), predicate, phase, and
// window. Two materializations collide only if genuinely identical.
std::uint64_t identity_hash(const trace::views::detail::ViewPlan& plan) {
    dstd::String k;
    for (const auto& b : plan_bases(plan)) {
        k += b.path;
        k.push_back('|');
        k += dstd::to_string(b.size);
        k.push_back('|');
        k += dstd::to_string(b.mtime);
        k.push_back('\n');
    }
    k += "s=";
    k += dstd::to_string(plan.record_schema ? plan.record_schema->params_hash()
                                            : 0);
    k += "q=";
    k += plan.query ? plan.query->source() : "";
    k += phase_tag(plan);
    k += window_tag(plan);
    return dftracer::utils::dstd::hash::fnv1a_hash(k);
}

dstd::String hex8(std::uint64_t h) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x",
                  static_cast<unsigned>(h & 0xFFFFFFFFu));
    return dstd::String(buf);
}

dstd::Path manifest_path(dstd::StringView dir) {
    return dstd::PathView(dir) / MANIFEST_NAME;
}

// A listed entry is of `want` kind, following a symlink to find it out.
bool entry_is(const dstd::fs::Dir& dir, const dstd::fs::DirEntry& entry,
              dstd::fs::FileKind want) {
    if (entry.kind == want) return true;
    if (entry.kind != dstd::fs::FileKind::Symlink &&
        entry.kind != dstd::fs::FileKind::Unknown) {
        return false;
    }
    const auto found = dir.stat(entry.name);
    return found && found->kind == want;
}

// The entries of `dir` that are of `want` kind, as full paths. A directory that
// cannot be listed has none.
dstd::Vector<dstd::Path> list_kind(dstd::StringView dir,
                                   dstd::fs::FileKind want) {
    dstd::Vector<dstd::Path> out;
    const auto opened = dstd::fs::Dir::open(dstd::PathView(dir));
    if (!opened) return out;
    auto listing = opened->entries();
    if (!listing) return out;
    for (const dstd::fs::DirEntry& entry : *listing) {
        if (entry_is(*opened, entry, want)) {
            out.push_back(dstd::PathView(dir) / entry.name);
        }
    }
    return out;
}

struct Manifest {
    /// Params hash of the record schema the view was filtered under; views
    /// written before it was recorded have none and never match.
    dstd::Optional<std::uint64_t> schema;
    std::uint64_t bytes = 0;
    dstd::String query;
    int phase = static_cast<int>(trace::views::Phase::Any);
    bool has_window = false;
    double win_begin = 0.0;
    double win_end = 0.0;
    dstd::Vector<BaseId> bases;
};

dstd::Optional<std::uint64_t> parse_hex(dstd::StringView text) {
    if (text.empty() || text.size() > 16) return dstd::nullopt;
    std::uint64_t out = 0;
    for (const char c : text) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9')
            digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<unsigned>(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<unsigned>(c - 'A') + 10;
        else
            return dstd::nullopt;
        out = (out << 4) | digit;
    }
    return out;
}

dstd::Optional<Manifest> read_manifest(dstd::StringView dir) {
    const dstd::Path path = manifest_path(dir);
    const auto found = dstd::fs::exists(path.view());
    if (!found || !*found) return dstd::nullopt;
    try {
        simdjson::dom::parser parser;
        simdjson::dom::element root = parser.load(path.c_str());
        std::int64_t version = 0;
        if (root["version"].get(version) != simdjson::SUCCESS ||
            version != MANIFEST_VERSION)
            return dstd::nullopt;
        Manifest m;
        dstd::StringView schema;
        if (root["schema"].get(schema) == simdjson::SUCCESS) {
            const auto hash = parse_hex(schema);
            if (!hash) return dstd::nullopt;
            m.schema = *hash;
        }
        std::uint64_t bytes = 0;
        if (root["bytes"].get(bytes) == simdjson::SUCCESS) m.bytes = bytes;
        m.query = dstd::String(dstd::StringView(root["query"]));
        m.phase = static_cast<int>(std::int64_t(root["phase"]));
        simdjson::dom::element w = root["window"];
        if (!w.is_null()) {
            m.has_window = true;
            m.win_begin = double(w["begin"]);
            m.win_end = double(w["end"]);
        }
        for (simdjson::dom::element b : simdjson::dom::array(root["bases"])) {
            BaseId id;
            id.path = dstd::String(dstd::StringView(b["path"]));
            id.size = std::uint64_t(std::int64_t(b["size"]));
            id.mtime = std::int64_t(b["mtime"]);
            m.bases.push_back(std::move(id));
        }
        std::sort(
            m.bases.begin(), m.bases.end(),
            [](const BaseId& a, const BaseId& c) { return a.path < c.path; });
        return m;
    } catch (const simdjson::simdjson_error&) {
        return dstd::nullopt;
    } catch (const std::logic_error&) {
        return dstd::nullopt;
    }
}

std::uint64_t dir_size(dstd::StringView dir) {
    std::uint64_t sum = 0;
    // A tree that cannot be walked sums what was visited.
    (void)dstd::fs::walk(
        dstd::PathView(dir), [&](dstd::PathView path, dstd::fs::FileKind kind) {
            if (kind == dstd::fs::FileKind::File ||
                kind == dstd::fs::FileKind::Symlink) {
                const auto found = dstd::fs::stat(path);
                if (found && found->kind == dstd::fs::FileKind::File) {
                    sum += found->size;
                }
            }
            return dstd::fs::WalkAction::Continue;
        });
    return sum;
}

// When a view was last used: its last_used file, else its manifest. Only the
// order of two values matters.
std::int64_t last_used(dstd::StringView dir) {
    for (const char* name : {LAST_USED_NAME, MANIFEST_NAME}) {
        const dstd::Path file = dstd::PathView(dir) / name;
        const auto t = dstd::fs::mtime_ns(file.view());
        if (t) return static_cast<std::int64_t>(*t);
    }
    return 0;
}

void touch_last_used(dstd::StringView dir) {
    const dstd::Path path = dstd::PathView(dir) / LAST_USED_NAME;
    dstd::ScopedFd fd(
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666));
    if (fd.valid()) ::futimens(fd.get(), nullptr);
}

// Removes the least recently used views under `root` until they hold at most
// cache_max_bytes(); `keep` goes only when it alone is over the budget. Another
// view whose lock is held (it is being built) stays.
void evict_views(dstd::StringView root, dstd::StringView keep) {
    struct Entry {
        dstd::String dir;
        std::uint64_t bytes;
        std::int64_t last_used;
    };
    dstd::Vector<Entry> entries;
    std::uint64_t total = 0;
    for (const dstd::Path& path :
         list_kind(root, dstd::fs::FileKind::Directory)) {
        const dstd::String dir(path.str());
        auto m = read_manifest(dir);
        if (!m) continue;
        const std::uint64_t bytes = m->bytes ? m->bytes : dir_size(dir);
        entries.push_back({dir, bytes, last_used(dir)});
        total += bytes;
    }
    const std::uint64_t budget = cache_max_bytes();
    if (total <= budget) return;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  return a.last_used < b.last_used;
              });
    auto drop = [&](const Entry& e) {
        // The caller wrote `keep` and may hold its lock.
        const int fd = e.dir == keep ? -1 : lock_view_dir(e.dir);
        if (fd < 0 && e.dir != keep) return;
        release_root(e.dir);
        const auto removed = dstd::fs::remove_all(dstd::PathView(e.dir));
        unlock_view_dir(fd);
        if (removed) total -= e.bytes;
    };
    for (const auto& e : entries) {
        if (total <= budget) break;
        if (e.dir != keep) drop(e);
    }
    if (total > budget)
        for (const auto& e : entries)
            if (e.dir == keep) drop(e);
}

// Trace files inside an MV directory (its own index is a subdir, skipped).
dstd::Vector<dstd::String> view_trace_files(const dstd::String& dir) {
    dstd::Vector<dstd::String> out;
    for (const dstd::Path& path : list_kind(dir, dstd::fs::FileKind::File)) {
        const dstd::StringView name = path.view().filename();
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".pfw") == 0)
            out.emplace_back(path.str());
        else if (name.size() >= 7 &&
                 name.compare(name.size() - 7, 7, ".pfw.gz") == 0)
            out.emplace_back(path.str());
    }
    std::sort(out.begin(), out.end());
    return out;
}

trace::views::ViewFile view_file(const dstd::String& file,
                                 const dstd::String& index) {
    trace::views::ViewFile out;
    out.file_path.assign(file.data(), file.size());
    out.index_path.assign(index.data(), index.size());
    return out;
}

// All part files of an MV as ViewFiles, each paired with the index its writer
// used. Parts directly under parts/ use the view root. A distributed
// materialize puts each rank's parts in a subdir of parts/, which resolves to
// the same view root inside the index root and to its own root elsewhere.
// keep std: ViewPlan::files is a std::vector
std::vector<trace::views::ViewFile> gather_view_files(const dstd::String& dir) {
    namespace dfti = trace::internal;
    // keep std: ViewPlan::files is a std::vector
    std::vector<trace::views::ViewFile> out;
    const dstd::String view_idx = dfti::determine_index_path(dir, "");
    const dstd::String parts = index::view_parts_dir(dir);
    for (const auto& t : view_trace_files(parts))
        out.push_back(view_file(t, view_idx));
    for (const dstd::Path& sub_path :
         list_kind(parts, dstd::fs::FileKind::Directory)) {
        const dstd::String sub(sub_path.str());
        const dstd::String sidx = dfti::determine_index_path(sub, "");
        for (const auto& t : view_trace_files(sub))
            out.push_back(view_file(t, sidx));
    }
    return out;
}

// keep std: ViewPlan::files is a std::vector
std::uint64_t total_size(const std::vector<trace::views::ViewFile>& files) {
    std::uint64_t sum = 0;
    for (const auto& f : files) {
        dftracer::utils::common::InputFileStat st;
        if (dftracer::utils::common::stat_input(f.file_path, st))
            sum += st.size;
    }
    return sum;
}

// A stored MV can answer `plan` only if its base set is byte-identical and
// unchanged, its phase covers the query's, its window covers the query's, and
// its predicate subsumes the query's.
bool manifest_matches(const Manifest& m,
                      const trace::views::detail::ViewPlan& plan,
                      const dstd::Vector<BaseId>& q_bases) {
    if (!m.schema || *m.schema != plan.record_schema->params_hash())
        return false;
    if (m.bases.size() != q_bases.size()) return false;
    for (std::size_t i = 0; i < m.bases.size(); ++i)
        if (m.bases[i].path != q_bases[i].path ||
            m.bases[i].size != q_bases[i].size ||
            m.bases[i].mtime != q_bases[i].mtime)
            return false;

    if (m.phase != static_cast<int>(trace::views::Phase::Any) &&
        m.phase != static_cast<int>(plan.phase))
        return false;

    if (m.has_window) {
        if (!plan.time_range) return false;
        if (m.win_begin > plan.time_range->first ||
            m.win_end < plan.time_range->second)
            return false;
    }

    if (m.query.empty()) return true;  // MV keeps every row
    if (!plan.query) return false;     // query keeps every row, MV does not
    auto mv_q = duql::try_parse(m.query);
    if (!mv_q) return false;
    return duql::query_subsumes(mv_q->root(), plan.query->root());
}

// True if any base file the manifest was built from is now gone or changed, so
// the MV can never be served again and is safe to delete.
bool manifest_is_stale(const Manifest& m) {
    for (const auto& b : m.bases) {
        auto cur = stat_base(b.path);
        if (!cur || cur->size != b.size || cur->mtime != b.mtime) return true;
    }
    return false;
}

}  // namespace

dstd::String views_root(const trace::views::detail::ViewPlan& plan) {
    // A view's identity needs the record schema; without it there is no cache.
    if (!plan.record_schema || plan.build_step) return {};
    if (!plan.views_root.empty())
        return dstd::String(dstd::StringView(plan.views_root));
    const dstd::String dir = cache_dir(plan);
    if (dir.empty()) return {};
    return index::views_dir(dir);
}

dstd::String view_slug(const trace::views::detail::ViewPlan& plan) {
    const dstd::String canonical =
        plan.query ? duql::to_string(plan.query->root()) : "all";
    return sanitize_predicate(canonical) + phase_tag(plan) + window_tag(plan) +
           "-" + hex8(identity_hash(plan));
}

bool view_is_fresh(const trace::views::detail::ViewPlan& plan) {
    const dstd::String root = views_root(plan);
    if (root.empty()) return false;
    const dstd::Path dir = dstd::PathView(root) / view_slug(plan);
    const auto present = dstd::fs::exists(manifest_path(dir.str()).view());
    return present && *present &&
           !gather_view_files(dstd::String(dir.str())).empty();
}

int lock_view_dir(dstd::StringView dir) {
    const dstd::Path path = dstd::PathView(dir) / ".lock";
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) return -1;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {  // another builder holds it
        ::close(fd);
        return -1;
    }
    return fd;  // released (and the advisory lock dropped) by unlock_view_dir
}

void unlock_view_dir(int fd) {
    if (fd < 0) return;
    ::flock(fd, LOCK_UN);
    ::close(fd);
}

dstd::String materialize_view_dir(const trace::views::detail::ViewPlan& plan) {
    const dstd::String root = views_root(plan);
    if (root.empty()) return {};
    const dstd::Path dir = dstd::PathView(root) / view_slug(plan);
    dstd::String dir_text(dir.str());
    if (!dstd::fs::create_directories(
            dstd::PathView(index::view_parts_dir(dir_text)))) {
        return {};
    }
    return dir_text;
}

void register_view(dstd::StringView dir,
                   const trace::views::detail::ViewPlan& plan) {
    char schema[17];
    std::snprintf(
        schema, sizeof(schema), "%016llx",
        static_cast<unsigned long long>(
            plan.record_schema ? plan.record_schema->params_hash() : 0));
    dstd::String j = "{\n";
    j += "  \"version\": " + dstd::int_text(MANIFEST_VERSION) + ",\n";
    j += "  \"schema\": \"" + dstd::String(schema) + "\",\n";
    j += "  \"bytes\": " + dstd::int_text(dir_size(dir)) + ",\n";
    j += "  \"query\": \"";
    json::append_json_escaped(j, plan.query ? plan.query->source() : "");
    j += "\",\n";
    j += "  \"phase\": " + dstd::int_text(static_cast<int>(plan.phase)) + ",\n";
    if (plan.time_range) {
        j += "  \"window\": {\"begin\": " +
             dstd::int_text(plan.time_range->first) +
             ", \"end\": " + dstd::int_text(plan.time_range->second) + "},\n";
    } else {
        j += "  \"window\": null,\n";
    }
    j += "  \"bases\": [";
    const auto bases = plan_bases(plan);
    for (std::size_t i = 0; i < bases.size(); ++i) {
        j += i ? ",\n    {" : "\n    {";
        j += "\"path\": \"";
        json::append_json_escaped(j, bases[i].path);
        j += "\", \"size\": " + dstd::int_text(bases[i].size) +
             ", \"mtime\": " + dstd::int_text(bases[i].mtime) + "}";
    }
    j += bases.empty() ? "]\n}\n" : "\n  ]\n}\n";

    (void)dstd::fs::write_file(manifest_path(dir).view(), j);
    if (!plan.record_schema) return;
    const dstd::String root = views_root(plan);
    if (!root.empty()) evict_views(root, dir);
}

// keep std: ViewPlan::files is a std::vector
dstd::Optional<std::vector<trace::views::ViewFile>> find_subsuming_view(
    const trace::views::detail::ViewPlan& plan) {
    const dstd::String root = views_root(plan);
    if (root.empty()) return dstd::nullopt;
    const auto root_is_dir = dstd::fs::is_directory(dstd::PathView(root));
    if (!root_is_dir || !*root_is_dir) return dstd::nullopt;

    const auto q_bases = plan_bases(plan);
    if (q_bases.size() != plan.files.size())
        return dstd::nullopt;  // unstattable
    const std::uint64_t base_bytes = [&] {
        std::uint64_t s = 0;
        for (const auto& b : q_bases) s += b.size;
        return s;
    }();

    // keep std: ViewPlan::files is a std::vector
    std::vector<trace::views::ViewFile> best;
    dstd::String best_dir;
    std::uint64_t best_bytes = base_bytes;  // must beat the base to be worth it
    for (const dstd::Path& path :
         list_kind(root, dstd::fs::FileKind::Directory)) {
        const dstd::String dir(path.str());
        auto m = read_manifest(dir);
        if (!m) continue;
        // Evict a view built from a base that has since changed or vanished: it
        // can never be served again, and nobody reads a non-matching MV.
        if (manifest_is_stale(*m)) {
            release_root(dir);
            (void)dstd::fs::remove_all(dstd::PathView(dir));
            continue;
        }
        if (!manifest_matches(*m, plan, q_bases)) continue;

        auto files = gather_view_files(dir);
        if (files.empty()) continue;
        const std::uint64_t bytes = total_size(files);
        if (bytes >= best_bytes)
            continue;  // not smaller than base or a better MV

        best = std::move(files);
        best_dir = dir;
        best_bytes = bytes;
    }

    if (best.empty()) return dstd::nullopt;
    touch_last_used(best_dir);
    return best;
}

}  // namespace dftracer::utils::index::cache
