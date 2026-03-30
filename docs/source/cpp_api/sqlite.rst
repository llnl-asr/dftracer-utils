Async SQLite API
================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/sqlite>`.


Asynchronous SQLite database operations integrated with the dftracer executor and coroutine system. All classes and functions are in the ``dftracer::utils::sqlite`` namespace.

Overview
--------

The async SQLite module provides a thin coroutine-aware wrapper around SQLite3 that allows database operations to be performed asynchronously without blocking the executor. Operations are submitted to a dedicated SQLite thread pool and the coroutine is suspended until completion.

Key Features:

- **Async execution**: Database operations submit work to a thread pool and suspend via ``co_await``
- **Sync fallback**: When no executor is active, operations run synchronously inline
- **VFS integration**: Custom SQLite VFS implementation uses async I/O backend for file operations
- **Minimal overhead**: Thin wrapper on top of SQLite3; no ORM abstractions
- **Thread-safe**: All database access is serialized through the thread pool

.. mermaid::

   sequenceDiagram
       participant Task as CoroTask
       participant Await as SqliteAwaitable
       participant Pool as SQLite ThreadPool
       participant DB as sqlite3

       Task->>Await: co_await sqlite::run(fn)
       Await->>Pool: submit work
       Note over Task: suspended
       Pool->>DB: execute SQL
       DB-->>Pool: result
       Pool-->>Await: complete
       Await-->>Task: resume with result

Database Management
-------------------

The ``SqliteDatabase`` class wraps a SQLite connection:

Opening a Database
~~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   #include <dftracer/utils/core/sqlite/database.h>

   // Create and open an in-memory database
   sqlite::SqliteDatabase db;
   db.open(":memory:");

   // Or open a file-backed database
   sqlite::SqliteDatabase db("path/to/db.sqlite");

   // Check if open
   if (db.is_open()) {
       // Database is ready
   }

   // Get the raw sqlite3* handle for advanced SQLite API
   sqlite3 *raw_db = db.get();

Custom VFS
~~~~~~~~~~

For databases that should use the async I/O backend:

.. code-block:: cpp

   // Register the dftracer async I/O VFS
   // This is typically done once at application startup
   sqlite::register_dftracer_sqlite_vfs(io_backend, executor);

   // Then open with the custom VFS
   sqlite::SqliteDatabase db;
   db.open_with_vfs("trace.db", "dftracer");

   // Later, unregister when shutting down
   sqlite::unregister_dftracer_sqlite_vfs();

VFS Implementation Details
~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``DfTracerSqliteVfs`` is a custom SQLite Virtual File System that:

- Replaces SQLite's default file I/O with async operations from the I/O backend
- Handles WAL mode, synchronization, and shared memory regions
- Integrates with the Executor to resume coroutines on completion

Prepared Statements
-------------------

The ``SqliteStmt`` class wraps a compiled SQL statement:

Binding Parameters
~~~~~~~~~~~~~~~~~~~

SQLite uses placeholders (``?``, ``?1``, ``:name``) in SQL. Bind values before execution:

.. code-block:: cpp

   #include <dftracer/utils/core/sqlite/statement.h>

   sqlite::SqliteStmt stmt(db, "INSERT INTO logs (id, message, level) VALUES (?, ?, ?)");

   stmt.bind_int(1, 42);
   stmt.bind_text(2, "Connection opened");
   stmt.bind_int(3, INFO_LEVEL);

   // Execute and handle result...

Binding Functions
~~~~~~~~~~~~~~~~~

Statement Execution (Async)
---------------------------

Use the ``SqliteAwaitable<T>`` template to execute arbitrary database operations asynchronously:

The Generic ``run()`` Function
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For simple async database work that doesn't require a ``SqliteDatabase`` object:

Example: Async Query
^^^^^^^^^^^^^^^^^^^^

.. code-block:: cpp

   #include <dftracer/utils/core/sqlite/async.h>
   #include <dftracer/utils/core/sqlite/database.h>
   #include <dftracer/utils/core/sqlite/statement.h>
   #include <dftracer/utils/core/coro/task.h>
   #include <iostream>

   using namespace dftracer::utils;

   CoroTask<void> count_events(sqlite::SqliteDatabase& db) {
       // Define work to run on the sqlite thread pool
       auto result = co_await sqlite::run([&db]() {
           sqlite::SqliteStmt stmt(db, "SELECT COUNT(*) FROM events");
           // Use SQLite C API directly
           sqlite3 *raw_db = db.get();
           sqlite3_stmt *raw_stmt = stmt.get();

           int count = 0;
           if (sqlite3_step(raw_stmt) == SQLITE_ROW) {
               count = sqlite3_column_int(raw_stmt, 0);
           }
           return count;
       });

       std::cout << "Total events: " << result << std::endl;
   }

Async Submission Helpers
~~~~~~~~~~~~~~~~~~~~~~~~

Low-level helpers for integrating with the executor and thread pool:

Error Handling
--------------

The ``SqliteError`` exception class represents database errors:

Error Types
~~~~~~~~~~~

Errors are categorized into types:

- ``DATABASE_ERROR``: SQLite runtime error (e.g., constraint violation, locked database)
- ``STATEMENT_ERROR``: Prepared statement compilation or execution error
- ``OPEN_ERROR``: Database open failure
- ``VFS_ERROR``: VFS registration or I/O error
- ``UNKNOWN_ERROR``: Unexpected error condition

Example: Error Handling
^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: cpp

   #include <dftracer/utils/core/sqlite/error.h>
   #include <dftracer/utils/core/sqlite/database.h>

   try {
       sqlite::SqliteDatabase db("data.db");
       // ... database operations ...
   } catch (const sqlite::SqliteError& e) {
       if (e.type() == sqlite::SqliteError::OPEN_ERROR) {
           std::cerr << "Cannot open database: " << e.what() << std::endl;
       } else {
           std::cerr << "Database error: " << e.what() << std::endl;
       }
   }

Complete Async Example
----------------------

A complete example showing async database initialization, insertion, and querying:

.. code-block:: cpp

   #include <dftracer/utils/core/sqlite/async.h>
   #include <dftracer/utils/core/sqlite/database.h>
   #include <dftracer/utils/core/sqlite/statement.h>
   #include <dftracer/utils/core/coro/task.h>
   #include <dftracer/utils/core/pipeline/pipeline.h>
   #include <dftracer/utils/core/pipeline/pipeline_config.h>
   #include <iostream>

   using namespace dftracer::utils;

   // Initialize database schema async
   CoroTask<void> init_db(sqlite::SqliteDatabase& db) {
       co_await sqlite::run([&db]() {
           sqlite3_exec(db.get(),
               "CREATE TABLE IF NOT EXISTS logs ("
               "  id INTEGER PRIMARY KEY,"
               "  timestamp INTEGER,"
               "  message TEXT"
               ");",
               nullptr, nullptr, nullptr);
           return true;
       });
       std::cout << "Database initialized" << std::endl;
   }

   // Insert a log entry async
   CoroTask<void> insert_log(sqlite::SqliteDatabase& db, int id,
                             long ts, const std::string& msg) {
       co_await sqlite::run([&db, id, ts, &msg]() {
           sqlite::SqliteStmt stmt(db, "INSERT INTO logs VALUES (?, ?, ?)");
           stmt.bind_int(1, id);
           stmt.bind_int64(2, ts);
           stmt.bind_text(3, msg);

           sqlite3_step(stmt.get());
           return true;
       });
       std::cout << "Inserted log entry " << id << std::endl;
   }

   // Query logs async
   CoroTask<void> query_logs(sqlite::SqliteDatabase& db) {
       auto count = co_await sqlite::run([&db]() {
           sqlite::SqliteStmt stmt(db, "SELECT COUNT(*) FROM logs");
           sqlite3_step(stmt.get());
           return sqlite3_column_int(stmt.get(), 0);
       });
       std::cout << "Database has " << count << " log entries" << std::endl;
   }

   // Main coroutine
   CoroTask<void> main_app() {
       sqlite::SqliteDatabase db;
       db.open(":memory:");

       co_await init_db(db);
       co_await insert_log(db, 1, 1000, "First event");
       co_await insert_log(db, 2, 2000, "Second event");
       co_await query_logs(db);
   }

   int main() {
       auto config = PipelineConfig()
           .with_name("SqliteExample")
           .with_compute_threads(1);

       auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
           co_await main_app();
       }, "MainApp");

       Pipeline pipeline(config);
       pipeline.set_source({task});
       pipeline.execute();
       return 0;
   }

Raw SQLite API Access
---------------------

For advanced use cases, you can access the underlying SQLite C API directly:

.. code-block:: cpp

   #include <sqlite3.h>
   #include <dftracer/utils/core/sqlite/database.h>

   sqlite::SqliteDatabase db("app.db");

   // Get raw sqlite3* handle
   sqlite3 *raw_db = db.get();

   // Use any SQLite C API function
   const char *sql = "SELECT * FROM users WHERE id = ?";
   sqlite3_stmt *stmt = nullptr;
   sqlite3_prepare_v2(raw_db, sql, -1, &stmt, nullptr);

   // ... bind parameters and execute ...
   sqlite3_finalize(stmt);

Sync Operations Outside Executor
---------------------------------

``SqliteDatabase`` can be used outside the coroutine executor for synchronous
operations. When ``SqliteAwaitable`` detects no executor thread pool
(``pool_ == nullptr``), it executes the operation inline in ``await_ready()``
without suspending the coroutine.

For fully synchronous usage (no executor at all), use ``SqliteDatabase``
directly with the raw SQLite C API:

.. code-block:: cpp

   #include <dftracer/utils/core/sqlite/database.h>

   sqlite::SqliteDatabase db("data.db");

   // Use sqlite3 C API directly
   sqlite3 *raw = db.get();

   sqlite3_exec(raw, "CREATE TABLE IF NOT EXISTS kv (k TEXT, v TEXT)",
                nullptr, nullptr, nullptr);

   sqlite3_stmt *stmt = nullptr;
   sqlite3_prepare_v2(raw, "INSERT INTO kv VALUES (?, ?)", -1, &stmt, nullptr);
   sqlite3_bind_text(stmt, 1, "key", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, "value", -1, SQLITE_STATIC);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   db.close();
