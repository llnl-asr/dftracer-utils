#ifndef DFTRACER_UTILS_TRACE_PROVENANCE_PROVENANCE_GRAPH_H
#define DFTRACER_UTILS_TRACE_PROVENANCE_PROVENANCE_GRAPH_H

// Provenance graph extraction from dftracer traces.
//
// dftracer's provenance mode (dftracer_prov.h / dftracer_prov.py) records:
//   * one metadata record per entity instance per process:
//       args.name  = "prov_entity:<16-hex hash>"
//       args.value = "<type>|<id>|<store>|<uri>"
//     where hash = FNV-1a-64(type + 0x1f + id), identical in every language.
//   * one event per activity, cat "PROV", whose args carry the edges:
//       prov_aid       unique activity instance id
//       prov_activity  activity type
//       prov_used      comma-separated entity hashes consumed  (cause)
//       prov_generated comma-separated entity hashes produced  (effect)
//       prov_nused / prov_ngen   expected counts (detects truncation)
//   * cat "PROV_CONT" events carrying overflowed list chunks, sharing prov_aid.
//
// extract_provenance_graph() runs one indexed, parallel scan through a
// trace::views::View (the engine behind TraceViewer / dftracer_view),
// dedupes entities across processes and folds PROV_CONT chunks into their
// activity, so callers get the graph rather than raw events.

#include <dftracer/utils/core/coro/task.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::views {
class View;
}

namespace dftracer::utils::trace::provenance {

struct ProvenanceEntity {
    std::string hash;   // 16 hex digits
    std::string type;   // entity type, e.g. "relaxed_structure"
    std::string id;     // instance id, e.g. "MGYP001796749278/model_1_ptm"
    std::string store;  // memory | file | db | object | network | ...
    std::string uri;
    std::vector<std::int64_t> pids;  // processes that declared it
};

struct ProvenanceActivity {
    std::string aid;       // unique activity instance id
    std::string name;      // event name
    std::string activity;  // activity type
    std::int64_t pid = 0;
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;
    std::string cat;                       // event category
    std::vector<std::string> used;         // cause entity ids (16-hex)
    std::vector<std::string> generated;    // effect entity ids
    std::vector<std::string> invalidated;  // entities this event ended
    std::vector<std::string> updated;      // entities modified in place
    std::int64_t nused = -1;  // expected counts as reported (-1: absent)
    std::int64_t ngen = -1;
    bool has_main = false;    // false: only PROV_CONT chunks were seen
};

/// An entity type as described by the producer (prov_type:<type> metadata,
/// value "<role>|<description>"): its role in the workflow and what it is.
struct ProvenanceType {
    std::string name;         // entity type, e.g. "best_structure"
    std::string role;         // input | output | intermediate ("" if not given)
    std::string description;  // what the type represents in the workflow
};

/// Entity -> entity relation from an ER record (dftracer entity API), e.g.
/// subject CONTAINS object, subject DERIVED_FROM object.
struct ProvenanceEntityRelation {
    std::string relation;  // contains | part_of | derived_from | revision_of |
                           // specialization_of | alternate_of | depends_on
    std::string subject;   // entity id (16-hex)
    std::string object;
};

/// A storage location (mount point) holding provenance-relevant files.
struct ProvenanceMount {
    std::string path;  // e.g. "/p/lustre5", "/mnt/nnf/<id>"
    std::string
        fstype;        // from the mount table; "rabbit"/"unknown" when inferred
};

/// A file touched by a provenance activity or named by an entity's uri.
struct ProvenanceFile {
    std::string fhash;  // dftracer file hash (POSIX/STDIO fhash)
    std::string path;   // absolute path: the FH record, or a relative FH path
                        // resolved against the process cwd (start event)
    std::string mount;  // owning mount point (ProvenanceMount::path)
    std::string rel;    // path relative to `mount`
};

/// One activity's I/O on one file, aggregated over its POSIX/STDIO calls.
struct ProvenanceAccess {
    std::string aid;    // ProvenanceActivity::aid
    std::string fhash;  // ProvenanceFile::fhash
    std::string path;   // ProvenanceFile::path (the file's identity)
    std::string op;     // read | write | open | delete | meta
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
};

/// Entity <-> file association: the entity's uri is (or prefixes) the file.
struct ProvenanceEntityFile {
    std::string entity;  // ProvenanceEntity::hash
    std::string fhash;
    std::string path;    // ProvenanceFile::path
};

struct ProvenanceStats {
    std::uint64_t entity_records = 0;   // prov_entity metadata records scanned
    std::uint64_t prov_events = 0;
    std::uint64_t cont_events = 0;
    std::uint64_t dangling_hashes = 0;  // referenced, never declared
    std::uint64_t isolated_entities = 0;  // declared, never referenced
    std::uint64_t truncated_activities = 0;
    std::uint64_t orphan_chunks = 0;      // PROV_CONT without its PROV event
    std::uint64_t io_events = 0;          // POSIX/STDIO events scanned
    std::uint64_t io_attributed = 0;      // ... inside a provenance activity
    std::uint64_t files_excluded = 0;     // dropped by the exclusion rules
};

struct ProvenanceOptions {
    /// Attribute POSIX/STDIO I/O to activities and emit files/mounts.
    bool include_io = true;
    /// Keep interpreter/system files (.py, .so, site-packages, /proc, ...).
    bool include_all_files = false;
    /// Extra mount points (absolute prefixes) checked before the host mount
    /// table, e.g. the compute nodes' mounts when serving from a login node.
    std::vector<std::string> mounts;
};

struct ProvenanceGraph {
    std::vector<ProvenanceEntity> entities;
    std::vector<ProvenanceActivity> activities;
    std::vector<ProvenanceType> types;
    std::vector<ProvenanceEntityRelation> entity_relations;
    std::vector<ProvenanceFile> files;
    std::vector<ProvenanceMount> mounts;
    std::vector<ProvenanceAccess> accesses;
    std::vector<ProvenanceEntityFile> entity_files;
    ProvenanceStats stats;

    /// {"entities":[...],"activities":[...],"stats":{...}}
    std::string to_json() const;
};

/// Scan `view` for provenance records and assemble the graph.
///
/// The view's existing scoping (files, time range, pid filters) is kept;
/// this adds phase(Any) and scans every event, because the dftracer entity
/// API attaches relation arrays (used/generated/invalidated/updated) to
/// events of any category; a byte-level prefilter keeps that cheap.
/// `num_slots` is the worker parallelism (0 -> 1).
coro::CoroTask<ProvenanceGraph> extract_provenance_graph(
    const views::View& view, std::size_t num_slots,
    ProvenanceOptions options = {});

/// Split `path` into (mount point, path relative to it) using `table`
/// (longest prefix wins), then the Rabbit rule (/mnt/nnf/<id>), then the
/// first two path components. Exposed for tests.
std::pair<std::string, std::string> split_mount(
    const std::string& path, const std::vector<ProvenanceMount>& table);

}  // namespace dftracer::utils::trace::provenance

#endif  // DFTRACER_UTILS_TRACE_PROVENANCE_PROVENANCE_GRAPH_H
