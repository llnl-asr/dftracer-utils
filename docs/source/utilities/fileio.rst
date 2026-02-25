I/O Utilities
=============

File reading, writing, and streaming utilities.

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/file_reader.h>
   #include <dftracer/utils/utilities/fileio/streaming_file_reader.h>
   #include <dftracer/utils/utilities/fileio/streaming_file_writer.h>
   #include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>

Types
-----

.. code-block:: cpp

   // Binary data
   struct RawData {
       std::vector<unsigned char> data;
   };

   // Text content
   struct Text {
       std::string content;
       bool empty() const;
       std::size_t size() const;
   };

   // Line with position
   struct Line {
       std::string_view content;
       std::size_t line_number;  // 1-based
   };

FileReaderUtility
-----------------

Reads entire file into memory as text.

.. code-block:: cpp

   FileReaderUtility reader;
   Text content = reader.process(FileEntry{"/path/to/file.txt"});

BinaryFileReaderUtility
-----------------------

Reads entire file as binary data.

.. code-block:: cpp

   BinaryFileReaderUtility reader;
   RawData data = reader.process(FileEntry{"/path/to/file.bin"});

StreamingFileReaderUtility
--------------------------

Reads file in chunks with lazy evaluation.

**Input:**

.. code-block:: cpp

   struct StreamReadInput {
       fs::path path;
       std::size_t chunk_size = 64 * 1024;  // 64KB default
   };

**Example:**

.. code-block:: cpp

   StreamingFileReaderUtility reader;
   auto input = StreamReadInput{"/path/to/large_file.dat", 1024 * 1024};

   // Returns lazy iterator
   ChunkRange chunks = reader.process(input);

   for (const auto& chunk : chunks) {
       process(chunk);
   }

StreamingFileWriterUtility
--------------------------

Writes data in chunks.

.. code-block:: cpp

   StreamingFileWriterUtility writer("/output/file.dat");

   for (const auto& chunk : data_chunks) {
       writer.process(chunk);
   }

   writer.close();
   std::cout << "Wrote " << writer.total_bytes() << " bytes\n";

StreamingLineReader
-------------------

Lazy line-by-line reading for both plain and indexed files.

**Plain text files:**

.. code-block:: cpp

   // Read all lines
   LineRange lines = StreamingLineReader::read_plain("/path/to/file.txt");

   for (const auto& line : lines) {
       std::cout << line.line_number << ": " << line.content << "\n";
   }

   // Read specific line range
   LineRange subset = StreamingLineReader::read_plain(
       "/path/to/file.txt",
       100,   // start_line
       200    // end_line
   );

**Indexed (compressed) files:**

.. code-block:: cpp

   IndexedFileLineIteratorConfig config;
   config.archive_path = "/path/to/file.pfw.gz";
   config.index_path = "/path/to/file.pfw.gz.idx";
   config.start_line = 0;
   config.end_line = 1000;

   LineRange lines = StreamingLineReader::read_indexed(config);

   // Collect all into vector
   std::vector<Line> all_lines = lines.collect();

   // Or take first N
   std::vector<Line> first_100 = lines.take(100);

   // Or filter
   auto filtered = lines.filter([](const Line& l) {
       return l.content.find("error") != std::string_view::npos;
   });
