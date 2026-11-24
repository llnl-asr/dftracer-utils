:description: Reference for dftracer.utils.formats: the JSON and YAML readers and writers, with Value, MutableValue, RecordBatch, Writer and the record builders.

Formats (JSON and YAML)
=======================

``dftracer.utils.formats`` reads and writes JSON and YAML with the native
parsers. The two modules ``formats.json`` and ``formats.yaml`` have the same
names. A guide with the full workflow is :doc:`../guides/io/formats`.

The modules are named ``json`` and ``yaml``, like the standard ones. A file that
also uses the standard ``json`` imports the package as a module:

.. code-block:: python

   import json
   from dftracer.utils import formats

   for batch in formats.json.read_records("trace.jsonl.gz"):
       for record in batch:
           print(json.dumps(record.to_python()))

Names
-----

======================  =======================================================
Name                    What it is
======================  =======================================================
``read``, ``parse``     Parse one whole file or text. Give a ``Value``.
``load``, ``loads``     Parse one whole file or text. Give plain Python data.
``read_records``        Stream the records of a file, one ``RecordBatch`` at a
                        time.
``Value``               A read-only view of parsed text.
``MutableValue``        An overlay of changes on a ``Value``.
``Writer``              Writes records to a file.
``ObjectBuilder``,      Build one record or one nested value step by step.
``ArrayBuilder``
``Kind``                The kind of a ``Value``.
======================  =======================================================

Memory
------

``read``, ``parse``, ``load`` and ``loads`` parse one whole document. The input
must fit in the memory budget. A larger input raises ``MemoryError``, and the
message names ``read_records``.

``read_records`` streams. A JSON record is one line. A YAML record is an element
of a top-level block sequence or a document of a ``---`` stream. A file that is
one plain YAML document gives one record. Blank lines are skipped.

``batch_bytes`` caps the memory that one batch holds, including its parsed
form. The default is 64 MiB. A record that is larger than ``batch_bytes`` makes
a batch of its own. A loop that still holds the previous batch when it asks
for the next one holds about two batches.

A ``Value`` is a view and copies nothing. It keeps its document or its batch
alive. A ``Value`` that you keep from a record keeps the whole batch of that
record alive. Keep ``to_python()`` of what you need, not the ``Value``.

Parsing runs without the GIL. Other Python threads run while a call parses.

Raw text
--------

``raw`` is the text of a value. For the root of a record, and for the result of
``parse`` and ``read``, it is the exact source text:

- JSON record: the line without its newline.
- JSON document: the text without the white space around it.
- YAML record: the element from its ``-`` line, or the document from its
  ``---`` line, without the final newline.
- YAML document: the whole text.

For a value below the root, ``raw`` is rebuilt from the parsed form: compact
JSON, or block YAML (a scalar is its text).

A ``Writer`` writes an unchanged root value as its raw bytes, with no second
parse. A YAML record is written raw when the writer uses the same layout as the
source (``records="sequence"`` for a sequence, ``records="documents"`` for a
stream). A value of the other format, or a value in the other YAML layout, is
walked and written again.

MutableValue
------------

``Value.mutable()`` gives a ``MutableValue``. It keeps set, deleted and appended
entries apart from the source. The source ``Value`` never changes. Reads check
the changes first. A ``Writer`` writes the parts that you did not change as
their raw bytes. ``to_python()`` gives the data with the changes applied.

``__setitem__``, ``__delitem__`` and ``append`` take the same ``Input`` as a
writer. The index of an array may be negative.

Writing
-------

A ``Writer`` is a context manager. It streams, so the memory is about one gzip
member. ``write`` writes one record. ``record`` gives an ``ObjectBuilder`` that
builds one record member by member.

``compress=None`` picks gzip when the path ends in ``.gz``. ``compress=True``
and ``compress=False`` override the suffix.

``json.Writer`` writes one line per record in gzip members of about
``member_size`` bytes, so the file reads back in parallel. ``yaml.Writer``
writes the records as the elements of one block sequence (``records="sequence"``)
or as documents of a ``---`` stream (``records="documents"``).

A float that is NaN is written as the string ``"NaN"``. The infinities are
written as ``"Infinity"`` and ``"-Infinity"``. This is the spelling that
:doc:`trace_writer` uses. Any other float is written in the shortest form that
reads back to the same bits. A YAML scalar is text, so a number is written as
its text.

A writer is not thread-safe. A call made while another thread is inside a write
raises ``RuntimeError``.

A ``Writer`` that leaves its ``with`` block by an exception removes its output.
A builder that leaves its ``with`` block by an exception drops its record.

Errors
------

======================================  =======================================
Error                                   When
======================================  =======================================
``DFTUtilsReaderError``                 A syntax error, or a YAML feature that
                                        is not supported (anchors, aliases,
                                        tags, duplicate keys, tabs as
                                        indent). The message has the file and
                                        the line, such as ``bad.jsonl:4:``.
``FileNotFoundError``                   A file to read does not exist.
``DFTUtilsIOError``                     A writer cannot create its file, for
                                        example when the folder is missing.
``MemoryError``                         A whole document is larger than the
                                        memory budget.
``KeyError``                            ``value[key]`` has no such member.
``IndexError``                          ``value[i]`` is out of range.
``TypeError``                           A call of the wrong kind: ``value[key]``
                                        on an array, ``value[i]`` on an object,
                                        ``len`` or ``as_*`` of the wrong kind
                                        (JSON), ``iter`` on an object (use
                                        ``items()``), ``get`` on a value that
                                        is not an object. A writer gets a value
                                        that is not an ``Input`` (such as a
                                        set, bytes or a numpy scalar) or a key
                                        that is not ``str``.
``ValueError``                          YAML ``as_int``, ``as_float`` or
                                        ``as_bool`` of a scalar whose text is
                                        not that type. A bad option, such as
                                        ``batch_bytes=0``, ``member_size=0``,
                                        ``indent=0`` or an unknown ``records``.
                                        A value nested too deep to write.
``OverflowError``                       An ``int`` that does not fit in 64 bits
                                        (signed or unsigned) is written.
``RuntimeError``                        A builder call in the wrong order: a
                                        call on an outer builder while an inner
                                        one is open, a write or a record after
                                        ``close``, or a call from a second
                                        thread.
======================================  =======================================

``get`` returns ``None`` for a missing member and never raises ``KeyError``.

Module functions
----------------

The functions have the same signature in both modules. ``parse`` takes text.
``read`` and ``load`` take a path (``str`` or ``os.PathLike``) of a plain or
gzip file. A gzip file of several members reads as one stream.

.. autofunction:: dftracer.utils.formats.json.parse

.. autofunction:: dftracer.utils.formats.json.loads

.. autofunction:: dftracer.utils.formats.json.read

.. autofunction:: dftracer.utils.formats.json.load

.. autofunction:: dftracer.utils.formats.json.read_records

.. autofunction:: dftracer.utils.formats.yaml.parse

.. autofunction:: dftracer.utils.formats.yaml.loads

.. autofunction:: dftracer.utils.formats.yaml.read

.. autofunction:: dftracer.utils.formats.yaml.load

.. autofunction:: dftracer.utils.formats.yaml.read_records

Shared types
------------

These names are in ``dftracer.utils.formats``. They type the values that the
readers give and the writers take.

.. py:data:: dftracer.utils.formats.Scalar

   ``None``, ``bool``, ``int``, ``float`` or ``str``.

.. py:data:: dftracer.utils.formats.Data

   What ``to_python`` gives: a ``Scalar``, a ``dict`` with ``str`` keys of
   ``Data``, or a ``list`` of ``Data``.

.. py:data:: dftracer.utils.formats.Input

   What a writer and ``MutableValue`` take: a ``Scalar``, a mapping with ``str``
   keys, a sequence, or a ``ValueLike``. A tuple, a ``range`` and a read-only
   mapping are accepted. A ``set``, ``bytes`` and a numpy scalar are not.

.. autoclass:: dftracer.utils.formats.ValueLike
   :members:

JSON
----

.. autoclass:: dftracer.utils.formats.json.Kind
   :members:
   :undoc-members:

.. autoclass:: dftracer.utils.formats.json.Value
   :members:
   :special-members: __getitem__, __contains__, __len__, __iter__, __bool__

   A call of the wrong kind raises ``TypeError``. ``iter`` on an object raises
   ``TypeError``, so use ``items()``. ``as_int`` reads an ``int`` up to
   ``2**64 - 1`` and ``as_float`` reads an ``int`` too.

.. autoclass:: dftracer.utils.formats.json.MutableValue
   :members:
   :special-members: __getitem__, __setitem__, __delitem__, __len__, __iter__, __bool__

.. autoclass:: dftracer.utils.formats.json.RecordBatch
   :members:
   :special-members: __getitem__, __len__, __iter__

.. autoclass:: dftracer.utils.formats.json.RecordIterator
   :members:

.. autoclass:: dftracer.utils.formats.json.Writer
   :members:
   :special-members: __enter__, __exit__

.. autoclass:: dftracer.utils.formats.json.ObjectBuilder
   :members:
   :special-members: __enter__, __exit__

.. autoclass:: dftracer.utils.formats.json.ArrayBuilder
   :members:
   :special-members: __enter__, __exit__

YAML
----

A YAML scalar is text. ``as_str`` gives the text. ``as_int``, ``as_float`` and
``as_bool`` parse the text and raise ``ValueError`` when it is not that type.
``to_python`` gives every scalar as ``str``, and ``~`` or a missing value as
``None``. A ``Writer`` writes a string such as ``"yes"`` or ``"1"`` quoted, so
it reads back as the same text. The reader supports block and flow collections,
quoted and plain scalars, and comments. It refuses anchors, aliases, tags and
duplicate keys with ``DFTUtilsReaderError``.

.. autoclass:: dftracer.utils.formats.yaml.Kind
   :members:
   :undoc-members:

.. autoclass:: dftracer.utils.formats.yaml.Value
   :members:
   :special-members: __getitem__, __contains__, __len__, __iter__, __bool__

.. autoclass:: dftracer.utils.formats.yaml.MutableValue
   :members:
   :special-members: __getitem__, __setitem__, __delitem__, __len__, __iter__, __bool__

.. autoclass:: dftracer.utils.formats.yaml.RecordBatch
   :members:
   :special-members: __getitem__, __len__, __iter__

.. autoclass:: dftracer.utils.formats.yaml.RecordIterator
   :members:

.. autoclass:: dftracer.utils.formats.yaml.Writer
   :members:
   :special-members: __enter__, __exit__

.. autoclass:: dftracer.utils.formats.yaml.ObjectBuilder
   :members:
   :special-members: __enter__, __exit__

.. autoclass:: dftracer.utils.formats.yaml.ArrayBuilder
   :members:
   :special-members: __enter__, __exit__
