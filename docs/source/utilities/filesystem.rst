Filesystem
====================

Directory scanning and file enumeration utilities.

.. code-block:: cpp

   #include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>
   #include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

DirectoryScannerUtility
-----------------------

Scans directories and returns file entries.

**Input:**

.. code-block:: cpp

   struct DirectoryScannerUtilityInput {
       fs::path directory;
       bool recursive = false;

       static DirectoryScannerUtilityInput from_directory(const fs::path& dir);
       DirectoryScannerUtilityInput& with_recursive(bool value);
   };

**Output:**

.. code-block:: cpp

   struct FileEntry {
       fs::path path;
       std::size_t size;
       bool is_directory;
       bool is_regular_file;
   };

**Example:**

.. code-block:: cpp

   DirectoryScannerUtility scanner;
   auto input = DirectoryScannerUtilityInput::from_directory("/data/traces")
       .with_recursive(true);

   std::vector<FileEntry> files = scanner.process(input);

PatternDirectoryScannerUtility
------------------------------

Scans directories with pattern filtering.

**Input:**

.. code-block:: cpp

   struct PatternDirectoryScannerUtilityInput {
       fs::path directory;
       std::vector<std::string> patterns;  // e.g., {".pfw", ".pfw.gz"}
       bool recursive = false;

       static PatternDirectoryScannerUtilityInput from_directory(const fs::path& dir);
       PatternDirectoryScannerUtilityInput& with_patterns(std::vector<std::string> p);
       PatternDirectoryScannerUtilityInput& with_recursive(bool value);
   };

**Example:**

.. code-block:: cpp

   PatternDirectoryScannerUtility scanner;
   auto input = PatternDirectoryScannerUtilityInput::from_directory("/data")
       .with_patterns({".pfw", ".pfw.gz"})
       .with_recursive(true);

   std::vector<FileEntry> trace_files = scanner.process(input);
