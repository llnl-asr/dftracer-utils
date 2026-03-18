#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/indexer/internal/error.h>

#include <span>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft::indexing::queries {

using dftracer::utils::sqlite::SqliteStmt;
using indexer::internal::IndexerError;

void insert_metadata_lines(const SqliteDatabase& db, int file_info_id,
                           std::uint64_t checkpoint_idx,
                           std::string_view meta_type,
                           const std::vector<std::uint32_t>& line_numbers) {
    auto blob = pack_line_numbers(line_numbers);

    SqliteStmt stmt(db,
                    "INSERT OR REPLACE INTO checkpoint_metadata_lines"
                    "(checkpoint_idx, file_info_id, meta_type, "
                    "line_numbers) "
                    "VALUES(?, ?, ?, ?);");

    stmt.bind_int64(1, static_cast<std::int64_t>(checkpoint_idx));
    stmt.bind_int(2, file_info_id);
    stmt.bind_text(3, meta_type);
    stmt.bind_blob(4, blob.data(), static_cast<int>(blob.size()));

    int result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Failed to insert metadata lines: " +
                               std::string(sqlite3_errmsg(db.get())));
    }
}

void insert_metadata_lines(const SqliteDatabase& db, int file_info_id,
                           std::uint64_t checkpoint_idx,
                           std::string_view meta_type,
                           std::span<const std::uint32_t> line_numbers) {
    std::vector<std::uint32_t> vec(line_numbers.begin(), line_numbers.end());
    insert_metadata_lines(db, file_info_id, checkpoint_idx, meta_type, vec);
}

}  // namespace
   // dftracer::utils::utilities::composites::dft::indexing::queries
