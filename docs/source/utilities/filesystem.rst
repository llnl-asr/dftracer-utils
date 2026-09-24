:description: The coroutine directory-scanning utilities that discover trace files, fanning out concurrent child scans on parallel filesystems like Lustre.

Filesystem
====================

Directory scanning and file enumeration utilities.

.. code-block:: cpp

   #include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>
   #include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

Both scanners are coroutine utilities: they take a ``CoroScope&`` plus their
input and return a ``coro::CoroTask<std::vector<FileEntry>>``, or can be
called without a scope (they open their own on the current executor). A
recursive scan fans out one child scan per subdirectory over the scope, so
many directories are read concurrently on parallel filesystems such as
Lustre. Index-artifact directories (any name that starts with
``.dftindex``, such as ``.dftindex-cache`` and ``.dftindex_staging``) are never descended into during a recursive scan,
since they hold generated output, not source trace files.

DirectoryScannerUtility
-----------------------

Scans a directory (optionally recursively) and returns metadata about each
entry found.

**Input:**

.. code-block:: cpp

   struct DirectoryScannerUtilityInput {
       fs::path path;
       bool recursive = false;
       bool populate_size = true;  // also stats mtime; costs one extra stat() per file

       DirectoryScannerUtilityInput() = default;
       explicit DirectoryScannerUtilityInput(fs::path p, bool rec = false,
                                             bool with_size = true);
   };

**Output:**

.. code-block:: cpp

   struct FileEntry {
       fs::path path;
       std::size_t size = 0;
       std::uint64_t mtime = 0;  // Unix seconds; 0 when not populated
       bool is_directory = false;
       bool is_regular_file = false;
   };

**Example:**

.. code-block:: cpp

   #include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>

   using namespace dftracer::utils::utilities::filesystem;

   DirectoryScannerUtility scanner;
   DirectoryScannerUtilityInput input{"/data/traces", /*recursive=*/true};

   std::vector<FileEntry> files = co_await scanner(input);

PatternDirectoryScannerUtility
------------------------------

Wraps ``DirectoryScannerUtility`` and filters the results by
extension/suffix pattern.

**Input:**

.. code-block:: cpp

   struct PatternDirectoryScannerUtilityInput {
       std::string path;
       bool recursive = false;
       bool populate_size = true;
       std::vector<std::string> patterns;  // e.g. {".pfw.gz"}; empty = match all

       static PatternDirectoryScannerUtilityInput from_path(std::string p);
       PatternDirectoryScannerUtilityInput& with_patterns(std::vector<std::string> p);
       PatternDirectoryScannerUtilityInput& with_recursive(bool value);
   };

A pattern starting with ``.`` matches as an extension/suffix (``.pfw.gz``
matches ``file.pfw.gz``); a pattern of the form ``*.ext`` matches the same
way; anything else must match the filename exactly.

**Example:**

.. code-block:: cpp

   #include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

   using namespace dftracer::utils::utilities::filesystem;

   PatternDirectoryScannerUtility scanner;
   auto input = PatternDirectoryScannerUtilityInput::from_path("/data")
       .with_patterns({".pfw.gz"})
       .with_recursive(true);

   std::vector<FileEntry> trace_files = co_await scanner(input);
