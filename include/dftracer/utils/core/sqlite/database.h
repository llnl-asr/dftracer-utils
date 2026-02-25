#ifndef DFTRACER_UTILS_CORE_SQLITE_DATABASE_H
#define DFTRACER_UTILS_CORE_SQLITE_DATABASE_H

#include <sqlite3.h>

#include <string>

namespace dftracer::utils::sqlite {

class SqliteDatabase {
   public:
    SqliteDatabase();
    explicit SqliteDatabase(const std::string &db_path);
    ~SqliteDatabase();

    SqliteDatabase(const SqliteDatabase &) = delete;
    SqliteDatabase &operator=(const SqliteDatabase &) = delete;

    SqliteDatabase(SqliteDatabase &&other) noexcept;
    SqliteDatabase &operator=(SqliteDatabase &&other) noexcept;

    bool open(const std::string &db_path);
    void close();
    bool open_with_vfs(const std::string &db_path, const char *vfs_name);

    sqlite3 *get() const;
    bool is_open() const;

   private:
    std::string db_path_;
    sqlite3 *db_;
};

}  // namespace dftracer::utils::sqlite

#endif  // DFTRACER_UTILS_CORE_SQLITE_DATABASE_H
