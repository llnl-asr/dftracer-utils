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
       schema keeps its parent's decoder and fields.
   * - ``fields``
     - mapping
     - Field name to field settings (below). A field with the name of a
       parent field replaces it.
   * - ``index.path_budget``
     - count
     - How many of each file's most frequent other paths are indexed;
       replaces the parent's and the build's default.
   * - ``dictionaries``
     - list of mappings
     - Lookup rows; each one replaces the parent's dictionary of the same
       ``name``. Built only for schemas that extend ``dftracer``.
   * - ``dictionaries[].name``
     - string
     - Required. The dictionary name.
   * - ``dictionaries[].rows``
     - string
     - Required. The ``name`` of the metadata records (``ph: M``) that hold
       the rows.
   * - ``dictionaries[].key``
     - string
     - Required. The path of a row's key.
   * - ``dictionaries[].fields``
     - mapping
     - Required. Field name to path in the row record.
   * - ``dictionaries[].keys_in``
     - list of strings
     - Data fields that hold keys; ``resolved.<field>.<name>`` reads a row's
       field through them.

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
     - ``time``, ``duration``, ``entity``
     - The trace role the field plays; at most one field per role. Time and
       duration fields are ``int`` or ``float``. The entity is the default
       lane of ``call_tree`` and ``flamegraph``.
   * - ``unit``
     - ``ns``, ``us``, ``ms``, ``s``
     - The unit of a time or duration field (``us`` when absent); values
       convert to microseconds.
   * - ``always_index``
     - ``true``, ``false``
     - Index the field even past the path budget. The time field of a
       path-decoded schema is always indexed.

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
     - ``TimeRole<TimeUnit::MS>``, ``DurationRole<...>``, ``EntityRole``
   * - ``always_index``
     - ``field(always_index=True)``
     - ``AlwaysIndex``
   * - registration
     - when the class is defined
     - ``index::register_schema<A>(source)``
   * - reading records as objects
     - ``TraceViewer.rows(A)``; ``A(**fields)``, ``None`` for a field not
       given
     - ``trace::views::rows<A>(view)`` in
       ``dftracer/utils/trace/views/typed_rows.h``; ``decode_row<A>`` for
       one parsed record

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
