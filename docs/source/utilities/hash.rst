Hash
==============

Incremental, streaming hash computation with support for multiple algorithms and thread-safe variants.

.. code-block:: cpp

   #include <dftracer/utils/utilities/hash/hasher_utility.h>
   #include <dftracer/utils/utilities/hash/mt_hasher_utility.h>

Types
-----

.. code-block:: cpp

   enum class HashAlgorithm {
       FNV1A_64,  // FNV-1a 64-bit (streaming, deterministic)
       STD        // std::hash (platform-dependent, non-streaming)
   };

   struct Hash {
       std::size_t value = 0;
       bool operator==(const Hash& other) const;
       bool operator!=(const Hash& other) const;
   };

HasherUtility
-------------

Unified hash utility that can switch between algorithms at runtime. Default is FNV-1a.

**Basic hashing:**

.. code-block:: cpp

   #include <dftracer/utils/utilities/hash/hasher_utility.h>

   HasherUtility hasher;
   hasher.reset();
   hasher.update("Hello");
   hasher.update("World");
   Hash h = hasher.get_hash();

**Algorithm selection:**

.. code-block:: cpp

   HasherUtility hasher(HashAlgorithm::STD);
   hasher.reset();
   hasher.update("data");
   Hash h = hasher.get_hash();

   // Switch at runtime
   hasher.set_algorithm(HashAlgorithm::FNV1A_64);
   hasher.reset();

**Hashing POD types and binary data:**

.. code-block:: cpp

   HasherUtility hasher;
   hasher.reset();

   hasher.update("name");          // String
   int id = 42;
   hasher.update(id);              // POD type (hashes raw bytes)

   std::vector<unsigned char> bin = {0xFF, 0xFE};
   hasher.update(bin);             // Binary data

   Hash combined = hasher.get_hash();

**Variadic processing:**

.. code-block:: cpp

   HasherUtility hasher;
   hasher.reset();
   Hash h = hasher.process(1, 2, 3);
   Hash h2 = hasher.process("hello", 42, 3.14);

**Reusing in hot loops:**

.. code-block:: cpp

   // Per project conventions: reuse a single instance with reset()
   HasherUtility hasher;

   for (const auto& item : items) {
       hasher.reset();
       hasher.update(item.data);
       Hash h = hasher.get_hash();
       process_hash(h);
   }

**Pipeline integration (coroutine):**

.. code-block:: cpp

   HasherUtility hasher;
   Hash h = co_await hasher.process("data");

MTHasherUtility
---------------

Thread-safe wrapper around ``HasherUtility``. All operations protected by ``std::mutex``.

.. code-block:: cpp

   #include <dftracer/utils/utilities/hash/mt_hasher_utility.h>

   auto hasher = std::make_shared<MTHasherUtility>();
   hasher->reset();

   // Safe to call from multiple threads
   std::thread t1([&]() { hasher->update("data1"); });
   std::thread t2([&]() { hasher->update("data2"); });
   t1.join();
   t2.join();

   Hash result = hasher->get_hash();

.. note::

   While ``MTHasherUtility`` is thread-safe, you must coordinate the order of updates across threads if you want deterministic results.

Algorithm Comparison
--------------------

.. list-table::
   :header-rows: 1

   * - Feature
     - FNV1A_64
     - STD
   * - Streaming
     - Yes
     - No
   * - ``update("A"); update("B")``
     - = ``update("AB")``
     - != ``update("AB")``
   * - Deterministic
     - Cross-platform
     - Platform-dependent
   * - Use case
     - Default, most cases
     - Compatibility with std::hash

See Also
--------

- :doc:`/cpp_api/index` - Full C++ API documentation
- :doc:`composites` - Composites that use hashing for event verification
