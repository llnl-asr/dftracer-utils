:description: Why the Python package ships full type information and how it is put together: the py.typed marker, the native-extension stub, inline annotations on the wrappers, ty checking, parity with the C++/C-ABI surface, and the statically checked JIT DSL.

Python typing
=============

What this explains: why the ``dftracer.utils`` Python package carries type
information for its whole public surface, what that covers, how it is assembled,
and where the guarantees stop. It is a companion to :doc:`dataframe-model`,
which describes the columnar model these typed wrappers expose.

Why a typed surface
-------------------

Almost every Python entry point is a thin wrapper over the native columnar
engine. A call that passes the wrong dtype, subscripts a ``Series`` with the
wrong key, or hands a ``DataFrame`` op an argument the C ABI cannot accept fails
inside the extension, where the traceback points at binding glue rather than at
the line the caller wrote. Typing moves those errors to where they happen: a
type checker and an editor flag them against the call site before the native
code runs, and completion in the editor lists the ops that actually exist rather
than guessing across a dynamic boundary.

The types also serve as a contract. The Python API aims for one-to-one parity
with the C++ and C-ABI columnar surface, so the annotations are the machine-
readable statement of that parity: the argument a Python method accepts is the
argument its C++ method and its ``dft_*`` C ABI accept, named and shaped the
same way. When a new op is added full-stack, its Python binding is typed from
the same declaration as the rest, so the surfaces cannot quietly diverge.

What the types cover
--------------------

The annotations span the whole public surface, not just the entry points:

- **Series and DataFrame ops.** Method signatures carry argument and return
  types, so an op that returns a new column is typed ``-> Series`` and one that
  returns a frame is typed ``-> DataFrame``. Operators are typed too:
  ``__add__`` and the other arithmetic operators return a ``Series``,
  ``__getitem__`` is typed for both the integer and the slice case, and the
  comparison operators return a boolean mask.
- **Constructors and converters.** The zero-copy importers and exporters name
  their foreign types: ``Series.from_arrow`` takes a ``pa.Array``,
  ``DataFrame.from_pandas`` takes a ``pd.DataFrame``, ``from_numpy`` /
  ``from_polars`` / ``from_parquet`` / ``from_dict`` each name their source, and
  ``to_arrow`` / ``to_pandas`` / ``to_polars`` / ``to_numpy`` name their result.
  ``__array__`` is typed so ``numpy.asarray`` over a ``Series`` resolves.
- **The columnar expression DSL.** ``col`` and ``lit`` and the ``F`` accessor
  build a typed ``Expr``; ``apply`` is typed ``-> Series`` and ``eval_many``
  ``-> List[Series]``, so a lazy expression graph is checked before it is
  evaluated on the engine.
- **duql.** The fluent ``Field`` builder is typed, so a predicate
  assembled in Python is checked before it is serialized and run as a mask.
- **Enumerations.** The op selectors and kinds that cross the boundary
  (``Phase``, ``GroupKey``, ``AggOp``) are Python enums that mirror the C enum
  values, so a caller passes a named member rather than a bare integer and a
  wrong one is a type error.

How it is assembled
-------------------

Three pieces cover the package, checked by one tool:

- **The ``py.typed`` marker** (``python/dftracer/utils/py.typed``) advertises
  the package as typed under PEP 561, so a downstream project that imports
  ``dftracer.utils`` gets these types from its own type checker with no extra
  configuration.
- **A stub for the native extension.** ``dftracer_utils_ext`` is a raw CPython
  C-API extension and carries no inline annotations, so a companion
  ``dftracer_utils_ext.pyi`` declares the signatures the rest of the package and
  its callers see.
- **Inline annotations on the wrappers.** The pure-Python layer (such as
  ``series.py``, ``dataframe.py``, ``columnar.py``, ``duql.py`` and
  ``runtime.py``) is annotated in place, so the wrapper source is both the
  implementation and its own type declaration.

``ty`` checks the ``python/`` tree (``make typecheck``, which runs
``uvx ty check python/``). Its configuration lives under ``[tool.ty]`` in
``pyproject.toml``, including an override list of the modules held to the
strictest checking.

The JIT DSL is checked the same way
-----------------------------------

The strongest form of typing here is the JIT authoring DSL, where the type
checker validates code that will become native. A ``@jit.each_event`` body is
deliberately typed so an ordinary type checker validates it before it is
compiled to a plugin: ``jit.map`` infers the ``Map[tuple[K], V]`` type of a
declared map, so a wrong subscript or an accumulation against the wrong value
type is a type error at authoring time rather than a surprise at run time. See
:doc:`../jit` for the DSL and its vocabulary.

Where the guarantees stop
-------------------------

Static types describe the Python shapes, not every run-time invariant the engine
enforces. A column dtype is an Arrow ``DataType`` value carried at run time, so a
type checker confirms that a method is called with the right kinds of argument
but the engine is still what rejects an op applied to an incompatible dtype. The
foreign types in the constructor and converter signatures (``pa.Array``,
``pd.DataFrame`` and the rest) are quoted forward references, so importing
``dftracer.utils`` does not require pandas, polars or numpy to be installed; the
annotations resolve for a type checker or an editor only when the corresponding
library is present.

See also
--------

- :doc:`../jit` for the statically checked JIT authoring DSL.
- :doc:`../guides/choosing-an-api` for when to reach for the Python surface
  rather than C++ or a plugin.
- :doc:`../api/index` for the generated Python API reference.
- :doc:`dataframe-model` for the columnar model the typed wrappers expose.
