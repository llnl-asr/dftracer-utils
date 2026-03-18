#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>

#include <string_view>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;

std::vector<ProvenanceSource> query_provenance_sources(const SqliteDatabase& db,
                                                       int file_info_id) {
    SqliteStmt stmt(db,
                    "SELECT source_idx, path, num_checkpoints, "
                    "event_hash "
                    "FROM provenance_sources "
                    "WHERE file_info_id = ? "
                    "ORDER BY source_idx;");
    stmt.bind_int(1, file_info_id);

    std::vector<ProvenanceSource> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ProvenanceSource s;
        s.source_idx = sqlite3_column_int(stmt, 0);
        s.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        s.num_checkpoints = sqlite3_column_int(stmt, 2);
        s.event_hash =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        results.push_back(std::move(s));
    }
    return results;
}

std::vector<ProvenanceSegment> query_provenance_segments(
    const SqliteDatabase& db, int source_idx) {
    SqliteStmt stmt(db,
                    "SELECT source_idx, source_checkpoint, "
                    "output_line_start, output_line_end, "
                    "event_count "
                    "FROM provenance_segments "
                    "WHERE source_idx = ? "
                    "ORDER BY source_checkpoint, "
                    "output_line_start;");
    stmt.bind_int(1, source_idx);

    std::vector<ProvenanceSegment> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ProvenanceSegment s;
        s.source_idx = sqlite3_column_int(stmt, 0);
        s.source_checkpoint = sqlite3_column_int(stmt, 1);
        s.output_line_start = sqlite3_column_int(stmt, 2);
        s.output_line_end = sqlite3_column_int(stmt, 3);
        s.event_count = sqlite3_column_int(stmt, 4);
        results.push_back(std::move(s));
    }
    return results;
}

std::vector<ProvenanceSegment> query_all_provenance_segments(
    const SqliteDatabase& db) {
    SqliteStmt stmt(db,
                    "SELECT source_idx, source_checkpoint, "
                    "output_line_start, output_line_end, "
                    "event_count "
                    "FROM provenance_segments "
                    "ORDER BY output_line_start;");

    std::vector<ProvenanceSegment> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ProvenanceSegment s;
        s.source_idx = sqlite3_column_int(stmt, 0);
        s.source_checkpoint = sqlite3_column_int(stmt, 1);
        s.output_line_start = sqlite3_column_int(stmt, 2);
        s.output_line_end = sqlite3_column_int(stmt, 3);
        s.event_count = sqlite3_column_int(stmt, 4);
        results.push_back(std::move(s));
    }
    return results;
}

std::string query_provenance_info(const SqliteDatabase& db,
                                  std::string_view key) {
    SqliteStmt stmt(db,
                    "SELECT value FROM provenance_info "
                    "WHERE key = ?;");
    stmt.bind_text(1, key);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    return "";
}

std::string query_provenance_group_name(const SqliteDatabase& db) {
    SqliteStmt stmt(db, "SELECT name FROM provenance_group LIMIT 1;");

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    return "";
}

std::string query_provenance_group_predicate(const SqliteDatabase& db) {
    SqliteStmt stmt(db,
                    "SELECT predicate FROM provenance_group "
                    "LIMIT 1;");

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) {
            return "";
        }
        return reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    return "";
}

}  // namespace
   // dftracer::utils::utilities::composites::dft::indexing::queries
