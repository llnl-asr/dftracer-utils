:description: The duql query language's syntax: tokens, keywords, paths, literals, the grammar, conditions, stages, row sets and limits.

duql syntax
===========

duql is the query language of dftracer-utils. Every query string, from
``TraceViewer.duql``, ``View::duql``, ``dftracer_view --duql``, the C ABI
or a plugin, is duql text. This page lists the syntax and what each construct does.

The builders in Python (``dftracer.utils.duql``, see :doc:`../api/duql`) and
C++ (``duql/builder.h``: ``duql::Pipe`` and ``duql::Col``) build the same
syntax tree as the equal text and print it as canonical text.

Tokens
------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Token
     - Rule
   * - Name
     - A letter or ``_``, then letters, digits or ``_``.
   * - Quoted key
     - Any text between backticks, read as one key: ``counters.`cpu.idle_pct`.p50``
       has the three keys ``counters``, ``cpu.idle_pct`` and ``p50``. An empty
       quoted key is an error.
   * - Integer, float
     - Digits, with an optional fraction and exponent (``1e-5``). A number
       never holds a sign: ``-5`` is unary minus applied to ``5``.
   * - Duration
     - A number followed directly by ``ns``, ``us``, ``ms``, ``s``, ``m``,
       ``h`` or ``d``: ``250ms``.
   * - String
     - Text between ``"`` or ``'``. ``\\``, ``\"``, ``\'``, ``\n``, ``\t``,
       ``\r`` and ``\uXXXX`` (a surrogate pair for one code point above
       U+FFFF) are escapes; any other backslash stays in the string, so a
       regex such as ``"\d+\.h5"`` keeps its escapes. Printed strings use
       double quotes and these escapes.
   * - Parameter
     - ``$name``. A caller binds it to a value; it is never spliced into the
       text. A value is a number, a string, ``true``, ``false``, or a list
       of them, which only ``x [not] in $name`` reads; an integer also indexes an
       array, as in ``a[$n]``, and any other value there is a compile error.
       ``x [not] [i]like
       $name`` reads a string. The C ABI and ``dftracer_view --param`` take
       a value as duql text, such as ``--param 'names=["read", "write"]'``.
   * - Comment
     - ``#`` to the end of the line.

``-`` is always an operator, so ``a-b`` is ``a - b``. A name followed by
``(`` with no space is a call: ``any(tags)``; ``f (x)`` is not a call.

Keywords
--------

In expressions these words are reserved, in any letter case: ``and``,
``or``, ``not``, ``in``, ``like``, ``ilike``, ``between``, ``is``,
``escape``, ``null``, ``true``, ``false``. A field with such a name is
written with backticks: ```null` == 1``.
``missing`` is a keyword only after ``is``, so ``missing == "x"`` reads a field
named ``missing``.

Stage names (``where``, ``derive``, ``select``, ``drop``, ``rename``,
``distinct``, ``group``, ``agg``, ``window``, ``pivot``, ``unpivot``,
``sort``, ``take``, ``skip``, ``sample``, ``expand``, ``parse``, ``lookup``, ``union``,
``call``, ``time_range``, ``call_tree``, ``bucket``, ``session``) and ``from`` are
keywords only at the start of a stage. Elsewhere they are field names:
``where sample == 1`` compares the field ``sample``. A query that starts
with a stage name is a stage, so a field with that name at the start is
written with backticks: ```sample` == 1``. After a ``|``, a name followed
by ``(`` that is not a stage name is a pipeline macro call (see `Macros`_).

The first line may declare the language version, as ``duql 0.1`` or
``duql 0``. A query runs when its major version equals the engine's and its
minor version is not greater. A missing minor counts as ``0``. Printed queries
always carry the engine's version.

Paths
-----

``a.b.c`` walks objects; ``a[0]`` indexes an array and ``a[-1]`` counts from
the end. ``a[$n]`` indexes with an integer parameter.

Any other expression indexes too: ``xs[i]`` and ``xs[i - 1]`` read the
element at that row's index, 0-based, with a negative index counting from the
end. The result is null when the index is null, not an integer or out of
range, or when ``xs`` is not an array. A computed index ends the path, so
``a[i].b`` and ``a[i][0]`` are parse errors. Indexing the result of a call,
as in ``sort(a)[0]``, is not supported; derive the call first.

``.`` alone is the current row and ``.name`` a field of it;
``^.name`` is a field of the enclosing row.

Wildcard paths
~~~~~~~~~~~~~~

A ``*`` in place of a key after the first makes the path a pattern:
``args.counters.*.p50``. The ``*`` is a token of its own and matches one
whole segment, so a field named ``*`` is reached only with backticks. A pattern
stands alone in one of these places and is a syntax error anywhere else:

- an item of ``select``, with no ``as`` and no ``name =``;
- a path of ``drop``;
- a path of ``unpivot``;
- the argument of ``any(...)`` and ``all(...)`` in a comparison.

A pattern expands when the query is planned, to every scalar field path of
the source with the same number of segments and the same text in each segment
that is not ``*``. A View reads the paths from the merged index path catalog,
which the full index holds (``dftracer_index`` or the ``BatchIndexer``) and
the first unfiltered full scan of a trace writes. A file with no catalog yet
is scanned once, for its catalog only, before the pattern expands (about
0.25 s for a 53 MB trace); a file whose catalog cannot be written, because
another process holds its index for writing, is refused with an error naming
the file. Patterns expand against the record
paths, so a column made by ``derive`` or ``group`` in an earlier stage is not
matched.
The query then runs as the same query with those paths spelled out, so pruning
and the prefilter apply to each path. Array positions are segments: with the
elements of ``args.pos`` indexed, ``args.pos.*`` matches ``args.pos.0``,
``args.pos.1`` and so on. Matches are ordered segment by segment: numeric
segments by value, so ``2`` comes before ``10``, and before the others, which go
by bytes. A source that falls back to ``args`` also lets a pattern that does
not start with ``args`` match the paths under it, written without ``args``.

.. list-table::
   :header-rows: 1

   * - Form
     - Meaning
   * - ``select p``
     - One column for each match, named by its path.
   * - ``drop p``
     - Drops each match.
   * - ``unpivot p as k, v``
     - One row for each match, with the path as the key.
   * - ``any(p) op v``
     - ``(m1 op v) or (m2 op v) or ...`` over the matches.
   * - ``all(p) op v``
     - ``(m1 op v) and (m2 op v) and ...`` over the matches.

A missing or null field is unknown for its comparison, as for a spelled-out
path. With one match, ``any(p) op v`` is that comparison. Without a ``*``,
``any(p) op v`` keeps its array meaning (see `Quantifiers`_). A pattern with
no match fails the query and names the pattern:
``no field matches the pattern 'args.nope.*'``. A pattern matches the paths of
the index catalog, so it does not match a column made by ``derive`` or
``group``. ``**`` and a partial segment such as ``p*`` are not patterns.

.. code-block:: text

   select name, args.counters.*.p50
   where any(args.counters.*.p50) > 92
   unpivot args.counters.*.p50 as counter, p50

Grammar
-------

Operators, from lowest to highest precedence: ``or``; ``and``; ``not``;
comparisons (``==``, ``!=``, ``<``, ``<=``, ``>``, ``>=``, ``~``, ``~*``,
``!~``, ``!~*``, ``in``, ``between``, ``like``, ``ilike``, ``is``); ``+``,
``-``; ``*``, ``/``, ``//``, ``%``; unary ``-``; ``??``; the arrow ``->``.
Comparisons do not chain.

.. code-block:: text

   query    = [ "duql" int ] { decl } [ pipeline ]
   decl     = "let" name "=" pipeline ";"
            | "def" name [ "(" names ")" ] "=" body ";"
            | "source" name "{" [ member { ";" member } [ ";" ] ] "}"
   member   = name "=" pipeline | "def" name [ "(" names ")" ] "=" body
   body     = expr                               # an expression macro
            | stage { "|" stage }                # a pipeline macro
            | expr { "|" stage }                 # a pipeline macro
   pipeline = "from" src { "," src } { "|" stage }
            | stage { "|" stage }
            | expr { "|" stage }                 # an expression means "where expr"
   stage    = stage-word args | name "(" [ args ] ")"   # a pipeline macro call
   path     = key { "." key | "[" int "]" }
            | "." [ key { "." key | "[" int "]" } ]      # the current row
            | "^" "." key { "." key | "[" int "]" }      # the enclosing row
   indexed  = path "[" expr "]"                   # a computed index ends the path
   pattern  = key "." "*" { "." ( key | "*" ) }   # select, drop, unpivot, any(), all()

A sub-query in an expression starts with ``from``:
``run in (from runs | select run)``. The legacy test ``"text" in field`` is
a case-insensitive substring match; it needs a string on the left.

``scripts/duql.lark`` holds the complete grammar, with the argument forms
of each stage. The ``duql_grammar`` test checks it against the samples in
``tests/duql/grammar_samples.duql``.

Conditions
----------

A View (``View::duql``, ``TraceViewer.duql``, ``dftu_view_duql``,
``dftracer_view --duql``) runs a pipeline: the stages in `Pipelines`_. Every
other surface (``Query``, DataFrame masks, the server) takes a single filter:
no ``from``, no stage after the first. A condition may use:

- fields: ``a.b``, ``a[0]``, ``a[-1]`` (from the end of an array) and
  backticked keys;
- literals, ``null`` and parameters (``$name``);
- list literals, ``["read", "write"]``, wherever an expression goes, such as
  ``derive t = [cat, name]``, ``contains(["read", "write"], name)`` and
  ``len([1, 2, 3])``; a list compared with ``==`` is unknown, as an array is;
- the comparisons ``==``, ``!=``, ``<``, ``<=``, ``>``, ``>=`` between any
  two expressions;
- ``in`` and ``not in`` with a list of expressions;
- ``like``, ``not like``, ``ilike``, ``not ilike`` (with an optional
  ``escape``) and the regex operators with a string pattern, on any
  expression;
- ``"text" in field`` and ``"text" not in field``;
- ``x between lo and hi`` and ``x not between lo and hi``;
- ``x is null``, ``x is not null``, ``x is missing``, ``x is not missing``;
- ``a ?? b``;
- the arithmetic operators ``+``, ``-``, ``*``, ``/``, ``//``, ``%`` and
  unary ``-``;
- the functions below;
- ``any(path)`` in place of a field, compared with a literal;
- ``and``, ``or``, ``not`` and parentheses.

A duration and the time functions need a View, which knows the record
schema's time roles (see `Durations and time`_).

Only a comparison, ``in`` list or pattern of one field against literals or
parameters, or ``any(path)``, can skip chunks through the index or the raw
line pre-filter. A condition with any other expression is checked on every
record the scan reads. ``exists(path)`` also skips a whole file whose index
catalog has no ``path`` and nothing under it.

Patterns
~~~~~~~~

- ``x like p`` matches the whole string: ``%`` matches any run of
  characters, newlines included, and ``_`` exactly one UTF-8 character.
  ``ilike`` ignores the case of ASCII letters.
- A ``like`` pattern has no escape character unless it names one:
  ``x like "50!%" escape "!"`` matches the text ``50%``. The escape
  character may precede only ``%``, ``_`` or itself.
- ``"text" in x`` is true when ``text`` occurs in ``x``, ignoring the case of
  ASCII letters.
- ``x ~ re`` is true when the regex ``re`` matches anywhere in ``x``;
  ``~*`` ignores case (Unicode letters included); ``!~`` and ``!~*`` are
  their negations. ``.`` matches one UTF-8 character.
- The regex dialect is the common subset of RE2, PCRE2 and Vectorscan:
  literals, escapes, character classes (POSIX classes and ``\p{...}``
  included), anchors, alternation, capturing, named and non-capturing
  groups, greedy and lazy quantifiers, and the inline options ``(?i)``,
  ``(?m)`` and ``(?s)``. Backreferences, lookahead and lookbehind, atomic
  and possessive groups, recursion, callouts, ``\C``, ``\K`` and verbs
  such as ``(*UTF)`` are compile errors.
- A yes/no regex match (``~``, ``~*``, ``!~``, ``!~*``, the DataFrame
  ``str_search`` and ``str_matches``, and the pattern masks) runs on PCRE2
  with a JIT, behind a SIMD check for the literals every match needs. A value
  of 256 bytes or more whose pattern has an unbounded dot gap (``.*``,
  ``.+``) runs on Vectorscan instead, where it is faster. Vectorscan needs
  valid UTF-8, does not run ``\X`` or ``\R``, and leaves a non-ASCII value to
  PCRE2 for a pattern whose non-ASCII results differ between the engines
  (Unicode properties such as ``\p{L}``, and case-insensitive non-ASCII
  letters). Captures (``extract``, ``findall``, ``parse`` and ``regex_replace``)
  always run on PCRE2. The
  results are the same on both engines.
- Each PCRE2 match has a work limit. A value that reaches it runs again on
  Vectorscan, which has no work limit, for the exact result; a value
  Vectorscan cannot take (invalid UTF-8, or a build without Vectorscan)
  gives unknown (null in a DataFrame kernel). So a pathological pattern such
  as ``^(a|aa)+$`` never hangs a scan. Where the regex JIT is not available
  the limit is counted differently, so a string at the edge of the limit may
  differ between platforms.
- A pattern that matches only literal text (``abc``, ``abc%``, ``%abc``,
  ``%abc%``, ``"abc" in x``) runs without a regex engine, on a SIMD
  substring search. Case-sensitive ``like`` and regex conditions also let
  the raw line pre-filter skip lines that lack the literals every match
  contains.

Numbers
~~~~~~~

- ``+``, ``-`` and ``*`` on two integers give an integer, and with a double
  a double.
- ``/`` always gives a double: ``7 / 2`` is ``3.5``.
- ``//`` gives the floor of the quotient: ``-7 // 2`` is ``-4``.
- ``%`` gives the remainder with the sign of the divisor: ``-7 % 3`` is
  ``2``.
- Division or remainder by zero, and an integer result outside int64, are
  unknown. An unsigned field above int64 still compares exactly and takes
  part in arithmetic whose result fits in int64.
- An operand that is missing, null or not a number makes the result
  unknown.

Presence
~~~~~~~~

``x is null`` is true when ``x`` holds ``null``; ``x is missing`` is true
when ``x`` is absent; ``exists(x)`` is true when ``x`` is present, ``null``
included. These tests are never unknown. ``a ?? b`` and ``coalesce(a, b,
...)`` give the first operand that is neither missing nor null.

``x == null`` is a compile error that suggests ``x is null``, and so is a
``null`` element in an ``in`` list.

Functions
~~~~~~~~~

A function never fails on a record: an input of a type it does not take
gives unknown.

.. list-table::
   :header-rows: 1

   * - Function
     - Result
   * - ``exists(path)``
     - Whether ``path`` is present (``null`` included).
   * - ``coalesce(a, b, ...)``
     - The first argument that is neither missing nor null.
   * - ``if(c, a, b)``
     - ``a`` when ``c`` is true, else ``b``.
   * - ``case(c1, a1, c2, a2, ..., else)``
     - The ``a`` of the first true ``c``, else the last argument.
   * - ``case { c1 => a1, c2 => a2, else => d }``
     - The same as ``case(c1, a1, c2, a2, d)``. ``else`` is optional and
       last; without it the default is null.
   * - ``abs``, ``floor``, ``ceil``
     - Of a number; an integer stays an integer.
   * - ``round(x)``, ``round(x, digits)``
     - ``x`` rounded half away from zero.
   * - ``min(a, b, ...)``, ``max(a, b, ...)``
     - The least or greatest argument; with one array argument, of its
       elements.
   * - ``log(x)``, ``exp(x)``, ``pow(x, y)``
     - As doubles; ``log`` of a value that is not positive is unknown.
   * - ``len(x)``
     - Characters of a string, elements of an array, keys of an object.
   * - ``concat(a, b, ...)``
     - The arguments as text, joined.
   * - ``lower``, ``upper``, ``trim``
     - Of a string (ASCII letters and whitespace).
   * - ``starts_with(s, p)``, ``ends_with(s, p)``
     - Whether string ``s`` starts or ends with ``p``.
   * - ``contains(s, sub)``, ``contains(array, v)``
     - Whether ``sub`` occurs in ``s``, or an element equals ``v``.
   * - ``substr(s, start)``, ``substr(s, start, n)``
     - Characters of ``s`` from ``start`` (0-based).
   * - ``replace(s, from, to)``
     - ``s`` with every ``from`` replaced by ``to``.
   * - ``regex_replace(s, re, to)``
     - ``s`` with every non-overlapping match of the regex ``re`` replaced by
       ``to``, left to right. In ``to``, ``$n`` and ``${n}`` insert group
       ``n``, ``${name}`` the named group and ``$$`` a dollar sign; a group
       that took no part inserts nothing. An empty match inserts ``to`` and
       advances one character: ``regex_replace("ab", "x*", "-")`` is
       ``-a-b-``. Null gives null; no match leaves ``s`` as it is. ``re`` and
       ``to`` are string literals or parameters. An invalid pattern, a missing
       group and a stray ``$`` are compile errors.
   * - ``first(array)``, ``last(array)``, ``sum(array)``
     - The first or last element, or the sum of the numbers. In a ``group``
       or ``agg`` block these names are aggregates.
   * - ``extract(s, re)``, ``extract(s, re, group)``
     - The first match of the regex ``re`` in ``s``, or its capture
       ``group``; unknown when nothing matches.
   * - ``json(x)``
     - The canonical JSON text of ``x``: no spaces, object keys sorted.
   * - ``type(x)``
     - ``"missing"``, ``"null"``, ``"bool"``, ``"number"``, ``"string"``,
       ``"array"`` or ``"object"``.
   * - ``int(x)``, ``float(x)``, ``string(x)``
     - ``x`` converted; a string converts when it is a number.
   * - ``slice(array, i)``, ``slice(array, i, j)``
     - The elements from index ``i`` up to, not including, ``j`` (the end
       when absent). A negative index counts from the end; an index past
       either end is clamped.
   * - ``index_of(a, v)``
     - The 0-based position of the first element of ``a`` equal to ``v``;
       null when none is, when ``a`` is not an array or when ``v`` is null.
   * - ``sort(a)``
     - The elements in ascending order, nulls last; null when the elements
       present do not compare, such as numbers with strings. A filter that
       starts with ``sort(`` reads as the ``sort`` stage: write
       ``where sort(a) ...``.
   * - ``unique(a)``
     - Each distinct element once, at its first position; numbers compare
       by value and nulls count as one value.
   * - ``join(a, sep)``
     - The string elements joined with ``sep``, nulls skipped; null when an
       element is not a string or ``a`` is not an array. ``sep`` is a string
       literal or a parameter.
   * - ``flatten(array)``
     - The elements, with each element that is an array replaced by its
       elements (one level).
   * - ``keys(object)``, ``values(object)``
     - The keys, or the values, of an object, in the byte order of the
       keys.
   * - ``parse_json(s)``
     - The value that the JSON text ``s`` holds; unknown when ``s`` is not
       valid JSON.
   * - ``split(s, sep)``
     - The parts of ``s`` between each ``sep``, empty parts included;
       unknown when ``sep`` is empty.

Missing, null and unknown
-------------------------

Each condition gives one of three results: true, false or unknown. A filter
keeps a record only when the result is true.

- A field that is absent from the record is *missing*. A field present as
  ``null`` is *null*. A comparison, ``in``, ``not in``, pattern or regex on
  a missing or null field is unknown.
- A comparison across types, such as a string with a number or a bool with
  a number, is unknown. An object or an array compared with anything is
  unknown; compare ``json(x)`` with a string instead.
- Numbers compare exactly: an integer is never rounded to a double, so a
  field that holds ``9007199254740993`` does not equal ``9007199254740992.0``.
- ``in`` is true when the value equals an element, false when it can be
  compared with an element and equals none, and unknown otherwise.
- ``any(path)`` is unknown when ``path`` is not an array, true when an
  element makes the condition true, and false otherwise (an empty array
  gives false).
- ``and``, ``or`` and ``not`` follow Kleene logic: ``false and unknown`` is
  false, ``true or unknown`` is true, ``not unknown`` is unknown, and every
  other mix with unknown is unknown.

So ``status != 200``, ``status not in [200]`` and ``not (status == 200)``
all drop a record without ``status``. A bare name that is missing from the
record reads ``args.<name>``; a name present as ``null`` does not. An empty
string is a value, not a missing field.

DataFrame masks follow the same rule: a leaf on a missing column is null in
every row, a cell of another type is null, and the mask keeps a row only when
it is true. A condition with an expression runs on column kernels, as the
stages of a pipeline do (see `Columns after the scan`_); a column of a type
with no duql meaning (a timestamp, a struct) reads as null. ``Series``
logical operators (``&``, ``|``) use Kleene logic when either side has
nulls.

Pipelines
---------

On a View, a query is a pipeline of stages joined by ``|``:

.. code-block:: text

   where cat == "POSIX" and dur > 250us
   | derive ms = dur / 1000
   | sort -ms
   | take 5
   | select name, ms

.. list-table::
   :header-rows: 1

   * - Stage
     - Result
   * - ``where c``
     - The rows where ``c`` is true.
   * - ``select a, b, x = e, e as x``
     - Only these columns, in this order; an expression needs a name.
       Every item reads the columns before the stage.
   * - ``derive x = e, y = f``
     - Adds or replaces columns; ``y`` may read ``x``.
   * - ``drop a, b``
     - All columns but these. It works on every column the rows carry,
       including those a trace scan adds that the index does not list.
   * - ``rename x = a``
     - Column ``a`` named ``x``, whether the index lists ``a`` or the scan
       adds it. Two columns left with one name is an error.
   * - ``distinct a, x = e, e2 as y``
     - One row for each distinct key, in first-occurrence order, with the
       key columns only. A key is named as in ``group``.
   * - ``sort a, -b, c nulls first``
     - Stable order by the keys; ``-`` sorts descending. Nulls come last in
       both directions unless ``nulls first`` is given.
   * - ``take n``, ``skip n``
     - The first ``n`` rows, or all but them. ``n`` may be a parameter.
   * - ``take a..b``
     - Rows ``a`` to ``b``, counted from 1, both included: ``skip a - 1 |
       take b - a + 1``. It needs ``1 <= a <= b`` and takes no ``by``.
   * - ``take n by a, e [sort s]``
     - For each distinct key tuple, its first ``n`` rows in input order, or
       in the order of the sort keys ``s`` (ties in input order). Rows keep
       that order in the output.
   * - ``sample n [seed s]``
     - ``n`` rows, in input order. ``n`` and ``s`` may be parameters.
   * - ``sample p% [seed s]``
     - About ``p`` percent of the rows, in input order.
   * - ``group a, x = e { y = f(...), ... }``
     - One row for each distinct key tuple (see `Group and agg`_).
   * - ``agg { y = f(...), ... }``
     - One row over all input rows.
   * - ``time_range [lo] .. [hi] [overlap]``
     - The rows in a time window, open on a side with no bound (see
       `Trace stages`_).
   * - ``bucket w [every e] [at t] [fill [forward | linear] [from lo to hi]] [as name]``
     - A time bucket key for the next ``group`` or ``agg``. With ``every``,
       overlapping or sparse windows.
   * - ``call_tree``
     - The nesting of events per ``pid`` and ``tid``.
   * - ``name(args)``
     - The stages of a pipeline macro (see `Macros`_).
   * - ``window [k1, k2] [sort s] { a = expr, ... }``
     - Every row, with one added column per entry (see `Window`_).
   * - ``session [k1, k2] gap d [max m] [as name]``
     - Every row, with its session number within its key (see `Session`_).
   * - ``expand p [as e] [with_index i] [keep_empty]``
     - One row for each element of the array ``p`` (see `Expand`_).
   * - ``parse p ~ re``
     - One String column for each named group of the regex ``re`` (see
       `Parse`_).
   * - ``pivot k [in [v1 [as n1], v2]] { a = agg(...), ... }``
     - The values of ``k`` as columns (see `Pivot`_).
   * - ``unpivot a, b as key, value``
     - One row for each listed field (see `Unpivot`_).
   * - ``lookup s on k [== c], ... [into m]``
     - Every row once, with the columns of the row set ``s`` (see
       `Row sets and lookups`_).
   * - ``lookup s on k [== c], ... inner [into m]``
     - Only the rows with a match in ``s`` (see `Row sets and lookups`_).
   * - ``lookup s on k [== c], ... anti``
     - Only the rows with no match in ``s`` (see `Row sets and lookups`_).
   * - ``lookup s on k [== c], ... overlap [into m]``
     - The rows of ``s`` whose time span overlaps the row's (see
       `Row sets and lookups`_).
   * - ``lookup s on k [== c], ... asof t [== c2] [direction] [within d]``
     - Every row once, with the columns of the row of ``s`` nearest in time
       (see `Row sets and lookups`_).
   * - ``union (from ...)``, ``union name``, ``union "file"``
     - The rows so far, then the rows of another pipeline, row set, ``let``
       or file (see `Row sets and lookups`_).

``| call ns.f(args)`` runs a function a loaded plugin registers (see
`Plugin functions`_). It must be the last stage.

The leading ``where`` stages filter events in the scan, so they use the
index and the raw line pre-filter as a filter does. A ``select`` of plain
fields that comes right after them is the scan's projection. The other
stages run on the scanned columns, in order. The scan reads every field that
a later stage names, a bare ``args`` name such as ``x`` for ``args.x``
included when the source sets ``args_fallback`` (see `Sources`_). When a
later ``group``, ``agg``, ``select``, ``pivot`` or keyed ``distinct`` sets
the output columns, no bare ``distinct`` comes before it, and the index lists
every field the stages name, the scan reads only those fields.

A ``sample`` depends only on the query, the seed (0 when absent) and the
files, not on the number of workers or the checkpoint size. ``sample n``
picks row positions of the ordered scan; ``sample p%`` keeps a row when a
hash of its position and the seed falls under ``p``. ``n`` and the seed are
integer literals.

Without a ``sort``, rows come in input order: by file path, then by line.
The order does not depend on the number of workers or the checkpoint size.

Columns after the scan
~~~~~~~~~~~~~~~~~~~~~~

The stages after the scan read columns. There a missing field and a JSON
``null`` are both a null cell:

- ``x is null`` is true for both;
- ``x is missing`` is the same test as ``x is null``, and ``exists(x)`` the
  same as ``x is not null``;
- a field that no record holds is a column of nulls.

Put a condition that must tell missing from ``null`` in a leading ``where``,
where the scan reads the records.

Every expression after the scan runs on column kernels with the results
given above, unknown as a null cell. A ``json`` field of the record schema
is text in a column: ``json(x)`` reads it, and any other expression on it is
an error. After the scan these arguments may be a column or any expression: the
needle of ``starts_with``, ``ends_with`` and ``contains`` on a string, ``from``
and ``to`` of ``replace``, ``start`` and ``length`` of ``substr``, ``digits``
of ``round`` and the group of ``extract``, as in
``lookup runs on run | derive p = starts_with(fname, prefix)``. Each row gives
what the scan gives: null where an operand is null, true for an empty needle,
the row unchanged for an empty ``from``, and null for a negative ``start``,
``length`` or group and for a group past the groups of the pattern. A pattern
(``~``, ``like``, the pattern of ``extract`` and ``regex_replace``) and the
replacement of ``regex_replace`` stay a literal or a parameter. ``??``, ``coalesce``, ``if`` and
``case`` whose values have different types are an error; convert them with
``int()``, ``float()`` or ``string()``.

Durations and time
~~~~~~~~~~~~~~~~~~

A duration (``5ns``, ``10us``, ``250ms``, ``1s``, ``5m``, ``1h``, ``1d``)
takes the unit of the
field it meets in a comparison, a ``between``, arithmetic or ``bin``. The
field must have a time or duration role in the View's record schema:
``dur > 250ms`` compares ``dur`` with 250000 when ``dur`` is in
microseconds. A duration beside a field with no role is an error that names
the field.

The unit follows the expression the duration meets:

- ``+``, ``-``, ``??``, negation, and the value arguments of ``coalesce``,
  ``if``, ``case``, ``min``, ``max`` and ``abs`` keep the unit their timed
  operands share: ``coalesce(dur, 0) > 1ms``, ``-dur < -1ms``. A negative
  duration such as ``-1ms`` is allowed.
- A product or quotient of a timed field (``dur / 1000``, ``dur * 2``) has no
  clear unit, so a duration beside it is an error; compare the field itself,
  or give the unit with ``as_time(dur / 1000, "ms")``.
- A column keeps the unit of the value it holds: ``derive``, ``window`` and
  ``select`` columns take the unit of their expression, ``rename`` moves it,
  and ``group`` and ``agg`` keep it for ``sum``, ``min``, ``max``, ``mean``,
  ``std``, ``first``, ``last`` and ``quantile`` of a timed value. Any other
  column has none, even when it reuses a timed field's name:
  ``group pid { dur = count() } | where dur > 1ms`` is an error.

.. list-table::
   :header-rows: 1

   * - Function
     - Result
   * - ``as_time(x, unit)``
     - ``x``, counted in ``unit`` (``"ns"``, ``"us"``, ``"ms"`` or
       ``"s"``), in the unit of the schema's time role; it then takes
       durations.
   * - ``to_seconds(t)``
     - The time or duration field ``t`` in seconds, as a double.
   * - ``bin(t, d)``
     - ``t`` rounded down to a multiple of ``d``: ``(t // d) * d``.
   * - ``date_part(t, part)``
     - The UTC calendar field of the time ``t`` as an integer. ``part`` is
       one of ``"year"``, ``"month"``, ``"day"``, ``"hour"``, ``"minute"``,
       ``"second"``, ``"millisecond"``, ``"microsecond"``, ``"nanosecond"``,
       ``"day_of_week"`` (Monday is 0), ``"day_of_year"`` (from 1),
       ``"quarter"``, ``"iso_week"`` or ``"iso_year"``, as a string literal
       or parameter. The sub-second fields count within their whole:
       ``"millisecond"`` 0 to 999 within the second, ``"microsecond"`` 0 to
       999999 within the second, ``"nanosecond"`` 0 to 999 within the
       microsecond, as pandas.
   * - ``format_time(t, fmt)``
     - ``t`` as UTC text. ``fmt`` is a string literal or parameter with
       the directives ``%Y %y %m %d %H %I %M %S``, ``%f`` (six digit
       microseconds), ``%j``, ``%a %A %b %B`` (English names), ``%p``
       (``AM`` or ``PM``), ``%F`` (``%Y-%m-%d``), ``%T`` (``%H:%M:%S``),
       ``%s`` (epoch seconds), ``%z`` (``+0000``), ``%Z`` (``UTC``) and
       ``%%``.
   * - ``now()``
     - The current time in the record's time unit. See below.

In ``date_part`` and ``format_time``, ``t`` is a field with a time or
duration role, or ``as_time(x, unit)``. Its unit comes from the role or the
unit; trace timestamps are Unix-epoch microseconds by default. A time before
1970 gives the calendar before 1970. Null gives null. The result is always
UTC; there is no other time zone. An unknown part, an unknown directive, a
trailing ``%`` and a ``t`` with no time unit are compile errors.

.. code-block:: text

   group h = date_part(ts, "hour") { n = count() }
   derive d = format_time(ts, "%F %T.%f")

For ``ts`` 1700000000123456 the second query gives
``2023-11-14 22:13:20.123456``.

``now()`` is read once when the query compiles. It is the same for every row
and every use in that query. It carries the time unit, so
``ts > now() - 1h`` works. It needs a View, because the unit comes from the
record schema's time roles; ``now()`` without a View is a compile error.

Group and agg
~~~~~~~~~~~~~

.. code-block:: text

   group name, big = dur > 1ms { n = count(), ms = sum(dur) / 1000 }
   agg { n = count(), p99 = quantile(dur, 0.99) }
   group name { count(), sum(dur) }

- A key is a field or a named expression (``name = expr`` or
  ``expr as name``). A field key is named by its path.
- Each block entry is ``name = expr``, where ``expr`` holds one or more
  aggregate calls and no field outside them, such as ``sum(dur) / 1000``,
  ``sum(size) / count()`` or ``coalesce(max(dur), 0)``. An aggregate's
  arguments are ordinary expressions, so ``sum(dur / 1000)`` is valid. A
  field outside an aggregate, such as ``dur + count()``, and an entry with no
  aggregate are compile errors. In an entry, ``sum``, ``min``, ``max``,
  ``first`` and ``last`` with one argument are the aggregate, so
  ``max(max(dur), 0)`` is the function over the aggregate. A ``pivot`` entry
  is one aggregate call.
- An entry without ``name =`` is named from its text: each run of
  characters other than letters, digits and ``_`` becomes one ``_``, trimmed
  at both ends. So ``count()`` is ``count``, ``sum(dur)`` is ``sum_dur`` and
  ``quantile(dur, 0.99)`` is ``quantile_dur_0_99``. A name that starts with a
  digit gets a leading ``_``. The same rule names the entries of a ``window``
  and a ``pivot`` block. Two entries with one name are an error.
- The result has the key columns, then one column per aggregate.
- Rows are sorted by the key tuple, ascending, with null keys last. Equal
  numbers are one key. A null key is its own group, shown as null.
- ``agg`` over zero rows gives one row: counts are 0 and the other
  aggregates null.
- An aggregate name that exists only in a block (``count``, ``count_if``,
  ``count_distinct``, ``mean``, ``var``, ``std``, ``quantile``,
  ``histogram``, ``collect``, ``arg_max``, ``arg_min``, ``sketch``,
  ``merge``, ``busy``, ``concurrency``, ``utilization``, ``active``) is a
  compile error outside one. Outside a block, ``sum``, ``min``, ``max``,
  ``first`` and ``last`` are the functions in `Functions`_.

Aggregates
~~~~~~~~~~

Every aggregate skips null inputs. Over a group with no non-null input,
``count(e)`` is 0 and the other value aggregates are null.

.. list-table::
   :header-rows: 1

   * - Aggregate
     - Result
   * - ``count()``
     - The number of rows.
   * - ``count(e)``
     - The number of rows where ``e`` is not null.
   * - ``count_if(c)``
     - The number of rows where ``c`` is true.
   * - ``sum(e)``
     - The sum. Integer input gives an integer; a sum outside int64 is
       null.
   * - ``min(e)``, ``max(e)``
     - The least or greatest value.
   * - ``mean(e)``
     - The mean, as a double.
   * - ``var(e)``, ``std(e)``
     - The sample variance and standard deviation (divided by n - 1).
   * - ``first(e)``, ``last(e)``
     - The first or last non-null value, in input order.
   * - ``quantile(e, q)``
     - The ``q`` quantile, within 1% relative error (DDSketch). ``q`` is a
       literal or a parameter in [0, 1].
   * - ``histogram(e)``
     - A list of buckets, each ``lo``, ``hi``, ``count``.
   * - ``count_distinct(e)``
     - The number of distinct values. Values that are equal count once, so
       ``1`` and ``1.0`` are one value. A list or object value is skipped.
   * - ``collect(e)``
     - A list of the values, in input order; an empty list when the group
       has none.
   * - ``arg_max(e, by)``, ``arg_min(e, by)``
     - ``e`` at the row with the greatest or least ``by``, the first such
       row on a tie. Rows where ``by`` is null are skipped; ``by`` orders
       bools, then numbers, then strings.
   * - ``sketch(e)``
     - A DDSketch of the numbers (1% relative error), as base64 text, the
       form trace summaries store.
   * - ``merge(s)``
     - The merge of the stored sketches ``s``, as base64 text. A value that
       is not a sketch, or a sketch of another accuracy, is skipped.
   * - ``quantile(merge(s), q)``
     - The ``q`` quantile of the merged sketches, clamped to their least and
       greatest values.
   * - ``busy()``
     - Time during which at least one record runs.
   * - ``concurrency()``
     - Average parallelism: the sum of durations divided by ``busy()``.
   * - ``utilization()``
     - ``busy()`` divided by the span from the first start to the last
       end; with ``bucket``, by the bucket width, and after a leading
       ``time_range``, by the window. They do not run after a ``bucket``
       with ``every``.
   * - ``active()``
     - The largest number of records that run at the same time.

The occupancy aggregates (``busy``, ``concurrency``, ``utilization``,
``active``) need the time and duration roles.

Trace and frame plans
~~~~~~~~~~~~~~~~~~~~~

A ``group`` or ``agg`` runs on one of two plans. Both give the same rows.

.. list-table::
   :header-rows: 1

   * - Plan
     - Taken when
     - Runs as
   * - Trace
     - The stage follows only scan stages (leading ``where``, a leading
       ``time_range``, ``bucket`` without ``every``, a leading field
       ``select``); every key is
       a string or integer field; every input is a numeric field; every
       aggregate is ``count()``, ``sum``, ``min``, ``max``, ``mean``,
       ``var``, ``std``, ``quantile`` with ``0 < q < 1``, ``histogram`` or an
       occupancy aggregate.
     - The View's trace aggregation in the scan: index, rollups, occupancy
       clipped to buckets.
   * - Frame
     - Any other case, including a ``bucket`` with ``every``.
     - Computed columns, then a LazyFrame ``group_by``. With
       ``count_distinct``, ``collect``, ``arg_max``, ``arg_min``,
       ``sketch`` or ``merge``, one streaming pass over the rows in input
       order that also folds the other aggregates of the block; explain
       shows it as ``group fold over ...``.

The occupancy aggregates need the trace plan. Where it is not possible they
fail with an error that names the aggregate and the reason, for example
``derive x = 1 | group name { b = busy() }`` names ``busy`` and ``derive``.

On the dftracer record schema, the index may hold no type for an ``args``
field. A group whose key or input is such a field takes the frame plan.

Trace stages
~~~~~~~~~~~~

The trace stages need the time role of the record schema; ``overlap`` and
``call_tree`` also need the duration role. On a schema without the role
each is a compile error that names the role. Durations in these stages take
the unit of the time role; ``lo``, ``hi`` and ``d`` are numbers,
parameters or durations.

.. list-table::
   :header-rows: 1

   * - Stage
     - Result
   * - ``time_range lo .. hi``
     - Rows whose start is in ``[lo, hi)``. As the first stage after the
       leading ``where`` stages, it is the View's ``time_range``: the
       occupancy aggregates then take every record that overlaps the
       window, clipped to it. Elsewhere it is a filter on the columns.
       ``lo ..`` and ``.. hi`` leave a side open; such a window is a filter
       on the columns, with no clipping. A bound is a number, a parameter,
       a duration, or ``+``, ``-``, ``*`` and ``/`` over them:
       ``$t0 .. $t0 + 10s``.
   * - ``time_range lo .. hi overlap``
     - Rows with ``ts < hi and ts + dur > lo``. As the first stage after
       the leading ``where`` stages, it is a scan filter.
   * - ``bucket d [as name]``
     - The next ``group`` or ``agg`` gets the first key ``bucket`` (or
       ``name``): the start
       of the ``d``-wide bucket that holds the row's start, aligned to 0.
       Occupancy aggregates clip each record to every bucket it overlaps.
       ``bucket d at t`` aligns the bucket starts to ``t`` (default 0):
       they are ``t + k * d``. A ``bucket`` with no ``group`` or ``agg`` after it is a compile
       error.
   * - ``bucket w every e``
     - Windows of width ``w`` that start every ``e``. A record at time
       ``t`` belongs to every window ``[s, s + w)`` that holds it, and the
       ``group`` or ``agg`` after the stage sees each window as a group, so
       every aggregate is exact. With ``e`` smaller than ``w`` the windows
       overlap, as in a moving rate: ``bucket 5s every 1s | agg { b =
       sum(size) }``. With ``e`` larger than ``w`` some records fall in no
       window. The starts are ``t + k * e``. ``w``, ``e`` and ``t`` read
       like ``d``: numbers, durations, parameters and arithmetic. A
       non-positive ``w`` or ``e`` is a compile error. Events of size 1, 2,
       3, 4 at 0, 1000, 2000, 3000 microseconds with
       ``bucket 2ms every 1ms | agg { b = sum(size), n = count() }`` give
       windows starting at -1000, 0, 1000, 2000, 3000 with ``b`` 1, 3, 5, 7,
       4 and ``n`` 1, 2, 2, 2, 1; ``bucket 2ms at 1ms | agg { n = count() }``
       gives buckets at -1000, 1000, 3000 with ``n`` 1, 2, 1. A hopping
       bucket runs on the frame plan and copies each row into each window
       that holds it, so the cost grows with rows times ``w / e``. The
       occupancy aggregates ``busy``, ``concurrency``, ``utilization`` and
       ``active`` do not run after it (a compile error); ``at`` alone keeps
       the trace plan and them.
   * - ``bucket d fill``
     - As ``bucket d``, and each group also has a row for every bucket
       between the first and the last, with counts 0 and other aggregates
       null.
   * - ``bucket d fill forward``
     - As ``bucket d fill``, and an added bucket takes the group's
       nearest earlier present value of every aggregate but the counts.
       It is null before the group's first present value. A present null
       is not filled.
   * - ``bucket d fill linear``
     - As ``bucket d fill``, and an added bucket takes the straight line
       between the group's nearest earlier and later present values. Only
       numeric aggregates are filled; a bucket without both neighbours is
       null. A present null is not filled.
   * - ``bucket d fill [forward | linear] from lo to hi``
     - The range runs from the bucket that holds ``lo`` to the bucket that
       holds ``hi``, and ``lo`` and ``hi`` read like ``time_range`` bounds.
       Buckets of the data outside the range stay. ``lo`` greater than
       ``hi`` is a compile error. Without ``from .. to`` the range is the
       result's first to last bucket, the same for every group. With
       ``every``, the missing window starts are on the ``e`` grid and
       ``lo`` and ``hi`` floor to it.
   * - ``call_tree``
     - For each event, per ``pid`` and ``tid``: its nesting ``depth`` and
       ``parent``, the output row number of the enclosing event (-1 for
       none). Columns ``pid``, ``tid``, ``ts``, ``dur``, ``name``,
       ``depth``, ``parent``; rows ordered by ``pid``, ``tid``, then start.
       It may follow only ``where`` stages.

``bucket d fill`` builds a grid of buckets times groups. The environment
variable ``DUQL_FILL_MAX_ROWS`` caps it (default 10000000); a larger grid
fails the query with an error that gives both counts. The cap
applies to the range of ``from .. to`` and to a hopping bucket. Every mode adds counts of 0.

Window
~~~~~~

.. code-block:: text

   window pid, name sort ts { n = row_number(), gap = ts - lag(ts) }

``window`` keeps every input row, in input order, and adds one column per
entry.

- Each entry is an expression that uses at least one window function, such
  as ``g = ts - lag(ts)``. A plain expression is a compile error, and so is
  a window function outside a window block.
- Rows are partitioned by the key tuple. With no keys there is one
  partition. A null key is its own partition.
- Inside a partition, rows are ordered by the ``sort`` keys: ``-k`` sorts
  descending, ``nulls first`` is allowed, and nulls come last by default.
  Ties keep input order.
- ``min`` and ``max`` with two or more
  arguments stay the scalar functions.
- ``sum``, ``mean``, ``min``, ``max``, ``count``, ``count_if``,
  ``count_distinct``, ``collect``, ``arg_max``, ``arg_min``, ``var``, ``std``
  and ``quantile`` take a frame. ``histogram`` does not:

  - ``f(e) over N rows`` is the row and the ``N - 1`` rows before it.
  - ``f(e) over d`` is the rows whose sort key lies in ``[k - d, k]`` for the
    row's key ``k``, peers included. It needs one ascending numeric sort key
    and a whole-number width in the key's units. A duration such as ``1s``
    needs a key with a time or duration role. The width may be a parameter.
  - An empty frame gives null. The counts give 0 and ``collect`` gives an
    empty list.
  - Each function gives the value its ``group`` aggregate gives over the
    frame. Nulls are skipped. ``arg_max`` and ``arg_min`` take the earliest
    row of the frame on a tie. ``collect`` lists the values in sort order.
    ``var`` and ``std`` are null with fewer than 2 values and 0 on a
    constant frame.
  - A framed ``quantile`` is exact, with linear interpolation. Over the whole
    partition it stays the sketch that the ``group`` aggregate uses, so the
    two can differ slightly.
  - A ``collect`` whose output passes 2^27 values in total fails. A range
    frame over a large partition repeats the partition for each row.

  .. code-block:: text

     window pid sort ts { m = mean(size) over 3 rows, bw = sum(size) over 1s }

.. list-table::
   :header-rows: 1

   * - Function
     - Result
   * - ``row_number()``
     - The 1-based position in the partition.
   * - ``rank()``, ``dense_rank()``
     - The rank with gaps, or without. Rows with equal ``sort`` keys tie;
       with no ``sort`` every row ranks 1.
   * - ``lag(e)``, ``lag(e, n)``, ``lead(e)``, ``lead(e, n)``
     - ``e`` of the row ``n`` before or after, in sort order. ``n`` is a
       non-negative integer literal or a parameter, default 1. Null past the
       edge of the partition. ``lag(e, n, default)`` and ``lead(e, n, default)``
       give ``default`` past the edge only; a null ``e`` stays null.
   * - ``running_sum(e)``
     - The sum of the non-null values so far; null until the first one. An
       integer sum is exact (``uint64`` over an unsigned column); a sum
       outside the type is a query error.
   * - ``running_count()``
     - The number of rows so far.
   * - ``running_min(e)``, ``running_max(e)``, ``running_mean(e)``
     - The minimum, maximum or mean of the non-null values so far; null
       until the first one.
   * - ``ntile(n)``
     - The 1-based bucket of the row when the partition is split into ``n``
       buckets of near-equal size.
   * - ``nth(e, k)``
     - ``e`` of the ``k``-th row (1-based) of the whole sorted partition;
       nulls are not skipped, and null when the partition is shorter.
   * - ``percent_rank()``, ``cume_dist()``
     - The relative rank ``(rank - 1) / (rows - 1)``, and the share of rows
       at or before the row. Rows with equal ``sort`` keys get equal values.
   * - ``fill_forward(e)``
     - The nearest non-null ``e`` at or before the row.
   * - ``count()``, ``count(e)``
     - Over the whole partition: its rows, or its rows where ``e`` is not
       null.
   * - ``sum(e)``, ``min(e)``, ``max(e)``, ``mean(e)``
     - Over the whole partition. Null inputs are skipped; ``sum`` over no
       values is null.
   * - ``first(e)``, ``last(e)``
     - The first or last non-null value in sort order, for the whole
       partition.
   * - ``count_if(c)``, ``count_distinct(e)``, ``collect(e)``
     - Over the whole partition or a frame: the rows where ``c`` is true, the
       distinct non-null values of ``e``, or the list of the non-null values
       of ``e``.
   * - ``arg_max(e, by)``, ``arg_min(e, by)``
     - The ``e`` of the row with the largest or smallest ``by``, over the
       whole partition or a frame.
   * - ``var(e)``, ``std(e)``, ``quantile(e, q)``
     - Over the whole partition or a frame, the same value the aggregate of
       that name gives in a ``group`` (see `Aggregates`_).
   * - ``histogram(e)``
     - Over the whole partition, the same value the aggregate of that name
       gives in a ``group``. It takes no frame.

The stage runs on the ``dftu.frame.window`` kernel after the ordered scan.
This holds for ``count_distinct``, ``collect``, ``arg_max`` and ``arg_min``
over the whole partition and for every framed function. Over the whole
partition, ``var``, ``std``, ``quantile`` and ``histogram`` are grouped by
partition and joined back to the rows.

Session
~~~~~~~

``session [k1, k2] gap d [max m] [as name]`` keeps every row, in input
order, and adds the Int64 column ``name`` (``session`` by default): the
1-based number of the row's session within its key. The rows of a key are
taken in the order of the record schema's time role, ties in input order.
The first row with a time starts session 1. A later row starts the next
session when its time is more than ``d`` after the latest end of the current
session, or, with ``max``, more than ``m`` after the time of the session's
first row. A row ends at its time plus its duration role, in the time role's
unit, or at its time when the schema has no duration role or the row no
duration. A row without a time has a null session and changes no session.

``d`` and ``m`` are durations (``30s``, ``5m``) or numbers in the time
role's unit. A schema without a time role, a negative ``d``, a ``max`` that
is not positive, or a ``name`` that is already a column is an error.

.. code-block:: text

   session pid, tid gap 1ms | group pid, tid, session { n = count(), t = sum(dur) }

gives one row per burst of each thread: bursts are separated by more than
1 ms of idle time. The stage runs on the ``sessionize`` window kernel after
the ordered scan and holds the columns it reads for every row in memory, as
``window`` does; the scan reads only the fields the pipeline names when a
later stage sets the output columns.

Expand
~~~~~~

.. code-block:: text

   where cat == "POSIX" | expand sizes as size with_index i

``expand p`` gives one row for each element of the array ``p``, in element
order, and repeats the other columns.

- The array column is replaced by the element column ``e``. Its default
  name is the last key of ``p``.
- Object elements become the columns ``e.<field>``. The fields come from
  the index path catalog.
- ``with_index i`` adds the 0-based position as ``i``.
- An empty array gives no row; with ``keep_empty`` it gives one row with
  ``e`` and ``i`` null.
- A missing, null or non-array ``p`` gives one row with ``e`` null (and
  ``i`` null).

Parse
~~~~~

.. code-block:: text

   where cat == "POSIX" | parse name ~ "(?<op>[a-z]+)\d*_(?<fd>\d+)"

``parse p ~ re`` adds one String column for each named group of the regex
``re``, named after the group, in group order. ``re`` is a string literal or
a parameter. The string ``"open64_17"`` gives ``op = "open"`` and
``fd = "17"``.

- A column is null where ``p`` is null, the pattern does not match, or the
  group took no part.
- A group named like an existing column replaces that column, as ``derive``
  does.
- Only ``~`` is allowed. For a case-insensitive match write ``(?i)`` in the
  pattern.
- It is a compile error when the pattern has no named group, when the
  operator is ``~*``, ``!~`` or ``!~*``, and when the pattern is invalid.
- The regex runs once for each group and row, so the cost is that of one
  ``extract`` for each group. It runs on PCRE2.

Pivot
~~~~~

.. code-block:: text

   group name, cat { t = sum(dur) } | pivot cat in ["POSIX", "STDIO"] { t = sum(t) }

``pivot k`` turns the values of ``k`` into columns. Its block takes the
aggregates of a `Group and agg`_ block.

- One row for each distinct tuple of the other columns: all columns except
  ``k`` and the columns the aggregates read. A scan field keeps its bare
  name, so ``f`` and not ``args.f``. Rows are sorted by that tuple.
- One column for each aggregate and value, named ``a.<value>``, in
  aggregate-major order. A cell is null when the tuple has no row with that
  value.
- A value name is the duql literal: a string as is, an integer without
  ``.0``, ``true`` or ``false``, and ``null``.
- ``v as n`` names the columns of the value ``v``: ``n`` when the block has
  one aggregate, else ``a_n`` for each aggregate ``a``.
- With ``in``, the columns are exactly the listed values, in list order, and
  later stages may follow.
- Without ``in``, the columns follow the values in ascending order. More
  values than ``DUQL_PIVOT_MAX_COLUMNS`` (environment variable, default
  1024) fail with an error that names both counts. Any stage after the
  pivot is then a compile error that asks for ``in``.

Unpivot
~~~~~~~

.. code-block:: text

   unpivot read_bytes, write_bytes as kind, bytes

For each input row, ``unpivot`` gives one row for each listed field, in list
order. The first name after ``as`` holds the field name and the second its value. The other
columns are kept.

The listed fields must fit one value type:

- The same type stays.
- Integers of different widths give Int64.
- Integers and doubles give doubles.
- If the type of any listed field is unknown, the values become strings.
- Any other mix is an error that names the fields and their types.

Quantifiers
~~~~~~~~~~~

.. code-block:: text

   any(hosts, .state == "down")
   all(sizes, . > 0)
   any(events, .name == ^.name)

``any(p, e)`` and ``all(p, e)`` test ``e`` on each element of the array
``p``. Inside ``e``, ``.`` is the element, ``.name`` a field of it, ``.[0]``
an index into it and ``^.name`` a field of the record.

.. list-table::
   :header-rows: 1

   * - Call
     - True
     - False
   * - ``any(p, e)``
     - ``e`` is true for some element.
     - ``p`` is an array and ``e`` is false for every element (an empty
       array gives false).
   * - ``all(p, e)``
     - ``p`` is an array and ``e`` is true for every element (an empty array
       gives true).
     - ``e`` is false for some element.

Any other case is unknown, and both are unknown when ``p`` is missing, null
or not an array.

- ``.`` and ``^.`` outside a quantifier are compile errors, and so is a
  quantifier inside another quantifier.
- ``any(p) op v`` equals ``any(p, (. op v) ?? false)``, as the existing
  ``any()`` leaf does. ``all(p) op v`` equals ``all(p, . op v)``.
- ``any(p)`` and ``all(p)`` with a pattern ``p`` expand over the matching
  fields instead (see `Wildcard paths`_).
- In a scan filter, a quantifier is checked per record and never skips a
  chunk. In a stage after the scan, it runs vectorized over list columns.
  Both give the same results. DataFrame and LazyFrame duql filters take
  quantifiers over List columns.

Arrays after the scan
~~~~~~~~~~~~~~~~~~~~~

An array field that a later stage reads is a List column after the scan,
and an object field is one column per leaf, such as ``args.o.a``. The
element type comes from the index path catalog, which the Indexer builds.
Object elements make a ``List<Struct>``. Without a path catalog the element
types are inferred for each batch, and a field that no stage reads as an
array reads by name.

If the elements of one array field mix types, such as numbers and strings,
the elements are JSON text. An after-scan quantifier over that field then
fails with an error that says its elements mix types. A scan filter still
works.

After the scan, ``slice``, ``flatten``, ``keys``, ``values``,
``parse_json``, ``split``, ``index_of``, ``sort``, ``unique`` and ``join`` each compute a column that the rest of the
expression reads, and explain shows it as ``call __duql_c_<n> = ...``.

- ``values(o)`` needs fields of one type, or numbers only, which become
  doubles when they mix integers and doubles. Other mixes, and a field that
  holds an object or array, are compile errors that name the field.
- An object with no field that holds a value reads as missing, so
  ``keys(o)`` of ``{}`` is unknown after the scan and ``[]`` in a scan
  filter.
- ``parse_json(s)`` gives canonical JSON text. Only ``json()`` and a whole
  ``select`` or ``derive`` item read it; a scan filter reads the value.
- Inside the condition of ``any`` or ``all``, these functions run only in a
  scan filter.
- A list literal after the scan needs items of one type. ``join`` over a
  list of strings runs as an expression, with no column call.
- For ``tags`` equal to ``["b", "a", "b", null]``, ``sort(tags)`` is
  ``["a", "b", "b", null]``, ``unique(tags)`` is ``["b", "a", null]``,
  ``join(tags, ",")`` is ``b,a,b`` and ``index_of(tags, "a")`` is 1.
- ``contains(array, v)`` of a string array has no column kernel; it runs
  only in a scan filter.

.. _duql-plugin-functions:

Plugin functions
~~~~~~~~~~~~~~~~

.. code-block:: text

   derive e = myplug.entropy(x, 8) * 2
   select name, myplug.twice(dur) as d
   sort ts | call myplug.head(3)

A loaded plugin registers functions under a dotted name (``ns.f``). Build the
plugin set before the query runs; its functions are visible until the set is
released.

- A scalar function (column to column) runs in a ``derive`` or ``select``
  entry, alone or anywhere inside the entry's expression, and gives one value
  per row. Later ``derive`` entries read the result.
- The first argument is a column expression. Each later argument fills, in
  order, the next operand of the function: a string literal is the text
  operand, a number or bool literal or a parameter is a scalar operand (at
  most two), and one other expression is the second column. More arguments
  than that are a compile error. Arguments are positional; a duration literal
  is not accepted.
- The result type is the function's own.
- ``| call ns.f(args)`` runs a table function (table to table) over the rows
  so far. Its literal arguments fill the operands after the table, in order.
  The result columns are the function's. ``call`` must be the last stage;
  any stage after it is a compile error.
- A dotted name that no loaded plugin registered is a compile error that
  lists the loaded plugin namespaces, or says that none is loaded.
- A plugin function in a ``where``, a key, an aggregate argument, a ``let``
  or a sub-query is a compile error. The error for a ``where``, key or
  aggregate argument names ``derive`` and ``select``.
- Arguments the function refuses fail before any row is read. The message
  names the function and its signature. On a schemaless view, where a
  column's type is known only from its rows, an argument type the function
  refuses fails at the first batch.
- A function a plugin registers as a reducer (column to value) runs as an
  aggregate in a ``group`` or ``agg`` entry, alone or inside an aggregate
  expression:
  ``group name { e = myplug.entropy(dur) }`` or
  ``agg { r = myplug.entropy(dur) / count() }``.
  The first argument is a column expression and later arguments are number
  literals or parameters that fill the reducer's operands in order.
- A reducer sees each group's non-null values in input order. A group with no
  non-null value gives null. The column type follows the reducer's result
  (integer, float or bool); a generic scalar result is a float.
- A reducer in a ``window`` or ``pivot`` block is a compile error. Arguments
  it does not take fail before any row is read and name its signature. A
  reducer that fails on a group fails the query.
- A plugin aggregate runs on the frame plan and keeps every group's values
  until the end, like ``collect``. Plugin mergeable states and jit
  accumulators are not duql aggregates. A plugin node is not a ``call``
  target.

Explain
~~~~~~~

``View::explain_duql``, ``TraceViewer.explain_duql`` (also
``explain_query``),
``dftu_view_explain_duql`` and ``dftracer_view --duql ... --explain`` print
the plan without scanning, one step per line:

.. code-block:: text

   scan filter: cat == "POSIX" and dur > 250 (pushed)
   scan order: file, then line
   with_column ms = dur / 1000
   sort_by_multi ms
   head 5
   select name, ms

A ``group`` or ``agg`` line names the plan it took, and the new steps have
their own lines:

.. list-table::
   :header-rows: 1

   * - Stage
     - Explain line
   * - ``group`` or ``agg``
     - ``group (trace plan): keys name; aggs n = count()`` or
       ``group (frame plan): keys ...; aggs ...``
   * - ``bucket d`` (trace plan)
     - ``time_bucket d``
   * - ``bucket w every e [at t]``
     - ``hop buckets w every e [at t]``
   * - ``bucket d fill``
     - ``fill buckets d [forward | linear] [from lo to hi]``
   * - ``take n by k``
     - ``head_by k n``
   * - ``sample n seed s``
     - ``sample n seed s``
   * - leading ``time_range lo .. hi``
     - ``scan time_range lo .. hi (pushed)``
   * - ``call_tree``
     - ``call_tree pid, tid``
   * - ``window``
     - ``window keys pid, name; sort ts; gap = ts - lag(ts)``
   * - ``expand``
     - ``expand sizes as size with_index i keep_empty``
   * - ``pivot``
     - ``pivot cat in [POSIX, STDIO]``, or ``pivot cat (columns after the
       scan)`` without ``in``
   * - ``unpivot``
     - ``unpivot a, b as k, v``
   * - Quantifier after the scan
     - ``quantify __duql_q_0 = any(...)``
   * - A row set a lookup reads
     - ``side runs: from all; not cached`` (or ``cached, N rows``), then
       its own plan, indented; ``side files: from all; stored in the
       index`` for a row set the index stores
   * - ``from`` a row set of the source
     - ``from files (rows stored in the index)`` or ``from runs (its side
       runs when the query executes)``
   * - A pipeline that reads row sets
     - ``reads runs before the scan``
   * - A key set added to the scan filter
     - ``scan filter: run in (keys of runs) (pushed)``
   * - Lookup after the scan
     - ``lookup __duql_l_0 = run -> runs.app``
   * - ``lookup``
     - ``lookup runs on run == run (side joined, sharing the scan)``, or
       ``(side from memory)`` when the build step or the cache has it
   * - ``lookup ... overlap``
     - ``lookup ph on pid == pid overlap (sweep over ph)``
   * - ``lookup ... asof``
     - ``lookup s on pid == pid asof ts == t forward within 5 (op
       dftu.frame.asof)``, then ``sort_by __duql_pos``, which puts the rows
       back in input order
   * - ``union``
     - ``union from "b.pfw.gz":``, then its plan, indented

Row sets and lookups
--------------------

A query on a View can name row sets and read them by key: its own ``let``
row sets and the row sets of the record schema's source. Each row set the
query reads is a lookup side: it runs once, as the first step of executing
the query, before the main scan, unless the index stores its rows (see
`Sources`_). ``View::duql`` and ``explain`` run no side.

.. code-block:: text

   let runs = where type == "run" | select run, app;
   where type == "ev" and run -> runs.app == "laghos"
   | lookup runs on run
   | select i, app

``let name = pipeline;``
   Names a row set that later declarations and the main pipeline read. A
   ``let`` or a sub-query with no ``from`` reads ``all``. A ``let`` that
   reads itself or a later ``let`` is a compile error, and so is a ``let``
   named ``all``, ``data`` or a row set of the source.

``from x`` and ``from a, b``
   The records a pipeline starts from: a ``let``, a row set of the source,
   ``all`` (every record of the View's files, metadata records included,
   with none of the View's filters), ``data`` (the source's ``data`` row
   set; the default of the main pipeline), a quoted file path, or a
   parameter bound to one. A file is read with its own index. ``from a, b``
   is the rows of ``a``, then the rows of ``b``. Any other name is a compile
   error.

``k in (from ...)``, ``(a, b) in (from ...)``, ``k not in (from ...)``
   A semi-join: TRUE when some row of the sub-query equals the key, FALSE
   when the key is present and none does, UNKNOWN when a key part is
   missing or null. ``not in`` is its negation. The sub-query gives one
   column per key part.

``k -> s.path``, ``k -> s(id).path``, ``(a, b) -> s.path``
   The value at ``path`` of the row of the row set ``s`` (a ``let`` or a
   row set of the source) whose key column (named like ``k``, or ``id``)
   equals ``k``. Arrows chain:
   ``run -> runs.sys -> systems.cpu``. No match, or a missing or null key,
   gives MISSING.

``(from ...)`` in an expression
   A scalar sub-query: the value of its one cell, null when it gives no
   row. More rows or columns are a query error that names the sub-query. A
   sub-query may read the enclosing row with ``^.``; see `Correlated
   sub-queries`_.

``lookup s on k [== c], ... [into m]``
   Keeps every row once, in order, and adds each column of ``s`` but its
   key columns; a row with no match gets nulls. ``c`` names the key column
   of ``s`` when it is not named like ``k``. With ``into m`` it adds the
   list ``m`` of every matching row as an object, ``[]`` when none. It runs
   as the engine's ``lookup`` (or ``nest``) join: the rows stream, and when
   ``s`` is not already in memory its plan joins in, so a side over the same
   files shares their scan. The side has no size cap: when it outgrows the
   View's memory budget the join partitions both sides on the key onto disk
   and joins one partition at a time, giving the same rows and columns in an
   unspecified order (see `Limits`_).

``lookup s on k [== c], ... inner [into m]``
   Keeps only the rows with a match in ``s``, in order, with the columns, the
   rules and the errors of ``lookup``: at most one distinct match, and a
   conflict is an error. With ``into m`` it keeps the rows whose ``m`` is not
   empty. A row whose key is null has no match, so ``inner`` drops it.

``lookup s on k [== c], ... anti``
   Keeps only the rows with no match in ``s``, in order, and adds no column,
   so it takes no ``into``. A row whose key is null has no match, so ``anti``
   keeps it. It gives the rows ``where k not in (from s | select k)`` gives,
   except that a row with a null key is kept. ``inner`` and ``anti`` do not
   combine with ``asof`` or ``overlap``.

``lookup (from ...) on k``
   The side may be an inline pipeline instead of a name, such as
   ``lookup (from data | where type == "run" | select run, app) on run``. It
   may not read the enclosing row (``^.``).

``lookup s on k [== c], ... asof t [== c2] [backward | forward | nearest] [within d]``
   Keeps every row once, in order, and adds each column of ``s`` but its
   key columns and its time column ``c2`` (``t`` when it is not named). The
   row it takes is the one of ``s`` with equal keys and the time nearest
   ``t``: ``backward`` (the default) the largest time not after ``t``,
   ``forward`` the smallest not before it, ``nearest`` the closest, an equal
   distance going to the earlier time. Among rows of ``s`` with the same time
   ``backward`` and ``nearest`` take the last and ``forward`` the first, in
   the row order of ``s``. A row whose key or time is null or missing, or
   that has no candidate, gets nulls; a row of ``s`` with a null key or time
   never matches. ``within d`` also gives nulls when the taken time is more
   than ``d`` away. ``d`` is a non-negative number in the units of ``t``
   (a fraction bounds integer times by its floor), a parameter, or a
   duration such as ``5ms``, which takes the unit of ``t``'s time or
   duration role as in a comparison (a ``t`` with no role refuses it). It equals
   pandas ``merge_asof`` with the same direction, tolerance and keys. Times
   and keys of different number types compare as integers when both are
   integers, else as doubles. It runs on the engine's ``asof`` op, which
   collects the rows and ``s`` (``s`` is read in file order, before the
   scan), so it does not combine with ``into``.
   A column of ``s`` the rows already have is an error, except a record field
   the rows lack: those rows keep their own value and take ``s`` where they
   have none.

``lookup s on k [== c], ... overlap [into m]``
   Matches each row to the rows of ``s`` with equal keys whose interval
   overlaps its own. An interval is ``[time, time + duration)`` from the
   record schema's time and duration roles (a schema without a duration role
   is a compile error); ``s`` must carry those fields under their names
   (``select pid, ts, dur, phase``), and they are its interval, not added
   columns. Intervals ``[a, a + d)`` and ``[b, b + e)`` overlap when
   ``b < a + d`` and ``a < b + e``, so intervals that only touch do not, and
   an interval of length 0 overlaps only the intervals it lies strictly
   inside. A null or negative time or duration, or a null key, on either
   side never matches. Without ``into`` each match is an output row, the
   matches of a row in the row order of ``s``, and a row with no match stays
   once with nulls. With ``into m`` each row stays once and gets the list
   ``m`` of its matches as objects, ``[]`` when none. It collects the rows
   and reads ``s`` in file order before the scan, then sweeps: ``s`` is
   sorted by start within each key with a tree of latest ends, and each row
   takes the intervals that start at or before it and end after it, plus
   those that start inside it, so the time is ``O((n + m) log m + k)`` for
   ``n`` rows, ``m`` rows of ``s`` and ``k`` matches. Times and durations
   that are integers of up to 32 bits, or int64, compare exactly; others
   compare as doubles.

``union (from ...)``
   The rows so far, then the rows of the other pipeline. Columns match by
   name; a column on one side only is null on the other. Integers and
   doubles unify to doubles; other type mixes are an error that names the
   column.

.. _duql-sources:

Sources
~~~~~~~

A record schema's ``source`` (see :doc:`record-schema`) holds named row sets
(``name = pipeline``) and macros (``def``), separated by ``;``. A source row
set with no ``from`` reads ``all``. ``data`` is the row set a query with no
``from`` reads and a View reads; it is one ``where``, and ``all`` when the
source does not declare it. No row set may be named ``all``. A ``source``
declaration inside a query is a compile error: a source belongs to a record
schema.

The built-in sources:

.. code-block:: text

   # dftracer
   data = where ph not in ["M", 4];
   files = where ph in ["M", 4] and name == "FH" | select fhash = args.value, path = args.name | distinct;
   hosts = where ph in ["M", 4] and name == "HH" | select hhash = args.value, name = args.name | distinct;
   strings = where ph in ["M", 4] and name == "SH" | select shash = args.value, value = args.name | distinct;
   ranks = where ph in ["M", 4] and name == "PR" and args.name == "rank" | select pid, rank = args.value | distinct;
   def args_fallback = true

   # genesis
   data = where gtype != "run";
   runs = where gtype == "run"
        | select run, app, system, unique_input, nodes, ppn, papi_set,
                 method, sketch_accuracy, leaf
        | distinct

``ph`` 4 is the numeric metadata phase. The ``generic`` source is empty:
``data`` is every record.

``def args_fallback = true`` is a flag, not a macro. With it, a bare name
that a record does not hold reads ``args.<name>``: ``x`` reads ``args.x``. A
name present as ``null`` does not fall back. Without it (the ``generic``
source), ``x`` reads only the field ``x``.

A row set that is a ``where`` followed by at most one ``select`` of record
paths and an optional bare ``distinct``, with no arrow, lookup or
aggregate, is built with the index and stored per file (the ``core.rowset``
extension); with ``distinct``, each row once per file. Reading it decodes no
trace. Any other row set, and a file whose index holds no rows for it, runs
as a lookup side over ``all`` when the query executes. Both give the same
rows. Stored cells keep their JSON type: a number stays a number.

Macros
~~~~~~

``def name(a, b) = expr;`` defines a macro. A call ``name(x, y)`` expands to
a copy of ``expr`` with each parameter replaced by the argument in its
position, before the query is planned; any other name in the body is a
record path. A macro in a filter therefore pushes down as its expansion
would. A macro body may call other macros.

A body that starts with a stage word (``where``, ``bucket``, ``group`` and
the others) or with an expression followed by ``|`` is a pipeline macro; any
other body is an expression macro. One name space holds both kinds.

.. code-block:: text

   def io_rate(d) = where cat == "POSIX" | bucket d | agg { b = sum(size) };
   from data | io_rate(1ms) | sort b

A pipeline macro is called at a stage position: after a ``|``, or alone as
the first stage (``io_rate(1ms) | sort b``). The call runs the stages of the
body in its place, with each argument bound by position in the expressions
of those stages, before the query is planned. The ``where`` of a leading
call pushes into the scan as a hand-written ``where`` would. A body may call
expression and pipeline macros:

.. code-block:: text

   def pos = where cat == "POSIX";
   def rate(d) = pos() | bucket d | agg { n = count() };

A leading call that names no pipeline macro keeps its meaning as a filter:
``slow(250) | group ...`` and ``exists(x) | ...`` still test a condition.

Macros come from three scopes, and a name in an earlier scope hides the same
name in a later one:

1. the query;
2. the record schema's source;
3. the ``.duql`` files on ``$DFTRACER_DUQL_PATH`` (files or directories,
   ``:``-separated; a directory gives its ``.duql`` files in name order),
   then those of each ``--duql-path PATH`` of ``dftracer_view``,
   ``dftracer_stats`` and ``dftracer_comparator``. A macro file holds only
   ``def`` declarations.

A cycle, a wrong number of arguments, a named argument, a macro with the
name of a built-in function or of a stage, and two macros of one name in one
scope are compile errors. So are a pipeline macro inside an expression
(``where io_rate(1ms)``: "macro 'io_rate' is a pipeline; call it as a
stage"), an expression macro after ``|`` ("macro 'slow' is an expression;
write 'where slow(...)'") and an unknown name after ``|`` ("Unknown stage or
pipeline macro 'f'"). A body that starts with ``from`` is a parse error.

A parameter binds only where the body has an expression. A stage field that
takes a literal or a ``$param`` (``take n``, an output name, ``as name``)
does not take a macro parameter. A pipeline macro cannot read another row
set: use ``let`` for that. An error inside an expanded body points at the
call. A View query sees all three scopes; a single filter
(``Query``, a DataFrame mask, ``dftracer_stats --duql``) has no record schema
and sees the query and the path files.

Matching
~~~~~~~~

Keys match by duql value: numbers by value (``4`` equals ``4.0``, and an
integer past 2^53 is exact), strings by their bytes, booleans apart.
``"4"`` never equals ``4``. A missing or null key part matches nothing.

When several rows of ``s`` match, an arrow compared in a condition (a
``where``) holds when any matching row makes it hold. In ``select``,
``derive``, ``group`` keys, aggregates and ``lookup``, matching rows with
different values are a query error that names the row set and the key;
equal values give that value. ``lookup ... into`` never fails on several
matches.

Correlated sub-queries
~~~~~~~~~~~~~~~~~~~~~~

A sub-query, under ``in`` or in an expression, may compare its own fields
with the row around it. Write each comparison as a term of a ``where`` joined
by ``and``, with ``^.`` naming the enclosing row on one side of ``==``:

.. code-block:: text

   where dur > (from data | where name == ^.name | agg { m = mean(dur) })
   derive n = (from data | where run == ^.run and host == ^.host
               | agg { c = count() })
   where g not in (from data | where run == ^.run and dur > 50 | select g)

The result is what the sub-query gives when it runs for each row with ``^.``
bound to that row. It runs once, over every key, and each row reads the
rows of its own keys. Either side of ``==`` may hold the enclosing row, and
both sides may be expressions; they match as keys do (see `Matching`_). A row
whose key is missing or null has no matching rows: ``in`` gives FALSE and
``not in`` TRUE (unknown only when the tested value is missing or null), a
scalar over ``agg`` gives what the aggregate gives over no rows (``count()``
is 0, ``mean`` is null), and a scalar over ``select`` gives null. A scalar
sub-query that gives several rows for the key of a row is a query error that
names the sub-query and the key.

A correlated ``where`` may also bound one expression of the sub-query's row
by the enclosing row: ``inner < ^.x``, ``<=``, ``>``, ``>=`` (either side
first) and ``inner between ^.lo and ^.hi``, with at most one lower and one
upper bound. The sub-query then reads, for each row, the rows of its keys
whose bounded expression lies in that range:

.. code-block:: text

   derive longest_before = (from data | where ts < ^.ts | agg { m = max(dur) })
   derive recent = (from data | where pid == ^.pid
                    and ts between ^.ts - 1000 and ^.ts | agg { n = count() })
   where name == "read" and fd in (from data | where name == "write"
                                   and ts < ^.ts | select fd)

Bounds compare as ``<`` does: numbers by value across integer and float
types, strings by bytes, booleans apart. A null, missing or NaN value on
either side, or two values of different kinds, selects no row. Such a
sub-query ends, in an expression, in ``agg`` with one of ``count``,
``count_if``, ``sum``, ``min``, ``max`` or ``mean``, or in a one-column
``select``; under ``in``, in ``select`` or in ``group`` without aggregates.
The types are those of ``agg``: counts are integers, ``sum`` of integers is
an exact integer (a sum past int64 is a query error), ``sum`` of floats and
``mean`` are floats, ``min`` and ``max`` keep the column's type. ``sum`` and
``mean`` read a typed number column; cast an untyped (JSON) column with
``int()`` or ``float()``. Each row's answer comes from a sorted index of the
side found by binary search, never from a scan per row.

After the first correlated ``where`` a sub-query holds only ``where`` and
``derive`` stages and a last ``select``, ``group`` (under ``in``) or
``agg``. These are compile errors that name what to write instead:

- ``^.`` in a term that is neither a key nor a bound, a term that mixes the
  enclosing row with the sub-query's own fields on one side, bounds on two
  expressions (an interval: use ``lookup ... overlap``), a second lower or
  upper bound, ``not between``, ``^.`` in a stage other than ``where``, and
  a term comparing two enclosing fields;
- over a range, an aggregate other than the six above, and ``agg`` or a
  ``group`` with aggregates under ``in``;
- ``take``, ``sort``, ``distinct``, ``window`` or any other stage after the
  correlated ``where``, and a sub-query that does not end in ``select``,
  ``group`` or ``agg``.

``^.`` inside ``any()`` and ``all()`` still names the quantified record. The
side is the whole sub-query over every key; see `Limits`_ for its size. A
correlated ``in`` adds no key set to the scan, since its keys are tuples. A
mixed-type key (JSON column) keeps its value type.

Limits
~~~~~~

The side of an ``asof`` or ``overlap`` lookup, of a range-correlated
sub-query, of an arrow and of an uncorrelated scalar sub-query is collected
in memory, so it is small by contract. A side over ``DUQL_LOOKUP_MAX_ROWS``
rows (default 1,000,000) or ``DUQL_LOOKUP_MAX_BYTES`` bytes (default
256 MiB) fails the query with an error that names the side and both numbers.
It never gives a truncated answer.

The other sides have no such cap:

- A top-level ``and`` term ``k in (...)`` of the leading ``where`` reads only
  the distinct rows of its side, before the scan, and filters the scan with
  them, so export (``sink_json``), pruning and scan statistics work as for a
  literal filter. The distinct rows must fit the View's memory budget
  (``View::memory_budget``, about a third of the free memory by default);
  past it the query fails naming the side and the budget. To join a larger
  side, write the test under ``not`` or in a later ``where``.
- Any other ``in``, ``not in``, and a sub-query correlated by ``==`` keys,
  run after the scan through the engine's join, as does the side of a plain
  or ``into`` ``lookup``. The join keeps the right side in memory up to the
  memory budget. Over it, both sides are split by a hash of the key into
  partitions on disk, and a partition that is still over the budget splits
  again. Rows with equal keys, including a number of two types, meet in one
  partition. The row order of a spilled join is not the order of the rows,
  and a side too large for the caps is not stored in the lookup cache.

A stage that needs all its input rows at once runs on one collected frame:
``pivot``, ``bucket ... fill``, the partition aggregates of ``group`` and the
``overlap`` lookup. The plan below it spills past the memory budget, but the
frame the stage reads is then whole in memory, so its size is bounded by the
rows that reach the stage, not by the budget. Put a ``where``, a ``group`` or a
``select`` before such a stage to keep that small.

A term that joins runs after the scan, so the query cannot be exported with
``sink_json``; the other terms of its ``where`` still filter the scan.

Key-set pushdown
~~~~~~~~~~~~~~~~

A semi-join, or an arrow compared with a value, that is a top-level ``and``
term of the leading ``where`` adds ``k in {keys of the side}`` to the scan
filter when it runs, so bloom and min/max pruning apply to ``k``. A
semi-join's term is replaced by its key set, which it equals there, so the
scan keeps its fast paths for literal filters. For an arrow
whose condition reads only the side's value, only the keys whose rows meet
it are added. Nothing is added under ``not``, ``or``, ``is`` or ``??``, or
for a key that is not a whole record field. Above 4096 keys nothing is
added, since such a list prunes no chunk. Results are the same with
pushdown and without.

Lookup cache
~~~~~~~~~~~~

A side is stored in a RocksDB at ``.dftindex-cache/lookups`` beside the
single index of the files it reads, keyed by its text with parameters
bound, the duql version, the record schema and the index record of each
file. A file re-indexed with new records gives a new key. The store keeps
at most ``DFTRACER_CACHE_MAX_BYTES``, evicting the least recently used
sides. Deleting ``.dftindex-cache`` loses nothing but time. A side over
files that share no index, or whose index is not fresh, is not cached.

A plan that reads a lookup uses no rollup or view store.

Errors
------

A syntax error names the line and column and marks the token:

.. code-block:: text

   duql parse error at line 1, column 11:
     where a ==
                ^
     Unexpected end of query
