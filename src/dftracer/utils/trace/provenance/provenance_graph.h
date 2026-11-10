// SPDX-License-Identifier: MIT
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

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftracer::utils::trace::views {
class View;
}

namespace dftracer::utils::trace::provenance {

struct ProvenanceEntity {
    dstd::String hash;   // 16 hex digits
    dstd::String type;   // entity type, e.g. "relaxed_structure"
    dstd::String id;     // instance id, e.g. "MGYP001796749278/model_1_ptm"
    dstd::String store;  // memory | file | db | object | network | ...
    dstd::String uri;
    dstd::Vector<std::int64_t> pids;  // processes that declared it
};

struct ProvenanceActivity {
    dstd::String aid;       // unique activity instance id
    dstd::String name;      // event name
    dstd::String activity;  // activity type
    std::int64_t pid = 0;
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;
    dstd::String cat;                        // event category
    dstd::Vector<dstd::String> used;         // cause entity ids (16-hex)
    dstd::Vector<dstd::String> generated;    // effect entity ids
    dstd::Vector<dstd::String> invalidated;  // entities this event ended
    dstd::Vector<dstd::String> updated;      // entities modified in place
    std::int64_t nused = -1;  // expected counts as reported (-1: absent)
    std::int64_t ngen = -1;
    bool has_main = false;    // false: only PROV_CONT chunks were seen
};

/// An entity type as described by the producer (prov_type:\<type\> metadata,
/// value "<role>|<description>"): its role in the workflow and what it is.
struct ProvenanceType {
    dstd::String name;  // entity type, e.g. "best_structure"
    dstd::String role;  // input | output | intermediate ("" if not given)
    dstd::String description;  // what the type represents in the workflow
};

/// Entity -> entity relation from an ER record (dftracer entity API), e.g.
/// subject CONTAINS object, subject DERIVED_FROM object.
struct ProvenanceEntityRelation {
    dstd::String relation;  // contains | part_of | derived_from | revision_of |
                            // specialization_of | alternate_of | depends_on
    dstd::String subject;   // entity id (16-hex)
    dstd::String object;
};

/// A storage location (mount point) holding provenance-relevant files.
struct ProvenanceMount {
    dstd::String path;  // e.g. "/p/lustre5", "/mnt/nnf/<id>"
    dstd::String
        fstype;  // from the mount table; "rabbit"/"unknown" when inferred
};

/// A file touched by a provenance activity or named by an entity's uri.
struct ProvenanceFile {
    dstd::String fhash;  // dftracer file hash (POSIX/STDIO fhash)
    dstd::String path;   // absolute path: the FH record, or a relative FH path
                         // resolved against the process cwd (start event)
    dstd::String mount;  // owning mount point (ProvenanceMount::path)
    dstd::String rel;    // path relative to `mount`
};

/// One activity's I/O on one file, aggregated over its POSIX/STDIO calls.
struct ProvenanceAccess {
    dstd::String aid;    // ProvenanceActivity::aid
    dstd::String fhash;  // ProvenanceFile::fhash
    dstd::String path;   // ProvenanceFile::path (the file's identity)
    dstd::String op;     // read | write | open | delete | meta
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
};

/// Entity <-> file association: the entity's uri is (or prefixes) the file.
struct ProvenanceEntityFile {
    dstd::String entity;  // ProvenanceEntity::hash
    dstd::String fhash;
    dstd::String path;    // ProvenanceFile::path
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
    dstd::Vector<dstd::String> mounts;
};

struct ProvenanceGraph {
    dstd::Vector<ProvenanceEntity> entities;
    dstd::Vector<ProvenanceActivity> activities;
    dstd::Vector<ProvenanceType> types;
    dstd::Vector<ProvenanceEntityRelation> entity_relations;
    dstd::Vector<ProvenanceFile> files;
    dstd::Vector<ProvenanceMount> mounts;
    dstd::Vector<ProvenanceAccess> accesses;
    dstd::Vector<ProvenanceEntityFile> entity_files;
    ProvenanceStats stats;

    /// {"entities":[...],"activities":[...],"stats":{...}}
    dstd::String to_json() const;
};

/// Scan `view` for provenance records and assemble the graph.
///
/// The view's existing scoping (files, time range, pid filters) is kept;
/// this adds phase(Any) and scans every event, because the dftracer entity
/// API attaches relation arrays (used/generated/invalidated/updated) to
/// events of any category; a byte-level prefilter keeps that cheap.
/// `slot_count` is the worker parallelism (0 -> 1). A failed scan gives the
/// error of the view's scan.
coro::CoroTask<dstd::Result<ProvenanceGraph>> extract_provenance_graph(
    const views::View& view, std::size_t slot_count,
    ProvenanceOptions options = {});

/// Parses a query text that is part of the code. A text that does not parse
/// is a bug of the caller and ends the process. Exposed for tests.
duql::Query constant_query(const char* text);

/// Split `path` into (mount point, path relative to it) using `table`
/// (longest prefix wins), then the Rabbit rule (/mnt/nnf/\<id\>), then the
/// first two path components. Exposed for tests.
dstd::Pair<dstd::String, dstd::String> split_mount(
    const dstd::String& path, const dstd::Vector<ProvenanceMount>& table);

}  // namespace dftracer::utils::trace::provenance

#endif  // DFTRACER_UTILS_TRACE_PROVENANCE_PROVENANCE_GRAPH_H
