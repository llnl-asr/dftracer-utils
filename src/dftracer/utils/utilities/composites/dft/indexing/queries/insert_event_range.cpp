#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

#include <cstring>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;
using indexer::internal::IndexerError;

std::vector<unsigned char> pack_line_numbers(
    const std::vector<std::uint32_t>& lines) {
    std::vector<unsigned char> blob(lines.size() * sizeof(std::uint32_t));
    std::memcpy(blob.data(), lines.data(), blob.size());
    return blob;
}

std::vector<std::uint32_t> unpack_line_numbers(const unsigned char* data,
                                               std::size_t size) {
    std::size_t count = size / sizeof(std::uint32_t);
    std::vector<std::uint32_t> lines(count);
    std::memcpy(lines.data(), data, size);
    return lines;
}

void insert_event_range(const SqliteDatabase& db, int file_info_id,
                        std::uint64_t checkpoint_idx, const std::string& cat,
                        const std::string& name,
                        const std::vector<std::uint32_t>& line_numbers) {
    auto blob = pack_line_numbers(line_numbers);

    SqliteStmt stmt(db,
                    "INSERT OR REPLACE INTO checkpoint_event_ranges"
                    "(checkpoint_idx, file_info_id, cat, name, "
                    "line_numbers, event_count) "
                    "VALUES(?, ?, ?, ?, ?, ?);");

    stmt.bind_int64(1, static_cast<std::int64_t>(checkpoint_idx));
    stmt.bind_int(2, file_info_id);
    stmt.bind_text(3, cat);
    stmt.bind_text(4, name);
    stmt.bind_blob(5, blob.data(), static_cast<int>(blob.size()));
    stmt.bind_int64(6, static_cast<std::int64_t>(line_numbers.size()));

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert event range: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

}  // namespace
   // dftracer::utils::utilities::composites::dft::indexing::queries
