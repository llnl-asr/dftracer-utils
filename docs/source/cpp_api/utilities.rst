Utilities API
=============

Composable processing utilities. For usage examples, see :doc:`/utilities`.

Call Tree
---------

Build hierarchical call trees from DFTracer traces. See :doc:`/call-tree` for usage guide.

.. doxygenclass:: dftracer::utils::call_tree::CallTree
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::call_tree::CallTreeNodeInfo
   :project: dftracer-utils
   :members:

.. doxygenstruct:: dftracer::utils::call_tree::CallTreeStats
   :project: dftracer-utils
   :members:

Filesystem
----------

Directory scanning utilities.

.. doxygenclass:: dftracer::utils::utilities::filesystem::DirectoryScannerUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::filesystem::PatternDirectoryScannerUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

I/O
---

File reading and writing utilities.

.. doxygenclass:: dftracer::utils::utilities::io::FileReaderUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::io::BinaryFileReaderUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::io::StreamingFileReaderUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::io::StreamingFileWriterUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::io::lines::StreamingLineReader
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::io::lines::LineRange
   :project: dftracer-utils
   :members:
   :undoc-members:

Compression
-----------

Zlib compression utilities supporting GZIP, ZLIB, DEFLATE formats.

.. doxygenclass:: dftracer::utils::utilities::compression::zlib::CompressorUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::compression::zlib::DecompressorUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::compression::zlib::StreamingCompressorUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::compression::zlib::StreamingDecompressorUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

Text
----

Text processing utilities.

.. doxygenclass:: dftracer::utils::utilities::text::LineSplitterUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::text::LineFilterUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::text::MultiLinesFilterUtility
   :project: dftracer-utils
   :members:
   :undoc-members:
