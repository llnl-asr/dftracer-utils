#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_SQLITE_DATABASE_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_SQLITE_DATABASE_H

#include <dftracer/utils/utilities/indexer/internal/error.h>
#include <sqlite3.h>

#include <string>

namespace dftracer::utils::utilities::indexer::internal {

class SqliteDatabase {
   public:
    SqliteDatabase() : db_path_(""), db_(nullptr) {}

    SqliteDatabase(const std::string &db_path)
        : db_path_(db_path), db_(nullptr) {
        open(db_path);
    }

    ~SqliteDatabase() { close(); }

    bool open(const std::string &db_path) {
        if (is_open()) {
            close();
        }

        db_path_ = db_path;
        if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
            throw IndexerError(
                IndexerError::Type::DATABASE_ERROR,
                "Failed to open database: " + std::string(sqlite3_errmsg(db_)));
        }
        return true;
    }

    void close() {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    sqlite3 *get() const { return db_; }

    bool is_open() const { return db_ != nullptr; }

   private:
    std::string db_path_;
    sqlite3 *db_;
};

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_SQLITE_DATABASE_H
