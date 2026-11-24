:description: Read a large JSON lines or YAML file in batches, edit a record, write records, and convert to plain Python with dftracer.utils.formats.

Read and write JSON and YAML
============================

.. admonition:: Goal
   :class: goal

   Read a large JSON lines or YAML file in bounded memory, change a record,
   write records to a new file and get plain Python data when you need it.

``dftracer.utils.formats`` has two modules, ``formats.json`` and
``formats.yaml``. They have the same names, so the steps below work for both.
The modules are named like the standard ones, so import the package and use
``formats.json``:

.. code-block:: python

   from dftracer.utils import formats

   value = formats.json.parse('{"name": "read", "dur": 5}')
   print(value["name"].as_str(), value["dur"].as_int())

Set up the examples
-------------------

The next steps use a small file. This code writes 1000 records to
``events.jsonl.gz``. The ``.gz`` suffix makes the writer pick gzip.

.. code-block:: python

   from dftracer.utils import formats

   with formats.json.Writer("events.jsonl.gz") as w:
       for i in range(1000):
           w.write({"id": i, "name": "read" if i % 2 else "write", "dur": i * 3})

Read a large file in batches
----------------------------

``read_records`` gives one batch at a time. A JSON record is one line. A batch
holds about ``batch_bytes`` of memory (the default is 64 MiB), including its
parsed form. So the memory does not grow with the size of the file.

.. code-block:: python

   from dftracer.utils import formats

   total = 0
   for batch in formats.json.read_records("events.jsonl.gz", batch_bytes=1 << 20):
       for record in batch:
           if record["name"].as_str() == "read":
               total += record["dur"].as_int()
   print(total)

A ``Value`` is a view of the parsed text. It copies nothing. It keeps its whole
batch alive, so do not collect ``Value`` objects in a list. Keep plain data
instead:

.. code-block:: python

   slow = []
   for batch in formats.json.read_records("events.jsonl.gz"):
       for record in batch:
           if record["dur"].as_int() > 2900:
               slow.append(record.to_python())

The same loop reads YAML. A YAML record is an element of a top-level block
sequence or a document of a ``---`` stream. A YAML scalar is text, so
``as_int`` parses the text and raises ``ValueError`` when it is not a number:

.. code-block:: python

   from dftracer.utils import formats

   with formats.yaml.Writer("jobs.yaml") as w:
       for i in range(3):
           w.write({"job": f"j{i}", "nodes": i + 1})

   for batch in formats.yaml.read_records("jobs.yaml"):
       for record in batch:
           print(record["job"].as_str(), record["nodes"].as_int())

To read one whole document, use ``read`` for a ``Value`` or ``load`` for plain
data. They need the whole input in memory. A file that is too large raises
``MemoryError``, and the message tells you to use ``read_records``.

A syntax error raises ``DFTUtilsReaderError``. The message has the file and the
line, such as ``events.jsonl:4:``.

Edit a record with mutable()
----------------------------

A ``Value`` never changes. ``mutable()`` gives a ``MutableValue``, which keeps
your changes apart from the source. Reads see the changes. The source ``Value``
stays as it was.

.. code-block:: python

   from dftracer.utils import formats

   record = formats.json.parse('{"id": 1, "name": "read", "tags": ["a"], "tmp": 0}')
   m = record.mutable()
   m["name"] = "pread"
   m["tags"].append("b")
   m["extra"] = {"ok": True}
   del m["tmp"]

   print(m.to_python())
   print(record.to_python())

The first line prints the changed data. The second line prints the source, which
did not change.

Write records with write()
--------------------------

``Writer.write`` takes one record. A record is a ``dict`` with ``str`` keys, a
list, a scalar, a ``Value`` or a ``MutableValue``. Use the writer as a context
manager. If the block raises, the writer removes its output.

This example edits every record of a file and writes the result to a new file.
A record that you did not change is written as its source bytes, with no second
parse.

.. code-block:: python

   from dftracer.utils import formats

   with formats.json.Writer("fixed.jsonl.gz") as w:
       for batch in formats.json.read_records("events.jsonl.gz"):
           for record in batch:
               if record["name"].as_str() == "write":
                   m = record.mutable()
                   m["name"] = "pwrite"
                   w.write(m)
               else:
                   w.write(record)

A ``compress`` of ``None`` picks gzip from a ``.gz`` suffix. Pass
``compress=True`` or ``compress=False`` to override the suffix. A JSON writer
cuts the gzip file into members of about ``member_size`` bytes (4 MiB by
default), so the file reads back in parallel. A YAML writer takes
``records="sequence"`` (the default) or ``records="documents"``:

.. code-block:: python

   with formats.yaml.Writer("jobs.docs.yaml", records="documents") as w:
       w.write({"job": "j0", "nodes": 1})
       w.write({"job": "j1", "nodes": 2})

A float that is NaN is written as the string ``"NaN"``. The infinities are
written as ``"Infinity"`` and ``"-Infinity"``. A value that is not in the
accepted types, such as a set, raises ``TypeError``.

Write records with record()
---------------------------

``Writer.record`` gives an ``ObjectBuilder`` for one record. Use it when you
build a record piece by piece, or when you do not want to build a ``dict`` first.
``object`` and ``array`` open a nested builder. Close the inner builder before
you call the outer one again.

.. code-block:: python

   from dftracer.utils import formats

   with formats.json.Writer("built.jsonl") as w:
       with w.record() as r:
           r.member("name", "read")
           r.member("dur", 5)
           with r.object("args") as args:
               args.member("fd", 7)
               with args.array("sizes") as sizes:
                   sizes.value(4096)
                   sizes.value(8192)

The file then holds this line:

.. code-block:: text

   {"name":"read","dur":5,"args":{"fd":7,"sizes":[4096,8192]}}

A call in the wrong order raises ``RuntimeError``. If the block raises, the
builder drops its record and the file keeps the records before it.

Convert to plain Python
-----------------------

``to_python`` gives plain ``dict``, ``list``, ``str``, ``int``, ``float``,
``bool`` and ``None`` values. It walks the parsed form and does not parse the
text again. ``loads`` and ``load`` do the same for a whole text or file. Use the
standard ``json`` module next to them by keeping the package import:

.. code-block:: python

   import json
   from dftracer.utils import formats

   data = formats.json.loads('{"a": [1, 2, {"b": null}]}')
   print(json.dumps(data))

   value = formats.json.parse('{"a": 1}')
   assert value.to_python() == json.loads('{"a": 1}')

In YAML every scalar is a ``str``, and ``~`` is ``None``:

.. code-block:: python

   print(formats.yaml.loads("a: 1\nb: [x, y]\nc: ~\n"))

This prints ``{'a': '1', 'b': ['x', 'y'], 'c': None}``.

See also
--------

- :doc:`../../api/formats` for every class, function and error.
- :doc:`compression` to write multi-member gzip from the command line.
