:description: Keep your own data in the key-value store, from C++, C or Python, and pack its files.

Store your own data in the key-value store
==========================================

.. admonition:: Goal
   :class: goal

   Write keys and values with atomic batches, read them from point-in-time snapshots and merge small files when a directory grows.

The store that holds the index is also a public library.
Use ``dftracer/utils/store/store.h`` from C++, ``dftracer/utils/store/abi.h`` from C and ``dftracer.utils.Store`` from Python.

Write and read
--------------

.. code-block:: python

   from dftracer.utils import Store

   store = Store.open("/tmp/my-store")
   ns = store.namespace("my.app.artifacts", raw=True)

   with store.batch(ns, "/data/run1") as batch:
       batch.put(b"a", b"1")
       batch.put(b"b", b"2")
   store.flush()

   view = store.snapshot()
   assert view.get(ns, "/data/run1", b"a") == b"1"

A namespace is a named group of data.
With ``raw=True`` it keeps bytes as they are, the way C++ and C see them.
The next section shows the default mode, which keeps Python objects.
A scope inside it is one file path, a sequence of paths or ``None`` for the root.
A batch is atomic for one namespace and one scope.
The ``with`` block commits on a clean exit and discards the batch on an exception.
A reader sees a batch after ``flush``, ``close`` or a full extent.
A reader in another process looks for new data at most once per second.
Call ``refresh`` before ``snapshot`` to look at once.

The same in C++, inside a coroutine:

.. code-block:: cpp

   auto store = store::Store::open("/tmp/my-store");
   auto ns = store->ns(store::NamespaceSpec{.name = "my.app.artifacts"});
   const auto scope = store::ScopeKey::file("/data/run1");

   store::Batch batch = store->batch(*ns, scope);
   batch.put(key, value);
   auto committed = co_await store->commit(std::move(batch));

Erase keys
----------

.. code-block:: python

   with store.batch(ns, "/data/run1") as batch:
       batch.erase(b"a")
       batch.erase_range(b"m", b"p")
       batch.put(b"a", b"new")

The erases of a batch apply before its puts, whatever the call order, so ``a`` above holds ``new``.
A reader sees the erases and the puts together.
An erase needs the copy that this process wrote.
When another data file holds the current copy, the commit fails with ``Unsupported``.

Each process that writes makes its own copy of a scope, and the newest copy is the one readers see.
So the first batch a new writer commits to a scope replaces what that scope held.

To let several processes add keys to one scope, make the namespace shared.
Each scope then holds the keys of every writer, and the newest copy wins for a key that several wrote.

.. code-block:: python

   artifacts = store.namespace("artifacts", shared=True)

A shared namespace has no fold and refuses erases with ``Unsupported``.
``drop`` still clears the whole scope, and ``pack`` merges the copies of a scope into one.

Store typed keys and values
---------------------------

A namespace encodes each key and value and decodes them on read.
Pass ``None``, ``bool``, ``int``, ``float``, ``str``, ``bytes``, ``tuple``, ``list`` or ``dict``.
A ``bytearray`` or a ``memoryview`` becomes ``bytes`` after a round trip.
The store checks the exact type.
A subclass of these types, such as an ``IntEnum``, a ``namedtuple`` or ``class Name(str)``, raises ``TypeError`` until you register that subclass.
Any other type raises ``TypeError`` too.
An ``int`` can have up to 255 bytes.
Nesting of ``tuple``, ``list``, ``dict`` and registered values can be 64 levels deep, and a deeper value raises ``ValueError``.

.. code-block:: python

   ns = store.namespace("my.app.runs")
   with store.batch(ns, None) as batch:
       batch.put(("run", 1), {"loss": 0.5})
       batch.put(("run", 2), {"loss": 0.25})
       batch.put(("other", 1), None)
   store.flush()

   view = store.snapshot()
   assert view.get(ns, None, ("run", 2)) == {"loss": 0.25}
   assert [key for key, _ in view.scan(ns, None, ("run",))] == [("run", 1), ("run", 2)]

Type checkers see the key and value types of a namespace.
Annotate the variable, and the checker knows ``put``, ``get`` and ``scan``:

.. code-block:: python

   from typing import Dict, Tuple
   from dftracer.utils.store import Namespace

   runs: Namespace[Tuple[str, int], Dict[str, float]] = store.namespace("my.app.runs")

This form makes no check at run time.
To check at run time, pass both ``key_type`` and ``value_type``.
Each key and value that goes in or comes out must be an instance of them, or the call raises ``TypeError``.
This includes stored data that another program wrote.
Passing only one of the two raises ``TypeError``.

.. code-block:: python

   scores = store.namespace("my.app.scores", key_type=str, value_type=float)

A raw namespace is a ``RawNamespace``.
Only its batch, a ``RawBatch``, has ``fold``.

A ``scan`` prefix is a ``str``, ``bytes`` or ``tuple``.
It matches each longer value that starts the same.
A tuple prefix matches whole elements, so ``("a",)`` does not match the key ``("a\x00",)``.
``None`` means all keys.

Let a class store itself
~~~~~~~~~~~~~~~~~~~~~~~~

A class that defines ``store_type_id``, ``to_store`` and ``from_store`` in its own body works as a key or a value.
``to_store`` returns any value that the store can encode.
``from_store`` is a classmethod or a staticmethod that returns the object.
The store finds the three members by name and needs no registration to write.

.. code-block:: python

   class RunKey:
       store_type_id = 1

       def __init__(self, run, step):
           self.run = run
           self.step = step

       def to_store(self):
           return (self.run, self.step)

       @classmethod
       def from_store(cls, value):
           return cls(*value)

   with store.batch(ns, None) as batch:
       batch.put(RunKey("a", 3), 0.5)

The ``store_type_id`` is kept in the data, so never change it.
Keep it different for each class.
A subclass needs its own ``store_type_id``, and without one ``encode`` raises ``TypeError``.
The store never encodes a subclass as its parent.
Keys sort by the value that ``to_store`` returns.

A process that only reads does not know the id yet.
Apply ``register_type`` to the class, as a decorator or as a call, before the first read.
Stored data never names a class to import, and nothing is unpickled.
A read of an unknown id raises ``ValueError``.

.. code-block:: python

   from dftracer.utils.store import register_type

   @register_type
   class RunKey:
       ...

Register a class you cannot edit
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Give the id and two functions.
Registering the same class again with the same id and functions does nothing.
The same id on another class raises ``ValueError``.

.. code-block:: python

   from datetime import datetime

   register_type(
       datetime,
       2,
       lambda d: d.timestamp(),
       datetime.fromtimestamp,
   )

Store raw bytes for C++ and C
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Pass ``raw=True`` for data that C++ or the C ABI reads or writes.
Keys and values must be ``bytes``, ``bytearray`` or ``memoryview``.
The store keeps them as they are and does not copy them again, and reads return ``bytes``.
Any other type raises ``TypeError``.
A namespace with a fold is always raw, because fold inputs are fixed binary values.

.. code-block:: python

   raw = store.namespace("my.app.raw", raw=True)
   with store.batch(raw, None) as batch:
       batch.put(b"k", b"v")

``encode``, ``decode`` and ``encode_prefix`` are public in ``dftracer.utils.store``.
Use them on raw bytes that came from a default namespace.
In a raw namespace, filter the keys of a tuple prefix with ``matches_prefix``, because the bytes of ``encode_prefix`` alone also match some longer elements.
A read of raw bytes in a default namespace raises ``ValueError`` that names ``raw=True``.

Know these limits:

- This is Python only.
  A C++ or C reader sees the raw encoded bytes.
- Values of one type sort like their encodings.
  Values of different types sort by type tag.
  So ``1`` and ``1.5`` do not compare, and a scan puts all ints before all floats.
  Do not mix types in the keys of a namespace when order matters.
- ``raw`` is not stored on disk.
  Each process that uses a namespace passes the same value.
- A ``dict`` has stable bytes, because its entries are sorted by encoded key.
  That order has no useful meaning.
  Do not use a ``dict`` as a key when you scan.

Use the store after a fork
--------------------------

A handle that a fork child inherited fails each call with ``ForkedHandle``.
The child opens its own handle.
Dropping the inherited handle is safe, because it waits for nothing and closes nothing.

.. code-block:: python

   pid = os.fork()
   if pid == 0:
       store = Store.open("/tmp/my-store")

Do not fork while another thread is inside ``Store.open``.

Merge small files
-----------------

Each writer that closes leaves one data file.
``pack`` merges the small ones, and nothing runs it for you.

.. code-block:: python

   report = store.pack()
   print(report.files_packed, report.files_deleted)

Run ``pack`` with no other writer active in the directory.
It works in three steps:

#. It closes this process's writer and rewrites the current copies of the chosen files into one new file.
#. It makes those copies current in one record, so a reader sees all of them or none.
#. It deletes each closed file that has no current data, once the files that replaced it are older than ``keep_seconds``.

A file is chosen when it is below ``small_file_bytes`` and at least one other file is too, or when ``dead_ratio`` of its segment bytes belong to old copies.
A file without a footer is never rewritten and never counts as a replacement, because a crash may have cost it commits.
A failed ``pack`` changes nothing that a reader sees.

The default ``keep_seconds`` is one hour, so a run usually deletes the files that an earlier run replaced.
A snapshot older than a delete gets an I/O error from a file it had not opened yet, and never wrong data.

File count costs little while the files are local.
On a laptop SSD, with the operating system cache warm, opening a directory and reading one key took 3.3 ms with 1000 files and 11.7 ms with 4000 files.
Measure on your own file system before you choose how often to run ``pack``.
