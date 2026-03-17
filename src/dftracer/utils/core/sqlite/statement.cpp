#include <dftracer/utils/core/sqlite/database.h>
#include <dftracer/utils/core/sqlite/error.h>
#include <dftracer/utils/core/sqlite/statement.h>

#include <cstddef>
#include <span>

namespace dftracer::utils::sqlite {

SqliteStmt::SqliteStmt(const SqliteDatabase &db, const char *sql) {
    sqlite3 *raw_db = db.get();
    if (sqlite3_prepare_v2(raw_db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
        stmt_ = nullptr;
        throw SqliteError(SqliteError::Type::STATEMENT_ERROR,
                          "Failed to prepare SQL statement: " +
                              std::string(sqlite3_errmsg(raw_db)));
    }
}

SqliteStmt::SqliteStmt(sqlite3 *db, const char *sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
        stmt_ = nullptr;
        throw SqliteError(SqliteError::Type::STATEMENT_ERROR,
                          "Failed to prepare SQL statement: " +
                              std::string(sqlite3_errmsg(db)));
    }
}

SqliteStmt::~SqliteStmt() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
    }
}

SqliteStmt::operator sqlite3_stmt *() { return stmt_; }

sqlite3_stmt *SqliteStmt::get() { return stmt_; }

void SqliteStmt::reset() { sqlite3_reset(stmt_); }

void SqliteStmt::bind_int(int index, int value) {
    validate_parameter_index(index);
    int rc = sqlite3_bind_int(stmt_, index, value);
    if (rc != SQLITE_OK) {
        throw SqliteError(
            SqliteError::Type::STATEMENT_ERROR,
            "Failed to bind int parameter at index " + std::to_string(index));
    }
}

void SqliteStmt::bind_int64(int index, int64_t value) {
    validate_parameter_index(index);
    int rc = sqlite3_bind_int64(stmt_, index, value);
    if (rc != SQLITE_OK) {
        throw SqliteError(
            SqliteError::Type::STATEMENT_ERROR,
            "Failed to bind int64 parameter at index " + std::to_string(index));
    }
}

void SqliteStmt::bind_double(int index, double value) {
    validate_parameter_index(index);
    int rc = sqlite3_bind_double(stmt_, index, value);
    if (rc != SQLITE_OK) {
        throw SqliteError(SqliteError::Type::STATEMENT_ERROR,
                          "Failed to bind double parameter at index " +
                              std::to_string(index));
    }
}

void SqliteStmt::bind_text(int index, const std::string &text) {
    validate_parameter_index(index);
    int rc =
        sqlite3_bind_text(stmt_, index, text.c_str(),
                          static_cast<int>(text.length()), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        throw SqliteError(
            SqliteError::Type::STATEMENT_ERROR,
            "Failed to bind text parameter at index " + std::to_string(index));
    }
}

void SqliteStmt::bind_text(int index, std::string_view text) {
    validate_parameter_index(index);
    int rc = sqlite3_bind_text(stmt_, index, text.data(),
                               static_cast<int>(text.size()), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        throw SqliteError(
            SqliteError::Type::STATEMENT_ERROR,
            "Failed to bind text parameter at index " + std::to_string(index));
    }
}

void SqliteStmt::bind_text(int index, const char *text, int length,
                           void (*destructor)(void *)) {
    validate_parameter_index(index);
    int rc = sqlite3_bind_text(stmt_, index, text, length, destructor);
    if (rc != SQLITE_OK) {
        throw SqliteError(
            SqliteError::Type::STATEMENT_ERROR,
            "Failed to bind text parameter at index " + std::to_string(index));
    }
}

void SqliteStmt::bind_blob(int index, const void *blob, int length) {
    validate_parameter_index(index);
    int rc = sqlite3_bind_blob(stmt_, index, blob, length, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        throw SqliteError(
            SqliteError::Type::STATEMENT_ERROR,
            "Failed to bind blob parameter at index " + std::to_string(index));
    }
}

void SqliteStmt::bind_blob(int index, std::span<const std::byte> data) {
    bind_blob(index, data.data(), static_cast<int>(data.size()));
}

void SqliteStmt::bind_blob(int index, std::span<const unsigned char> data) {
    bind_blob(index, data.data(), static_cast<int>(data.size()));
}

void SqliteStmt::bind_null(int index) {
    validate_parameter_index(index);
    int rc = sqlite3_bind_null(stmt_, index);
    if (rc != SQLITE_OK) {
        throw SqliteError(
            SqliteError::Type::STATEMENT_ERROR,
            "Failed to bind null parameter at index " + std::to_string(index));
    }
}

void SqliteStmt::clear_bindings() { sqlite3_clear_bindings(stmt_); }

int SqliteStmt::bind_parameter_count() {
    return sqlite3_bind_parameter_count(stmt_);
}

void SqliteStmt::validate_parameter_index(int index) {
    if (index < 1) {
        throw SqliteError(
            SqliteError::Type::STATEMENT_ERROR,
            "Parameter index must be >= 1 (got " + std::to_string(index) + ")");
    }
    int param_count = sqlite3_bind_parameter_count(stmt_);
    if (index > param_count) {
        throw SqliteError(SqliteError::Type::STATEMENT_ERROR,
                          "Parameter index " + std::to_string(index) +
                              " exceeds parameter count " +
                              std::to_string(param_count));
    }
}

}  // namespace dftracer::utils::sqlite
