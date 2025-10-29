#ifndef DFTRACER_UTILS_INDEXER_SQLITE_STATEMENT_H
#define DFTRACER_UTILS_INDEXER_SQLITE_STATEMENT_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/indexer/error.h>
#include <dftracer/utils/indexer/sqlite/database.h>
#include <sqlite3.h>

#include <string>

using namespace dftracer::utils;

class SqliteStmt {
   public:
    SqliteStmt(const SqliteDatabase &db, const char *sql) {
        sqlite3 *raw_db = db.get();
        if (sqlite3_prepare_v2(raw_db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            stmt_ = nullptr;
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Failed to prepare SQL statement: " +
                                   std::string(sqlite3_errmsg(raw_db)));
        }
    }

    SqliteStmt(sqlite3 *db, const char *sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            stmt_ = nullptr;
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Failed to prepare SQL statement: " +
                                   std::string(sqlite3_errmsg(db)));
        }
    }

    ~SqliteStmt() {
        if (stmt_) {
            sqlite3_finalize(stmt_);
        }
    }

    SqliteStmt(const SqliteStmt &) = delete;
    SqliteStmt &operator=(const SqliteStmt &) = delete;

    operator sqlite3_stmt *() { return stmt_; }
    sqlite3_stmt *get() { return stmt_; }

    void reset() { sqlite3_reset(stmt_); }

    void bind_int(int index, int value) {
        validate_parameter_index(index);
        int rc = sqlite3_bind_int(stmt_, index, value);
        if (rc != SQLITE_OK) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Failed to bind int parameter at index " +
                                   std::to_string(index));
        }
    }

    void bind_int64(int index, int64_t value) {
        validate_parameter_index(index);
        int rc = sqlite3_bind_int64(stmt_, index, value);
        if (rc != SQLITE_OK) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Failed to bind int64 parameter at index " +
                                   std::to_string(index));
        }
    }

    void bind_double(int index, double value) {
        validate_parameter_index(index);
        int rc = sqlite3_bind_double(stmt_, index, value);
        if (rc != SQLITE_OK) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Failed to bind double parameter at index " +
                                   std::to_string(index));
        }
    }

    void bind_text(int index, const std::string &text) {
        validate_parameter_index(index);
        int rc = sqlite3_bind_text(stmt_, index, text.c_str(),
                                   static_cast<int>(text.length()),
                                   SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Failed to bind text parameter at index " +
                                   std::to_string(index));
        }
    }

    void bind_text(int index, const char *text, int length = -1,
                   void (*destructor)(void *) = SQLITE_TRANSIENT) {
        validate_parameter_index(index);
        int rc = sqlite3_bind_text(stmt_, index, text, length, destructor);
        if (rc != SQLITE_OK) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Failed to bind text parameter at index " +
                                   std::to_string(index));
        }
    }

    void bind_blob(int index, const void *blob, int length) {
        validate_parameter_index(index);
        int rc =
            sqlite3_bind_blob(stmt_, index, blob, length, SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Failed to bind blob parameter at index " +
                                   std::to_string(index));
        }
    }

    void bind_null(int index) {
        validate_parameter_index(index);
        int rc = sqlite3_bind_null(stmt_, index);
        if (rc != SQLITE_OK) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Failed to bind null parameter at index " +
                                   std::to_string(index));
        }
    }

    void clear_bindings() { sqlite3_clear_bindings(stmt_); }

    int bind_parameter_count() { return sqlite3_bind_parameter_count(stmt_); }

   private:
    sqlite3_stmt *stmt_;

    void validate_parameter_index(int index) {
        if (index < 1) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Parameter index must be >= 1 (got " +
                                   std::to_string(index) + ")");
        }
        int param_count = sqlite3_bind_parameter_count(stmt_);
        if (index > param_count) {
            throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                               "Parameter index " + std::to_string(index) +
                                   " exceeds parameter count " +
                                   std::to_string(param_count));
        }
    }
};

#endif  // DFTRACER_UTILS_INDEXER_SQLITE_STATEMENT_H
