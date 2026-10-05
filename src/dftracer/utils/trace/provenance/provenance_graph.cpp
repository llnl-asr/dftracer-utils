#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/trace/provenance/provenance_graph.h>
#include <dftracer/utils/trace/views/view.h>
#include <simdjson.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace dftracer::utils::trace::provenance {

namespace {

constexpr std::string_view ENTITY_PREFIX = "prov_entity:";
constexpr std::string_view TYPE_PREFIX = "prov_type:";

// Per-slot partial: keyed for O(1) merge, flattened into vectors at the end.
// dftracer entity API enums (core/common/entity.h), rendered as names.
std::string store_name(std::string_view v) {
    static const char* names[] = {"memory",      "gpu_memory",   "local_disk",
                                  "parallel_fs", "burst_buffer", "object_store",
                                  "database",    "network",      "other"};
    char* end = nullptr;
    std::string s(v);
    long i = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) return s.empty() ? "memory" : s;  // already a name
    return (i >= 0 && i < 9) ? names[i] : "other";
}
std::string role_name(std::string_view v) {
    static const char* names[] = {"unknown",      "input",     "output",
                                  "intermediate", "parameter", "reference"};
    char* end = nullptr;
    std::string s(v);
    long i = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) return s;
    return (i >= 0 && i < 6) ? names[i] : "unknown";
}
std::string relation_name(std::string_view v) {
    std::string s(v);
    switch (std::strtol(s.c_str(), nullptr, 10)) {
        case 16:
            return "derived_from";
        case 17:
            return "revision_of";
        case 18:
            return "contains";
        case 19:
            return "part_of";
        case 20:
            return "specialization_of";
        case 21:
            return "alternate_of";
        case 22:
            return "depends_on";
        default:
            return "related";
    }
}
constexpr std::string_view EVENT_RELATIONS[] = {"used", "generated",
                                                "invalidated", "updated"};

// Cheap byte-level prefilter: only lines that can hold provenance get parsed.
bool maybe_provenance(std::string_view line) {
    for (auto r : EVENT_RELATIONS) {
        std::string needle = "\"";
        needle.append(r).append("\":[");
        if (line.find(needle) != std::string_view::npos) return true;
    }
    return line.find("\"relations\":{") != std::string_view::npos ||
           line.find("\"name\":\"EH\"") != std::string_view::npos ||
           line.find("\"name\":\"ET\"") != std::string_view::npos ||
           line.find("\"name\":\"ER\"") != std::string_view::npos ||
           line.find("prov_") != std::string_view::npos;
}

void str_array(simdjson::dom::object o, std::string_view k,
               std::vector<std::string>& out) {
    simdjson::dom::array arr;
    if (o[k].get(arr) != simdjson::SUCCESS) return;
    for (simdjson::dom::element e : arr) {
        std::string_view sv;
        if (e.get(sv) == simdjson::SUCCESS) out.emplace_back(sv);
    }
}

struct Acc {
    std::unordered_map<std::string, ProvenanceEntity> entities;
    std::unordered_map<std::string, ProvenanceActivity> activities;
    std::unordered_map<std::string, ProvenanceType> types;
    std::vector<ProvenanceEntityRelation> entity_relations;
    ProvenanceStats stats;
};

void split_hashes(std::string_view s, std::vector<std::string>& out) {
    while (!s.empty()) {
        auto c = s.find(',');
        auto tok = s.substr(0, c);
        if (!tok.empty()) out.emplace_back(tok);
        if (c == std::string_view::npos) break;
        s.remove_prefix(c + 1);
    }
}

std::string_view str_field(simdjson::dom::object o, std::string_view k) {
    auto v = o[k];
    if (v.error() || !v.is_string()) return {};
    return v.get_string().value_unsafe();
}

std::int64_t int_field(simdjson::dom::object o, std::string_view k,
                       std::int64_t dflt) {
    auto v = o[k];
    if (v.error()) return dflt;
    if (v.is_int64()) return v.get_int64().value_unsafe();
    if (v.is_uint64())
        return static_cast<std::int64_t>(v.get_uint64().value_unsafe());
    if (v.is_string())
        return std::strtoll(std::string(v.get_string().value_unsafe()).c_str(),
                            nullptr, 10);
    return dflt;
}

void add_pid(std::vector<std::int64_t>& v, std::int64_t pid) {
    if (std::find(v.begin(), v.end(), pid) == v.end()) v.push_back(pid);
}

void ingest(Acc& a, std::string_view line) {
    thread_local simdjson::dom::parser parser;
    auto doc = parser.parse(line.data(), line.size());
    if (doc.error() || !doc.value_unsafe().is_object()) return;
    auto root = doc.value_unsafe().get_object().value_unsafe();
    auto args_r = root["args"];
    if (args_r.error() || !args_r.is_object()) return;
    auto args = args_r.get_object().value_unsafe();
    std::int64_t pid = int_field(root, "pid", 0);

    auto key = str_field(args, "name");

    // ---- dftracer entity API records (EH / ET / ER) and relation arrays.
    auto rec = str_field(root, "name");
    // Structured fields (current) or name/value with '|' (earlier prototype).
    auto field_or = [&](std::string_view k, int idx) -> std::string {
        auto v = args[k];
        if (!v.error()) {
            if (v.is_string())
                return std::string(v.get_string().value_unsafe());
            if (v.is_int64())
                return std::to_string(v.get_int64().value_unsafe());
            if (v.is_uint64())
                return std::to_string(v.get_uint64().value_unsafe());
        }
        if (idx < 0) return {};  // structured-only field (no pipe fallback)
        std::string_view val = str_field(args, "value");
        for (int i = 0; i < idx; ++i) {
            auto bar = val.find('|');
            if (bar == std::string_view::npos) return {};
            val.remove_prefix(bar + 1);
        }
        return std::string(val.substr(0, val.find('|')));
    };
    if (rec == "EH") {
        ++a.stats.entity_records;
        std::string id = field_or("id", -1);
        if (id.empty()) id = key;  // earlier layout: name = id
        if (id.empty()) return;
        auto& e = a.entities[id];
        if (e.hash.empty()) {
            e.hash = id;
            e.id = id;  // the app key is hashed, never stored
            e.type = field_or("type", 0);
            e.store = store_name(field_or("store", 1));
            e.uri = field_or("uri", 2);
        }
        add_pid(e.pids, pid);
        return;
    }
    if (rec == "ET") {
        std::string name = field_or("type", -1);
        if (name.empty()) name = key;
        if (name.empty() || a.types.count(name)) return;
        ProvenanceType t;
        t.name = name;
        t.role = role_name(field_or("role", 0));
        t.description = field_or("description", 1);
        a.types.emplace(std::move(name), std::move(t));
        return;
    }
    if (rec == "ER") {
        std::string rel = field_or("relation", -1);
        if (rel.empty()) rel = key;
        std::string subj = field_or("subject", 0), obj = field_or("object", 1);
        if (!subj.empty() && !obj.empty())
            a.entity_relations.push_back(
                ProvenanceEntityRelation{relation_name(rel), subj, obj});
        return;
    }
    {
        // Relations: args.relations{...} (current) or top-level arrays.
        simdjson::dom::object relobj = args;
        simdjson::dom::object nested;
        if (args["relations"].get(nested) == simdjson::SUCCESS) relobj = nested;
        std::vector<std::string> rel[4];
        bool any = false;
        for (int i = 0; i < 4; ++i) {
            str_array(relobj, EVENT_RELATIONS[i], rel[i]);
            any = any || !rel[i].empty();
        }
        if (any) {
            ++a.stats.prov_events;
            std::string aid = std::to_string(pid) + ":" +
                              std::to_string(int_field(root, "id", 0)) + ":" +
                              std::to_string(int_field(root, "ts", 0));
            auto& act = a.activities[aid];
            act.aid = aid;
            act.has_main = true;
            act.name = str_field(root, "name");
            act.cat = str_field(root, "cat");
            auto atype = str_field(args, "activity");
            act.activity = atype.empty() ? act.cat : std::string(atype);
            act.pid = pid;
            act.ts = static_cast<std::uint64_t>(int_field(root, "ts", 0));
            act.dur = static_cast<std::uint64_t>(int_field(root, "dur", 0));
            act.used = std::move(rel[0]);
            act.generated = std::move(rel[1]);
            act.invalidated = std::move(rel[2]);
            act.updated = std::move(rel[3]);
            return;
        }
    }

    // ---- legacy dftracer_prov helper records (prov_entity:/prov_type:/PROV).
    // Entity declaration: a metadata record keyed prov_entity:<hash>.
    // Entity type description: prov_type:<type> = "<role>|<description>".
    if (key.size() > TYPE_PREFIX.size() &&
        key.substr(0, TYPE_PREFIX.size()) == TYPE_PREFIX) {
        std::string name(key.substr(TYPE_PREFIX.size()));
        if (!a.types.count(name)) {
            std::string_view val = str_field(args, "value");
            auto bar = val.find('|');
            ProvenanceType t;
            t.name = name;
            if (bar != std::string_view::npos) {
                t.role = val.substr(0, bar);
                t.description = val.substr(bar + 1);
            } else {
                t.description = val;
            }
            a.types.emplace(std::move(name), std::move(t));
        }
        return;
    }
    if (key.size() > ENTITY_PREFIX.size() &&
        key.substr(0, ENTITY_PREFIX.size()) == ENTITY_PREFIX) {
        ++a.stats.entity_records;
        std::string hash(key.substr(ENTITY_PREFIX.size()));
        auto& e = a.entities[hash];
        if (e.hash.empty()) {
            e.hash = hash;
            std::string_view val = str_field(args, "value");
            std::string_view parts[4];
            for (auto& part : parts) {
                auto bar = val.find('|');
                part = val.substr(0, bar);
                val = bar == std::string_view::npos ? std::string_view{}
                                                    : val.substr(bar + 1);
            }
            e.type = parts[0];
            e.id = parts[1];
            e.store = parts[2].empty() ? "memory" : std::string(parts[2]);
            e.uri = parts[3];
        }
        add_pid(e.pids, pid);
        return;
    }

    auto cat = str_field(root, "cat");
    if (cat != "PROV" && cat != "PROV_CONT") return;
    auto aid = str_field(args, "prov_aid");
    if (aid.empty()) return;
    auto& act = a.activities[std::string(aid)];
    act.aid = aid;
    split_hashes(str_field(args, "prov_used"), act.used);
    split_hashes(str_field(args, "prov_generated"), act.generated);
    if (cat == "PROV_CONT") {
        ++a.stats.cont_events;
        return;
    }
    ++a.stats.prov_events;
    act.has_main = true;
    act.name = str_field(root, "name");
    act.activity = str_field(args, "prov_activity");
    act.pid = pid;
    act.ts = static_cast<std::uint64_t>(int_field(root, "ts", 0));
    act.dur = static_cast<std::uint64_t>(int_field(root, "dur", 0));
    act.nused = int_field(args, "prov_nused", -1);
    act.ngen = int_field(args, "prov_ngen", -1);
}

Acc merge(Acc&& x, Acc&& y) {
    x.entity_relations.insert(
        x.entity_relations.end(),
        std::make_move_iterator(y.entity_relations.begin()),
        std::make_move_iterator(y.entity_relations.end()));
    for (auto& [n, t] : y.types) x.types.emplace(n, std::move(t));
    for (auto& [h, e] : y.entities) {
        auto it = x.entities.find(h);
        if (it == x.entities.end()) {
            x.entities.emplace(h, std::move(e));
        } else {
            for (auto p : e.pids) add_pid(it->second.pids, p);
        }
    }
    for (auto& [aid, act] : y.activities) {
        auto it = x.activities.find(aid);
        if (it == x.activities.end()) {
            x.activities.emplace(aid, std::move(act));
            continue;
        }
        auto& t = it->second;
        t.used.insert(t.used.end(), act.used.begin(), act.used.end());
        t.generated.insert(t.generated.end(), act.generated.begin(),
                           act.generated.end());
        t.invalidated.insert(t.invalidated.end(), act.invalidated.begin(),
                             act.invalidated.end());
        t.updated.insert(t.updated.end(), act.updated.begin(),
                         act.updated.end());
        if (act.has_main) {
            t.has_main = true;
            t.name = std::move(act.name);
            t.activity = std::move(act.activity);
            t.pid = act.pid;
            t.ts = act.ts;
            t.dur = act.dur;
            t.nused = act.nused;
            t.ngen = act.ngen;
        }
    }
    x.stats.entity_records += y.stats.entity_records;
    x.stats.prov_events += y.stats.prov_events;
    x.stats.cont_events += y.stats.cont_events;
    return std::move(x);
}

ProvenanceGraph finish(Acc&& a) {
    ProvenanceGraph g;
    std::unordered_set<std::string> referenced_extra;
    g.stats = a.stats;
    for (auto& [n, t] : a.types) g.types.push_back(std::move(t));
    for (const auto& r : a.entity_relations) {
        referenced_extra.insert(r.subject);
        referenced_extra.insert(r.object);
    }
    g.entity_relations = a.entity_relations;
    std::sort(g.types.begin(), g.types.end(),
              [](const auto& x, const auto& y) { return x.name < y.name; });
    std::unordered_set<std::string_view> referenced(referenced_extra.begin(),
                                                    referenced_extra.end());
    for (const auto& [aid, act] : a.activities) {
        if (!act.has_main) ++g.stats.orphan_chunks;
        if ((act.nused >= 0 &&
             static_cast<std::size_t>(act.nused) != act.used.size()) ||
            (act.ngen >= 0 &&
             static_cast<std::size_t>(act.ngen) != act.generated.size()))
            ++g.stats.truncated_activities;
        for (const auto& h : act.used) referenced.insert(h);
        for (const auto& h : act.generated) referenced.insert(h);
        for (const auto& h : act.invalidated) referenced.insert(h);
        for (const auto& h : act.updated) referenced.insert(h);
    }
    for (const auto& [h, e] : a.entities)
        if (!referenced.count(h)) ++g.stats.isolated_entities;
    for (auto h : referenced)
        if (!a.entities.count(std::string(h))) ++g.stats.dangling_hashes;

    g.entities.reserve(a.entities.size());
    for (auto& [h, e] : a.entities) g.entities.push_back(std::move(e));
    g.activities.reserve(a.activities.size());
    for (auto& [aid, act] : a.activities)
        g.activities.push_back(std::move(act));
    // Deterministic output order.
    std::sort(g.entities.begin(), g.entities.end(),
              [](const auto& x, const auto& y) {
                  return std::tie(x.type, x.id) < std::tie(y.type, y.id);
              });
    std::sort(g.activities.begin(), g.activities.end(),
              [](const auto& x, const auto& y) {
                  return std::tie(x.ts, x.aid) < std::tie(y.ts, y.aid);
              });
    return g;
}

void str_array(simdjson::builder::string_builder& b,
               const std::vector<std::string>& v) {
    b.start_array();
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) b.append_comma();
        b.escape_and_append_with_quotes(v[i]);
    }
    b.end_array();
}

// ---------------------------------------------------------------------------
// I/O attribution: POSIX/STDIO calls -> the provenance activity running in the
// same process at that time.

struct Span {
    std::uint64_t begin = 0, end = 0;
    std::string aid;
};
using SpanIndex = std::unordered_map<std::int64_t, std::vector<Span>>;

SpanIndex index_spans(const std::vector<ProvenanceActivity>& acts) {
    SpanIndex idx;
    for (const auto& a : acts) {
        if (!a.has_main) continue;
        idx[a.pid].push_back(Span{a.ts, a.ts + a.dur, a.aid});
    }
    for (auto& [pid, v] : idx)
        std::sort(v.begin(), v.end(), [](const Span& x, const Span& y) {
            return x.begin < y.begin;
        });
    return idx;
}

// Innermost span containing ts: the latest-starting span that still covers it
// (provenance activities nest, e.g. relax inside run_inference).
const Span* find_span(const SpanIndex& idx, std::int64_t pid,
                      std::uint64_t ts) {
    auto it = idx.find(pid);
    if (it == idx.end()) return nullptr;
    const auto& v = it->second;
    auto ub = std::upper_bound(
        v.begin(), v.end(), ts,
        [](std::uint64_t t, const Span& s) { return t < s.begin; });
    int guard = 0;
    while (ub != v.begin() && guard++ < 256) {
        --ub;
        if (ub->end >= ts) return &*ub;
    }
    return nullptr;
}

std::string_view classify(std::string_view name) {
    static const std::unordered_set<std::string_view> reads = {
        "read",  "pread", "pread64", "readv",  "preadv", "fread", "fgets",
        "fgetc", "getc",  "getline", "fscanf", "mmap",   "mmap64"};
    static const std::unordered_set<std::string_view> writes = {
        "write", "pwrite", "pwrite64", "writev",  "pwritev",   "fwrite",
        "fputs", "fputc",  "putc",     "fprintf", "ftruncate", "truncate"};
    static const std::unordered_set<std::string_view> opens = {
        "open",    "open64", "openat",  "openat64", "creat",
        "creat64", "fopen",  "fopen64", "fdopen",   "freopen"};
    static const std::unordered_set<std::string_view> deletes = {
        "unlink", "unlinkat", "remove", "rmdir"};
    if (reads.count(name)) return "read";
    if (writes.count(name)) return "write";
    if (opens.count(name)) return "open";
    if (deletes.count(name)) return "delete";
    return "meta";
}

struct IoAgg {
    std::uint64_t calls = 0, bytes = 0;
};

struct IoAcc {
    // key: aid \x1f fhash \x1f op
    std::unordered_map<std::string, IoAgg> agg;
    std::unordered_map<std::string, std::string> fh;  // fhash -> absolute path
    // Relative FH paths ("." , "share/hip/version") are relative to the
    // process cwd; the same hash names different files in different pids.
    std::unordered_map<std::string, std::string> rel_fh;  // "pid:fhash" -> rel
    std::unordered_map<std::string, std::string> sh;      // string hash -> text
    std::unordered_map<std::int64_t, std::string> cwd_hash;  // pid -> SH hash
    std::uint64_t events = 0, attributed = 0;
};

void add_io(IoAcc& a, std::string_view aid, std::string_view fhash,
            std::string_view op, std::uint64_t bytes) {
    std::string key;
    key.reserve(aid.size() + fhash.size() + op.size() + 2);
    key.append(aid).push_back('\x1f');
    key.append(fhash).push_back('\x1f');
    key.append(op);
    auto& g = a.agg[key];
    ++g.calls;
    g.bytes += bytes;
}

void ingest_io(IoAcc& a, const SpanIndex& spans, std::string_view line) {
    thread_local simdjson::dom::parser parser;
    auto doc = parser.parse(line.data(), line.size());
    if (doc.error() || !doc.value_unsafe().is_object()) return;
    auto root = doc.value_unsafe().get_object().value_unsafe();
    auto args_r = root["args"];
    if (args_r.error() || !args_r.is_object()) return;
    auto args = args_r.get_object().value_unsafe();
    auto name = str_field(root, "name");
    // FH (file) and SH (string) hash records: args.name = text, .value = hash.
    if (name == "FH" || name == "SH") {
        auto text = str_field(args, "name");
        auto hash = str_field(args, "value");
        if (text.empty() || hash.empty()) return;
        if (name == "SH") {
            a.sh.emplace(std::string(hash), std::string(text));
        } else if (text.front() == '/') {
            a.fh.emplace(std::string(hash), std::string(text));
        } else {
            a.rel_fh.emplace(std::to_string(int_field(root, "pid", 0)) + ":" +
                                 std::string(hash),
                             std::string(text));
        }
        return;
    }
    if (name == "start") {  // process start record: args.cwd is an SH hash
        auto cwd = str_field(args, "cwd");
        if (!cwd.empty())
            a.cwd_hash.emplace(int_field(root, "pid", 0), std::string(cwd));
        return;
    }
    auto cat = str_field(root, "cat");
    if (cat != "POSIX" && cat != "STDIO") return;
    ++a.events;
    auto fhash = str_field(args, "fhash");
    if (fhash.empty()) return;
    const Span* sp =
        find_span(spans, int_field(root, "pid", 0),
                  static_cast<std::uint64_t>(int_field(root, "ts", 0)));
    if (sp == nullptr) return;
    ++a.attributed;
    if (name == "rename" || name == "renameat") {
        add_io(a, sp->aid, fhash, "delete", 0);
        auto np = str_field(args, "newpath_hash");
        if (!np.empty()) add_io(a, sp->aid, np, "write", 0);
        return;
    }
    auto op = classify(name);
    std::int64_t ret = int_field(args, "ret", 0);
    std::uint64_t bytes = 0;
    if ((op == "read" || op == "write") && ret > 0) {
        bytes = static_cast<std::uint64_t>(ret);
        // fread/fwrite return items, not bytes
        std::int64_t size = int_field(args, "size", 1);
        if ((name == "fread" || name == "fwrite") && size > 1)
            bytes *= static_cast<std::uint64_t>(size);
    }
    add_io(a, sp->aid, fhash, op, bytes);
}

IoAcc merge_io(IoAcc&& x, IoAcc&& y) {
    for (auto& [k, g] : y.agg) {
        auto& t = x.agg[k];
        t.calls += g.calls;
        t.bytes += g.bytes;
    }
    for (auto& [h, p] : y.fh) x.fh.emplace(h, std::move(p));
    for (auto& [h, p] : y.rel_fh) x.rel_fh.emplace(h, std::move(p));
    for (auto& [h, p] : y.sh) x.sh.emplace(h, std::move(p));
    for (auto& [pid, h] : y.cwd_hash) x.cwd_hash.emplace(pid, std::move(h));
    x.events += y.events;
    x.attributed += y.attributed;
    return std::move(x);
}

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}
bool ends_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

// Interpreter/system noise that is never a science entity.
bool excluded(std::string_view p) {
    if (p.empty() || p.front() != '/')
        return true;  // relative ("."), unresolved
    for (auto suf : {".py", ".pyc", ".pyi", ".so", ".pth", ".typed"})
        if (ends_with(p, suf)) return true;
    // Runtime caches and JIT scratch: ROCm comgr kernel compiles, Triton/DaCe
    // build dirs, XDG/OpenMM caches.
    for (auto sub : {"/site-packages/", "/dist-packages/", "/__pycache__/",
                     ".so.", "/lib/python", "/.cache/", "/comgr-", "/cache/",
                     "/triton/", "/dace/", "openmm.cache", "/cppattn_cache"})
        if (p.find(sub) != std::string_view::npos) return true;
    if (starts_with(p, "/dev/shm/")) return false;
    for (auto pre :
         {"/proc/", "/sys/", "/dev/", "/etc/", "/usr/lib", "/usr/share/",
          "/lib/", "/lib64/", "/opt/rocm", "/opt/cray/", "/collab/usr/"})
        if (starts_with(p, pre)) return true;
    return p == "/dev/null" || p.empty();
}

std::vector<ProvenanceMount> host_mount_table(
    const std::vector<std::string>& extra) {
    std::vector<ProvenanceMount> t;
    for (const auto& m : extra) {
        std::string p = m;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        if (!p.empty()) t.push_back({p, "user"});
    }
    std::ifstream in("/proc/self/mounts");
    std::string dev, mnt, fstype, rest;
    static const std::unordered_set<std::string> pseudo = {
        "proc",        "sysfs",      "cgroup",    "cgroup2",  "devpts",
        "securityfs",  "debugfs",    "tracefs",   "configfs", "pstore",
        "bpf",         "mqueue",     "hugetlbfs", "autofs",   "fusectl",
        "binfmt_misc", "rpc_pipefs", "devtmpfs",  "efivarfs", "rootfs"};
    while (in >> dev >> mnt >> fstype && std::getline(in, rest)) {
        if (mnt == "/" || pseudo.count(fstype)) continue;
        std::string un;  // /proc/mounts escapes spaces as \040
        for (std::size_t i = 0; i < mnt.size(); ++i) {
            if (mnt[i] == '\\' && i + 3 < mnt.size()) {
                un.push_back(static_cast<char>(
                    std::stoi(mnt.substr(i + 1, 3), nullptr, 8)));
                i += 3;
            } else {
                un.push_back(mnt[i]);
            }
        }
        t.push_back({un, fstype});
    }
    return t;
}

}  // namespace

std::pair<std::string, std::string> split_mount(
    const std::string& path, const std::vector<ProvenanceMount>& table) {
    const ProvenanceMount* best = nullptr;
    for (const auto& m : table) {
        if (m.path.size() <= 1) continue;
        if (path == m.path ||
            (starts_with(path, m.path) && path[m.path.size()] == '/')) {
            if (!best || m.path.size() > best->path.size()) best = &m;
        }
    }
    auto rel_of = [&](std::size_t n) {
        return n >= path.size() ? std::string(".") : path.substr(n + 1);
    };
    if (best) return {best->path, rel_of(best->path.size())};
    // Rabbit (NNF) staging mounts are per-job and absent on the serving host.
    if (starts_with(path, "/mnt/nnf/")) {
        auto e = path.find('/', 9);
        std::string m = e == std::string::npos ? path : path.substr(0, e);
        return {m, rel_of(m.size())};
    }
    // Fallback: first two components (/p/lustre5, /usr/WS2, /tmp/x).
    std::size_t n = 0;
    for (int k = 0; k < 2; ++k) {
        auto e = path.find('/', n + 1);
        if (e == std::string::npos) return {path, "."};
        n = e;
    }
    return {path.substr(0, n), rel_of(n)};
}

std::string ProvenanceGraph::to_json() const {
    simdjson::builder::string_builder b;
    b.start_object();
    b.escape_and_append_with_quotes("entities");
    b.append_colon();
    b.start_array();
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const auto& e = entities[i];
        if (i) b.append_comma();
        b.start_object();
        b.append_key_value("hash", e.hash);
        b.append_comma();
        b.append_key_value("type", e.type);
        b.append_comma();
        b.append_key_value("id", e.id);
        b.append_comma();
        b.append_key_value("store", e.store);
        b.append_comma();
        b.append_key_value("uri", e.uri);
        b.append_comma();
        b.escape_and_append_with_quotes("pids");
        b.append_colon();
        b.start_array();
        for (std::size_t k = 0; k < e.pids.size(); ++k) {
            if (k) b.append_comma();
            b.append(e.pids[k]);
        }
        b.end_array();
        b.end_object();
    }
    b.end_array();
    b.append_comma();
    b.escape_and_append_with_quotes("entity_relations");
    b.append_colon();
    b.start_array();
    for (std::size_t i = 0; i < entity_relations.size(); ++i) {
        if (i) b.append_comma();
        b.start_object();
        b.append_key_value("relation", entity_relations[i].relation);
        b.append_comma();
        b.append_key_value("subject", entity_relations[i].subject);
        b.append_comma();
        b.append_key_value("object", entity_relations[i].object);
        b.end_object();
    }
    b.end_array();
    b.append_comma();
    b.escape_and_append_with_quotes("types");
    b.append_colon();
    b.start_array();
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (i) b.append_comma();
        b.start_object();
        b.append_key_value("name", types[i].name);
        b.append_comma();
        b.append_key_value("role", types[i].role);
        b.append_comma();
        b.append_key_value("description", types[i].description);
        b.end_object();
    }
    b.end_array();
    b.append_comma();
    b.escape_and_append_with_quotes("activities");
    b.append_colon();
    b.start_array();
    for (std::size_t i = 0; i < activities.size(); ++i) {
        const auto& a = activities[i];
        if (i) b.append_comma();
        b.start_object();
        b.append_key_value("aid", a.aid);
        b.append_comma();
        b.append_key_value("name", a.name);
        b.append_comma();
        b.append_key_value("activity", a.activity);
        b.append_comma();
        b.append_key_value("pid", a.pid);
        b.append_comma();
        b.append_key_value("ts", a.ts);
        b.append_comma();
        b.append_key_value("dur", a.dur);
        b.append_comma();
        b.escape_and_append_with_quotes("used");
        b.append_colon();
        str_array(b, a.used);
        b.append_comma();
        b.escape_and_append_with_quotes("generated");
        b.append_colon();
        str_array(b, a.generated);
        b.append_comma();
        b.escape_and_append_with_quotes("invalidated");
        b.append_colon();
        str_array(b, a.invalidated);
        b.append_comma();
        b.escape_and_append_with_quotes("updated");
        b.append_colon();
        str_array(b, a.updated);
        b.append_comma();
        b.append_key_value("cat", a.cat);
        b.end_object();
    }
    b.end_array();
    b.append_comma();
    b.escape_and_append_with_quotes("files");
    b.append_colon();
    b.start_array();
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        if (i) b.append_comma();
        b.start_object();
        b.append_key_value("fhash", f.fhash);
        b.append_comma();
        b.append_key_value("path", f.path);
        b.append_comma();
        b.append_key_value("mount", f.mount);
        b.append_comma();
        b.append_key_value("rel", f.rel);
        b.end_object();
    }
    b.end_array();
    b.append_comma();
    b.escape_and_append_with_quotes("mounts");
    b.append_colon();
    b.start_array();
    for (std::size_t i = 0; i < mounts.size(); ++i) {
        if (i) b.append_comma();
        b.start_object();
        b.append_key_value("path", mounts[i].path);
        b.append_comma();
        b.append_key_value("fstype", mounts[i].fstype);
        b.end_object();
    }
    b.end_array();
    b.append_comma();
    b.escape_and_append_with_quotes("accesses");
    b.append_colon();
    b.start_array();
    for (std::size_t i = 0; i < accesses.size(); ++i) {
        const auto& x = accesses[i];
        if (i) b.append_comma();
        b.start_object();
        b.append_key_value("aid", x.aid);
        b.append_comma();
        b.append_key_value("fhash", x.fhash);
        b.append_comma();
        b.append_key_value("path", x.path);
        b.append_comma();
        b.append_key_value("op", x.op);
        b.append_comma();
        b.append_key_value("calls", x.calls);
        b.append_comma();
        b.append_key_value("bytes", x.bytes);
        b.end_object();
    }
    b.end_array();
    b.append_comma();
    b.escape_and_append_with_quotes("entity_files");
    b.append_colon();
    b.start_array();
    for (std::size_t i = 0; i < entity_files.size(); ++i) {
        if (i) b.append_comma();
        b.start_object();
        b.append_key_value("entity", entity_files[i].entity);
        b.append_comma();
        b.append_key_value("fhash", entity_files[i].fhash);
        b.append_comma();
        b.append_key_value("path", entity_files[i].path);
        b.end_object();
    }
    b.end_array();
    b.append_comma();
    b.escape_and_append_with_quotes("stats");
    b.append_colon();
    b.start_object();
    b.append_key_value("entities", entities.size());
    b.append_comma();
    b.append_key_value("activities", activities.size());
    b.append_comma();
    b.append_key_value("entity_records", stats.entity_records);
    b.append_comma();
    b.append_key_value("prov_events", stats.prov_events);
    b.append_comma();
    b.append_key_value("cont_events", stats.cont_events);
    b.append_comma();
    b.append_key_value("dangling_hashes", stats.dangling_hashes);
    b.append_comma();
    b.append_key_value("isolated_entities", stats.isolated_entities);
    b.append_comma();
    b.append_key_value("truncated_activities", stats.truncated_activities);
    b.append_comma();
    b.append_key_value("orphan_chunks", stats.orphan_chunks);
    b.append_comma();
    b.append_key_value("files", files.size());
    b.append_comma();
    b.append_key_value("mounts", mounts.size());
    b.append_comma();
    b.append_key_value("io_events", stats.io_events);
    b.append_comma();
    b.append_key_value("io_attributed", stats.io_attributed);
    b.append_comma();
    b.append_key_value("files_excluded", stats.files_excluded);
    b.end_object();
    b.end_object();
    return std::string(b);
}

coro::CoroTask<ProvenanceGraph> extract_provenance_graph(
    const views::View& view, std::size_t num_slots, ProvenanceOptions options) {
    if (num_slots == 0) num_slots = 1;
    // Relations may sit on events of any category (the entity API attaches
    // them to whatever event the app relates), so scan everything and let a
    // byte-level prefilter skip lines that cannot hold provenance.
    views::View v = view.phase(views::Phase::Any);
    auto scan = co_await v.map_batches<Acc>(
        [](Acc& a, const std::vector<std::string_view>& events) {
            for (auto ev : events)
                if (maybe_provenance(ev)) ingest(a, ev);
        },
        [](Acc&& x, Acc&& y) { return merge(std::move(x), std::move(y)); },
        num_slots, 0);
    ProvenanceGraph g = finish(std::move(scan.value));
    if (!options.include_io || g.activities.empty()) co_return g;

    // Second pass: POSIX/STDIO calls inside provenance activities.
    const SpanIndex spans = index_spans(g.activities);
    views::View io =
        view.phase(views::Phase::Any)
            .filter(duql::parse_or_throw(
                "cat == \"POSIX\" or cat == \"STDIO\" or name == \"FH\" or "
                "name == \"SH\" or name == \"start\""));
    auto ios = co_await io.map_batches<IoAcc>(
        [&spans](IoAcc& a, const std::vector<std::string_view>& events) {
            for (auto ev : events) ingest_io(a, spans, ev);
        },
        [](IoAcc&& x, IoAcc&& y) {
            return merge_io(std::move(x), std::move(y));
        },
        num_slots, 0);
    IoAcc& io_acc = ios.value;
    g.stats.io_events = io_acc.events;
    g.stats.io_attributed = io_acc.attributed;

    auto keep = [&](const std::string& path) {
        return options.include_all_files || !excluded(path);
    };
    std::unordered_map<std::string, std::int64_t> pid_of;  // aid -> pid
    for (const auto& a : g.activities) pid_of[a.aid] = a.pid;
    // fhash as seen by `pid` -> absolute path ("" when it cannot be resolved).
    // Relative FH paths resolve against that process's cwd (start record).
    auto resolve = [&](const std::string& fhash,
                       std::int64_t pid) -> std::string {
        if (auto it = io_acc.fh.find(fhash); it != io_acc.fh.end())
            return it->second;
        auto rt = io_acc.rel_fh.find(std::to_string(pid) + ":" + fhash);
        if (rt == io_acc.rel_fh.end()) return {};
        auto ch = io_acc.cwd_hash.find(pid);
        if (ch == io_acc.cwd_hash.end()) return {};
        auto cwd = io_acc.sh.find(ch->second);
        if (cwd == io_acc.sh.end() || cwd->second.empty()) return {};
        std::string rel = rt->second;
        while (rel.rfind("./", 0) == 0) rel.erase(0, 2);
        if (rel.empty() || rel == ".") return cwd->second;
        return cwd->second + (cwd->second.back() == '/' ? "" : "/") + rel;
    };
    std::unordered_map<std::string, ProvenanceFile> files;  // by abs path
    std::unordered_set<std::string> dropped;
    auto add_file = [&](const std::string& fhash,
                        const std::string& path) -> bool {
        if (path.empty()) return false;
        if (files.count(path)) return true;
        if (!keep(path)) {
            dropped.insert(path);
            return false;
        }
        files[path] = ProvenanceFile{fhash, path, "", ""};
        return true;
    };
    for (auto& [key, agg] : io_acc.agg) {
        auto p1 = key.find('\x1f');
        auto p2 = key.find('\x1f', p1 + 1);
        std::string aid = key.substr(0, p1);
        std::string fhash = key.substr(p1 + 1, p2 - p1 - 1);
        std::string path = resolve(fhash, pid_of[aid]);
        if (!add_file(fhash, path)) continue;
        g.accesses.push_back(ProvenanceAccess{
            aid, fhash, path, key.substr(p2 + 1), agg.calls, agg.bytes});
    }
    // Entity <-> file: the uri names the file, or prefixes it (a DB prefix
    // like pdb70 -> pdb70_a3m.ffdata, or a directory).
    for (const auto& e : g.entities) {
        if (e.uri.size() < 2 || e.uri.front() != '/') continue;
        for (const auto& [fh, path] : io_acc.fh) {
            bool match =
                path == e.uri ||
                (starts_with(path, e.uri) &&
                 (e.uri.back() == '/' || path[e.uri.size()] == '/' ||
                  path[e.uri.size()] == '_' || path[e.uri.size()] == '.'));
            if (match && add_file(fh, path))
                g.entity_files.push_back(
                    ProvenanceEntityFile{e.hash, fh, path});
        }
    }
    g.stats.files_excluded = dropped.size();

    auto table = host_mount_table(options.mounts);
    std::unordered_map<std::string, std::string> mount_type;
    for (auto& [fpath, f] : files) {
        auto [m, rel] = split_mount(f.path, table);
        f.mount = m;
        f.rel = rel;
        if (!mount_type.count(m)) {
            std::string t = "unknown";
            for (const auto& mt : table)
                if (mt.path == m) t = mt.fstype;
            if (t == "unknown" && starts_with(m, "/mnt/nnf/")) t = "rabbit";
            mount_type[m] = t;
        }
        g.files.push_back(std::move(f));
    }
    for (auto& [m, t] : mount_type) g.mounts.push_back(ProvenanceMount{m, t});
    std::sort(g.files.begin(), g.files.end(),
              [](const auto& x, const auto& y) { return x.path < y.path; });
    std::sort(g.mounts.begin(), g.mounts.end(),
              [](const auto& x, const auto& y) { return x.path < y.path; });
    std::sort(g.accesses.begin(), g.accesses.end(),
              [](const auto& x, const auto& y) {
                  return std::tie(x.aid, x.path, x.op) <
                         std::tie(y.aid, y.path, y.op);
              });
    co_return g;
}

}  // namespace dftracer::utils::trace::provenance
