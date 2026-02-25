#ifndef DFTRACER_UTILS_CORE_SQLITE_STATEMENT_H
#define DFTRACER_UTILS_CORE_SQLITE_STATEMENT_H

#include <sqlite3.h>

#include <string>

namespace dftracer::utils::sqlite {

class SqliteDatabase;

class SqliteStmt {
   public:
    SqliteStmt(const SqliteDatabase &db, const char *sql);
    SqliteStmt(sqlite3 *db, const char *sql);
    ~SqliteStmt();

    SqliteStmt(const SqliteStmt &) = delete;
    SqliteStmt &operator=(const SqliteStmt &) = delete;

    operator sqlite3_stmt *();
    sqlite3_stmt *get();

    void reset();

    void bind_int(int index, int value);
    void bind_int64(int index, int64_t value);
    void bind_double(int index, double value);
    void bind_text(int index, const std::string &text);
    void bind_text(int index, const char *text, int length = -1,
                   void (*destructor)(void *) = SQLITE_TRANSIENT);
    void bind_blob(int index, const void *blob, int length);
    void bind_null(int index);

    void clear_bindings();
    int bind_parameter_count();

   private:
    sqlite3_stmt *stmt_;

    void validate_parameter_index(int index);
};

}  // namespace dftracer::utils::sqlite

#endif  // DFTRACER_UTILS_CORE_SQLITE_STATEMENT_H
