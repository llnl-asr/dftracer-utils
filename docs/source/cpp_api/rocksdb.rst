RocksDB
=======

The RocksDB layer provides the shared storage backend used by the
root-local ``.dftindex`` and provenance stores introduced by the
RocksDB migration.

It includes:

- database wrappers and lifecycle management
- column-family and merge-operator registration for the ``.dftindex`` schema
- key encoding helpers for typed prefix/range scans
- manager utilities for sharing open database handles across readers,
  indexers, and higher-level composites
- bulk-ingest helpers (``SstFileWriter`` + ``IngestExternalFile``) used by
  the distributed indexing pipeline

Architecture
------------

.. mermaid::

   graph TD
       Readers["TraceReader / utilities"] --> Manager["RocksDBManager"]
       Indexers["Indexer / provenance writers"] --> Manager
       Manager --> Database["RocksDatabase"]
       Database --> CFs["Column families"]
       Database --> Codec["KeyCodec"]
       Database --> Merge["MergeOperators (AGGREGATION, SYSTEM_METRICS)"]
       CFs --> Store[".dftindex / provenance store"]
       Codec --> Store

All types live in the ``dftracer::utils::rocksdb`` namespace; the column
family constants live in ``dftracer::utils::rocksdb::cf``.

RocksDatabase
-------------

*What*: a thin RAII wrapper around a ``rocksdb::DB`` opened with the
``.dftindex`` column families and DFTracer-specific ``Env`` / ``FileSystem``.
*When*: perform typed point reads/writes, range deletes, merges, batched
writes, iteration, and bulk SST ingest against one store. Move-only (copy
deleted). Every key/value/column-family argument is a ``std::string_view``;
the column family defaults to ``cf::DEFAULT``. Operations return a
``rocksdb::Status`` - check ``ok()``.

Prefer opening through :ref:`RocksDBManager <rocksdb-manager>` so a single
handle is shared process-wide rather than reopening the same path.

Key signatures:

.. code-block:: cpp

   explicit RocksDatabase(const std::string& db_path,
                          OpenMode open_mode = OpenMode::ReadWrite);
   bool open(const std::string& db_path, OpenMode open_mode = OpenMode::ReadWrite);
   void close();
   bool is_open() const noexcept;

   ::rocksdb::Status put(std::string_view key, std::string_view value,
                         std::string_view column_family = cf::DEFAULT);
   ::rocksdb::Status get(std::string_view key, std::string* value,
                         std::string_view column_family = cf::DEFAULT) const;
   ::rocksdb::Status del(std::string_view key,
                         std::string_view column_family = cf::DEFAULT);
   ::rocksdb::Status merge(std::string_view key, std::string_view value,
                           std::string_view column_family = cf::DEFAULT);

   Batch begin_batch() const;                    // Batch = rocksdb::WriteBatch
   ::rocksdb::Status commit_batch(Batch& batch);
   std::unique_ptr<::rocksdb::Iterator> new_iterator(
       std::string_view column_family = cf::DEFAULT) const;
   ::rocksdb::Status ingest_external_files(
       std::string_view column_family,
       const std::vector<std::string>& external_files, bool ingest_behind = false);

Point read/write:

.. code-block:: cpp

   #include <dftracer/utils/core/rocksdb/database.h>

   using namespace dftracer::utils::rocksdb;

   RocksDatabase db("/path/to/.dftindex");
   db.put("hello", "world", cf::METADATA);

   std::string value;
   if (db.get("hello", &value, cf::METADATA).ok()) {
       use(value);
   }

Batched writes (atomic commit):

.. code-block:: cpp

   RocksDatabase::Batch batch = db.begin_batch();
   db.put(batch, cf::METADATA, "a", "1");
   db.put(batch, cf::METADATA, "b", "2");
   db.commit_batch(batch);

Prefix scan with an iterator:

.. code-block:: cpp

   auto it = db.new_iterator(cf::CHUNK_STATS);
   for (it->Seek("prefix"); it->Valid(); it->Next()) {
       if (!it->key().starts_with("prefix")) break;
       consume(it->key(), it->value());
   }

.. _rocksdb-manager:

RocksDBManager
--------------

*What*: a process-wide singleton registry of open databases keyed by their
normalized ``.dftindex`` root path. *Why*: RocksDB allows only one writer
per path, and reopening is expensive; the manager owns one live instance per
path so short-lived wrappers (index reader, provenance writer, Python
bindings) share it. *When*: any time you need a handle - always go through
``get_or_open()`` instead of constructing ``RocksDatabase`` directly.

Key signatures:

.. code-block:: cpp

   static RocksDBManager& instance();
   std::shared_ptr<RocksDatabase> get_or_open(
       const std::string& db_path,
       RocksDatabase::OpenMode open_mode = RocksDatabase::OpenMode::ReadWrite,
       RocksDatabase::CfOptionsOverride cf_override = nullptr);
   void reset(const std::string& db_path);
   void shutdown();

Example:

.. code-block:: cpp

   #include <dftracer/utils/core/rocksdb/db_manager.h>

   using namespace dftracer::utils::rocksdb;

   std::shared_ptr<RocksDatabase> db =
       RocksDBManager::instance().get_or_open("/path/to/.dftindex");
   db->put("k", "v", cf::METADATA);

The optional ``cf_override`` is a
``std::function<void(const std::string&, rocksdb::ColumnFamilyOptions&)>``
used to attach per-column-family merge operators (for example the
AGGREGATION and SYSTEM_METRICS families) at open time.

Column families
---------------

*What*: ``cf`` holds the fixed set of column-family names for the
``.dftindex`` schema as ``constexpr std::string_view`` constants, plus
``cf::ALL`` (a ``std::array`` of every name, used to open the full set).
*When*: pass one of these constants as the ``column_family`` argument to any
``RocksDatabase`` call. Names include ``cf::DEFAULT``, ``cf::METADATA``,
``cf::CHUNK_STATS``, ``cf::CHUNK_BLOOM`` / ``cf::FILE_BLOOM``,
``cf::MANIFEST``, ``cf::PROVENANCE``, ``cf::AGGREGATION``, and
``cf::SYSTEM_METRICS`` (see the header for the complete list).

.. code-block:: cpp

   #include <dftracer/utils/core/rocksdb/column_families.h>

   using namespace dftracer::utils::rocksdb;

   db.merge("stats-key", encoded, cf::AGGREGATION);

KeyCodec and KeyBuilder
-----------------------

*What*: helpers for building the byte keys used across the schema.
``KeyCodec`` encodes/decodes fixed-width big-endian integers (BE ordering
keeps numeric keys lexicographically sortable, so typed prefix/range scans
work). ``KeyBuilder`` assembles a composite key from a tag, separators,
strings, and BE integers via a fluent API. *When*: construct or parse keys
for typed prefix/range scans instead of hand-formatting strings.

Key signatures:

.. code-block:: cpp

   // KeyCodec (all static)
   static std::string   encode_be32(std::uint32_t value);
   static std::string   encode_be64(std::uint64_t value);
   static std::uint32_t decode_be32(std::string_view bytes);
   static std::uint64_t decode_be64(std::string_view bytes);
   static void append_be32(std::string& out, std::uint32_t value);
   static void append_be64(std::string& out, std::uint64_t value);

   // KeyBuilder (fluent; returns *this)
   KeyBuilder& append_tag(std::string_view tag);
   KeyBuilder& append_separator();
   KeyBuilder& append_string(std::string_view value);
   KeyBuilder& append_be32(std::uint32_t value);
   KeyBuilder& append_be64(std::uint64_t value);
   std::string build() const;
   void clear();

Example (composite key ``chunk:<file_id BE32>:<offset BE64>``):

.. code-block:: cpp

   #include <dftracer/utils/core/rocksdb/key_codec.h>

   using namespace dftracer::utils::rocksdb;

   std::string key = KeyBuilder()
                         .append_tag("chunk")
                         .append_separator()
                         .append_be32(file_id)
                         .append_separator()
                         .append_be64(offset)
                         .build();
   db.put(key, value, cf::CHUNK_STATS);

   // Decode a raw BE64 key back to an integer:
   std::uint64_t n = KeyCodec::decode_be64(raw_bytes);

filesystem
----------

*What*: factory helpers ``make_dftracer_file_system()`` and
``make_dftracer_env()`` that build the RocksDB ``FileSystem`` / ``Env`` used
by ``RocksDatabase``. *When*: rarely called directly - ``RocksDatabase``
constructs these internally. Reach for them only to customize the I/O
backend before opening a database.

.. code-block:: cpp

   std::shared_ptr<::rocksdb::FileSystem> make_dftracer_file_system();
   std::unique_ptr<::rocksdb::Env> make_dftracer_env(
       const std::shared_ptr<::rocksdb::FileSystem>& file_system);

See also:

- :doc:`api/rocksdb`
- :doc:`indexer`
