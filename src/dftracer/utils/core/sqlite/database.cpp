#include <dftracer/utils/core/sqlite/database.h>
#include <dftracer/utils/core/sqlite/error.h>

#include <utility>

namespace dftracer::utils::sqlite {

SqliteDatabase::SqliteDatabase() : db_path_(""), db_(nullptr) {}

SqliteDatabase::SqliteDatabase(const std::string &db_path)
    : db_path_(db_path), db_(nullptr) {
    open(db_path);
}

SqliteDatabase::~SqliteDatabase() { close(); }

SqliteDatabase::SqliteDatabase(SqliteDatabase &&other) noexcept
    : db_path_(std::move(other.db_path_)), db_(other.db_) {
    other.db_ = nullptr;
}

SqliteDatabase &SqliteDatabase::operator=(SqliteDatabase &&other) noexcept {
    if (this != &other) {
        close();
        db_path_ = std::move(other.db_path_);
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

bool SqliteDatabase::open(const std::string &db_path) {
    if (is_open()) {
        close();
    }

    db_path_ = db_path;
    if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
        throw SqliteError(
            SqliteError::Type::OPEN_ERROR,
            "Failed to open database: " + std::string(sqlite3_errmsg(db_)));
    }
    return true;
}

void SqliteDatabase::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

sqlite3 *SqliteDatabase::get() const { return db_; }

bool SqliteDatabase::is_open() const { return db_ != nullptr; }

bool SqliteDatabase::open_with_vfs(const std::string &db_path,
                                   const char *vfs_name) {
    if (is_open()) {
        close();
    }
    db_path_ = db_path;
    int rc =
        sqlite3_open_v2(db_path_.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs_name);
    if (rc != SQLITE_OK) {
        throw SqliteError(SqliteError::Type::OPEN_ERROR,
                          "Failed to open database with VFS '" +
                              std::string(vfs_name) +
                              "': " + std::string(sqlite3_errmsg(db_)));
    }
    return true;
}

}  // namespace dftracer::utils::sqlite
