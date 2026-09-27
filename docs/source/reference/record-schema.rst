:description: The keys of a record schema spec, the field types and roles, and the equivalent Python and C++ class declarations.

Record schema
=============

A record schema is a YAML or JSON mapping, a Python class or a C++ class; all
three resolve to the same definition. An unknown key or a value of the wrong
type is an error that names the source and the key. Specs load from
``.yaml``, ``.yml`` and ``.json`` files. To write one, see
:ref:`the indexing guide <describe-your-own-record-format>`.

Spec keys
---------

.. list-table::
   :header-rows: 1
   :widths: 26 18 56

   * - Key
     - Type
     - Meaning
   * - ``id``
     - string
     - Required. The schema id recorded with each file. ``dftracer`` and
       ``generic`` are built in and cannot be redefined.
   * - ``extends``
     - string
     - A registered schema to start from (default ``generic``). The new
       schema keeps its parent's decoder, fields and source.
   * - ``fields``
     - mapping
     - Field name to field settings (below). A field with the name of a
       parent field replaces it.
   * - ``index.path_budget``
     - count
     - How many of each file's most frequent other paths are indexed;
       replaces the parent's and the build's default.
   * - ``source``
     - string
     - duql text: the row sets (``name = pipeline``) and macros
       (``def name(a) = expr``) of the schema's source, separated by
       ``;``, and the flag ``def args_fallback = true``. A member with the
       name of a parent member replaces it; the others are kept. A source
       that does not compile rejects the spec. See
       :ref:`duql-sources`.

Field settings
--------------

.. list-table::
   :header-rows: 1
   :widths: 20 20 60

   * - Setting
     - Values
     - Meaning
   * - ``type``
     - ``bool``, ``int``, ``float``, ``string``, ``json``
     - Required. The type the field reads as. ``int`` keeps integers and
       whole numbers; ``float`` reads every number; ``string`` and ``bool``
       read only values of that JSON type; ``json`` reads any value, an
       object or array included, as canonical JSON text. Any other value
       reads as null. ``json`` needs a schema that decodes records by path
       (not one extending ``dftracer``).
   * - ``path``
     - string
     - The JSON path as the records write it (``meta.host``, ``tags.0``);
       the field name when absent.
   * - ``optional``
     - ``true``, ``false``
     - A required field (the default) is a detection path: a file is
       detected as the schema when at least 90 percent of its first 1000
       records hold every required path. A schema with no required field is
       used only when named.
   * - ``role``
     - ``time``, ``duration``, ``entity``, ``lane``, ``name``
     - The trace role the field plays; at most one field per role. Time and
       duration fields are ``int`` or ``float``; a ``string`` time holds
       ISO-8601 text and reads as microseconds since the epoch. Entity (the
       process, dftracer ``pid``), lane (the thread, ``tid``) and name fields
       are ``int`` or ``string``; a text entity or lane reads as a stable
       31-bit id. ``call_tree``, ``flamegraph`` and the group keys ``pid``,
       ``tid`` and ``name`` read these roles, and the group key ``rank`` the
       entity role.
   * - ``unit``
     - ``ns``, ``us``, ``ms``, ``s``
     - The unit of a numeric time or duration field (``us`` when absent).
       Times a query writes or reads are in these units.
   * - ``always_index``
     - ``true``, ``false``
     - Index the field even past the path budget. The time, duration,
       entity, lane and name fields of a path-decoded schema are always
       indexed.

Classes
-------

.. list-table::
   :header-rows: 1
   :widths: 24 38 38

   * - Spec
     - Python (``dftracer.utils.schemas``)
     - C++ (``dftracer/utils/index/schema_class.h``)
   * - ``id``, ``extends``
     - ``class A(RecordSchema, id="a")``; subclass a schema class, such as
       ``DFTracer`` or ``Generic``, to extend it
     - ``static constexpr std::string_view id = "a";`` and
       ``using extends = B;`` (``schemas::DFTracer``, ``schemas::Generic``)
   * - ``type``
     - annotation ``bool``, ``int``, ``float``, ``str``, ``Json``
     - member ``Field<T, "path">`` with ``T`` ``bool``, an integer, a
       floating point type, ``std::string`` or ``Json``
   * - ``optional``
     - ``Optional[...]``
     - ``std::optional<T>``
   * - ``path``
     - ``field(path=...)``; the attribute name when absent
     - the ``"path"`` template argument
   * - ``role``, ``unit``
     - ``field(role=..., unit=...)``
     - ``TimeRole<TimeUnit::MS>``, ``DurationRole<...>``, ``EntityRole``,
       ``LaneRole``, ``NameRole``
   * - ``always_index``
     - ``field(always_index=True)``
     - ``AlwaysIndex``
   * - ``source``
     - class attribute ``source = "..."``
     - ``static constexpr std::string_view SOURCE = "...";``
   * - registration
     - when the class is defined
     - ``index::register_schema<A>(source)``
   * - reading records as objects
     - ``TraceViewer.rows(A)``; ``A(**fields)``, ``None`` for a field not
       given
     - ``trace::views::rows<A>(view)`` in
       ``dftracer/utils/trace/views/typed_rows.h``; ``decode_row<A>`` for
       one parsed record

Built-in schemas
----------------

``dftracer`` reads DFTracer traces; its source declares ``data`` (every
record but the ``ph: M`` metadata records), the row sets ``files``,
``hosts``, ``strings`` and ``ranks``, and ``args_fallback``. ``genesis``
extends ``dftracer`` with the row set ``runs``. ``generic`` has no fields
and no source: ``data`` is every record and a bare name never reads
``args.<name>``. The source text is in :ref:`duql-sources`.

A spec that adds a row set:

.. code-block:: yaml

   id: nginx
   fields:
     status: {type: int}
     request_time: {type: float, role: duration, unit: s}
   source: |
     slow = where request_time > 1;
     def failed(s) = s >= 500

Loading and identity
--------------------

One id has one definition per process: registering the same definition
again does nothing, and another definition is an error that names both
sources. Where schemas load from:

- ``DFTRACER_SCHEMA_PATH``: files or directories, ``:``-separated, on first
  use.
- ``<index_dir>/schemas/``: when an index there is opened.
- Python ``dftracer.utils.schemas.register``/``load`` or a schema class,
  C++ ``index::register_schema``/``load_schemas``, C
  ``dftu_schema_register``/``dftu_schema_load``.
