Composite Utilities
===================

High-level utilities that combine multiple operations into complete workflows.

.. code-block:: cpp

   #include <dftracer/utils/utilities/composites/composites.h>

   using namespace dftracer::utils::utilities::composites;

BatchProcessorUtility
---------------------

Processes multiple items in parallel with optional result sorting.

.. code-block:: cpp

   template <typename Input, typename Output>
   class BatchProcessorUtility;

**Example:**

.. code-block:: cpp

   // Create workflow utility
   auto workflow = std::make_shared<MyItemProcessor>();

   // Wrap in batch processor
   auto batch = std::make_shared<BatchProcessorUtility<ItemInput, ItemOutput>>(workflow);

   // Optional: sort results by index
   batch->with_comparator([](const ItemOutput& a, const ItemOutput& b) {
       return a.index < b.index;
   });

   // Process items
   std::vector<ItemInput> inputs = /* ... */;
   std::vector<ItemOutput> outputs = batch->process(inputs);

DirectoryFileProcessorUtility
-----------------------------

Scans directory and processes each matching file in parallel.

.. code-block:: cpp

   template <typename FileOutput>
   class DirectoryFileProcessorUtility;

**Input:**

.. code-block:: cpp

   struct DirectoryProcessInput {
       fs::path directory;
       std::vector<std::string> extensions;  // e.g., {".pfw", ".gz"}
       bool recursive = false;

       static DirectoryProcessInput from_directory(const fs::path& dir);
       DirectoryProcessInput& with_extensions(std::vector<std::string> exts);
       DirectoryProcessInput& with_recursive(bool value);
   };

**Output:**

.. code-block:: cpp

   template <typename T>
   struct BatchFileProcessOutput {
       std::vector<T> results;
       std::size_t total_files;
       std::size_t successful_files;
   };

LineBatchProcessorUtility
-------------------------

Streaming line processing with optional filtering.

.. code-block:: cpp

   template <typename LineOutput>
   class LineBatchProcessorUtility;

Processes lines lazily and returns vector of results. Line processor returns ``std::optional<LineOutput>`` to filter lines.

**Example:**

.. code-block:: cpp

   auto processor = std::make_shared<LineBatchProcessorUtility<ParsedEvent>>(
       [](const Line& line) -> std::optional<ParsedEvent> {
           if (line.content.empty()) return std::nullopt;
           return parse_event(line);
       }
   );

   LineReadInput input{
       "/path/to/file.pfw.gz",
       "/path/to/file.pfw.gz.idx",
       0,     // start_line
       1000   // end_line
   };

   std::vector<ParsedEvent> events = processor->process(input);

FileCompressorUtility
---------------------

Compresses files with streaming.

**Input:**

.. code-block:: cpp

   struct FileCompressionUtilityInput {
       fs::path input_path;
       fs::path output_path;
       int compression_level = 6;

       static FileCompressionUtilityInput from_file(const fs::path& path);
       FileCompressionUtilityInput& with_output(const fs::path& path);
       FileCompressionUtilityInput& with_level(int level);
   };

**Output:**

.. code-block:: cpp

   struct FileCompressionUtilityOutput {
       bool success;
       std::size_t original_size;
       std::size_t compressed_size;
       double compression_ratio;
   };

FileDecompressorUtility
-----------------------

Decompresses files with streaming.

**Input:**

.. code-block:: cpp

   struct FileDecompressionUtilityInput {
       fs::path input_path;
       fs::path output_path;

       static FileDecompressionUtilityInput from_file(const fs::path& path);
       FileDecompressionUtilityInput& with_output(const fs::path& path);
   };

IndexedFileReaderUtility
------------------------

Creates readers for indexed compressed files, handling index creation.

.. code-block:: cpp

   struct IndexedReadInput {
       fs::path file_path;
       fs::path index_path;
       std::size_t checkpoint_size = DEFAULT_CHECKPOINT_SIZE;
       bool force_rebuild = false;

       static IndexedReadInput from_file(const fs::path& path);
       IndexedReadInput& with_index(const fs::path& path);
       IndexedReadInput& with_checkpoint_size(std::size_t size);
       IndexedReadInput& with_force_rebuild(bool value);
   };

**Example:**

.. code-block:: cpp

   IndexedFileReaderUtility utility;

   auto input = IndexedReadInput::from_file("/data/trace.pfw.gz")
       .with_index("/data/trace.pfw.gz.idx")
       .with_force_rebuild(false);

   std::shared_ptr<Reader> reader = utility.process(input);
