Utilities
=========

dftracer-utils provides a collection of composable utilities for trace file processing. These utilities can be used standalone or combined into pipelines.

.. toctree::
   :maxdepth: 2
   :caption: Available Utilities:

   utilities/filesystem
   utilities/fileio
   utilities/compression
   utilities/text
   utilities/composites
   call-tree

Overview
--------

Utilities follow a consistent pattern:

- **Input types**: Configuration structs with fluent builder API
- **Output types**: Result structs with success status and data
- **process() method**: Main entry point that transforms input to output
- **Tags**: Metadata like ``Parallelizable`` for thread-safe utilities

Example usage:

.. code-block:: cpp

   #include <dftracer/utils/utilities/filesystem/directory_scanner.h>

   using namespace dftracer::utils::utilities::filesystem;

   DirectoryScannerUtility scanner;
   auto input = DirectoryScannerUtilityInput::from_directory("/data")
       .with_recursive(true);
   auto files = scanner.process(input);

   for (const auto& entry : files) {
       std::cout << entry.path << "\n";
   }
