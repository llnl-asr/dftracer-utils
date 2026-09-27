:description: Why errors carry a (domain, code) identity instead of one global enum, and when the codebase uses a value result versus an exception.

The error model
================

What this explains: why dftracer-utils errors carry a ``(domain, code)``
identity instead of one global enum, and when the codebase uses a value
result versus an exception.

The problem with one flat error enum
------------------------------------------

A single global ``ErrorCode`` enum (``core/common/error.h`` still has one,
``ErrorCode``, kept for compatibility) forces every subsystem to either reuse
a handful of coarse categories that lose information, or keep adding cases to
a header nothing but that header owns - so every new subsystem's errors are a
merge conflict away from every other subsystem's. The extensible identity
that replaces it as the primary type, ``Error``, is built to let a subsystem
define its own error codes in its own header, without editing anything
outside it, while still giving every caller a small, closed set of
categories it can switch on without knowing those subsystem-specific codes.

Domain, code, and condition
--------------------------------

An ``Error`` (``core/common/error.h``) carries three things:

- ``domain`` identifies which subsystem raised it, as an FNV-1a hash of a
  human-readable name (``ErrorDomain``, built with ``make_error_domain``).
  The hash is the identity used for matching and for the C ABI; the name
  rides along only because a hash cannot be printed back.
- ``code`` is that subsystem's own enum value, meaningful only paired
  with its domain - the duql library's ``DuqlErrc::Pattern`` and some other
  subsystem's code ``1`` are unrelated even if their integer values collide.
- ``condition`` is a ``Condition``, a small, closed, cross-domain enum
  (``Unknown``, ``Internal``, ``InvalidArgument``, ``NotFound``, ``Io``,
  ``Parse``, ``Compression``, ``Timeout``, ``Unsupported``, ``Cancelled``). A
  caller that does not know or care which subsystem failed can still
  exhaustively switch on ``condition`` and get a sensible answer.

A subsystem opts in by defining, in its own namespace (found by ADL), two
functions for its error enum:

.. code-block:: cpp

   enum class DuqlErrc : std::int32_t { Parse, Pattern, Unsupported };

   constexpr ErrorDomain error_domain(DuqlErrc) noexcept {
       return ERROR_DOMAIN;  // this subsystem's domain, defined once
   }
   constexpr Condition error_condition(DuqlErrc e) noexcept {
       switch (e) {
           case DuqlErrc::Parse:       return Condition::Parse;
           case DuqlErrc::Pattern:     return Condition::InvalidArgument;
           case DuqlErrc::Unsupported: return Condition::Unsupported;
       }
   }

``make_error(DuqlErrc::Pattern, "...")`` then deduces both the domain and
the condition from the enum type alone (the ``ErrorEnum`` concept checks that
both functions exist), so a code from one subsystem cannot accidentally be
attached to another subsystem's domain, and the association is checked at
compile time rather than by convention. This is the pattern each subsystem
that wants its own error codes follows - see ``duql/errc.h`` for the
duql library's domain as a worked example.

.. mermaid::

   graph LR
       Enum["Subsystem enum<br/>(e.g. DuqlErrc)"] --> Domain["error_domain(e)<br/>-> ErrorDomain"]
       Enum --> Cond["error_condition(e)<br/>-> Condition"]
       Domain --> Error["Error{domain, code, condition, message}"]
       Cond --> Error

Values versus exceptions
------------------------------

Two channels carry an error, chosen by whether the failure is expected to be
handled by the immediate caller:

- ``ErrorOr<T>`` (``expected<T, Error>``) is the recoverable-failure
  channel: a function that can fail in the ordinary course of use - a query
  that does not parse, a key that is not found - returns ``ErrorOr<T>`` and
  the caller is expected to check it. ``Result<T>`` is the older,
  ``DFTUtilsError``-based equivalent, kept for code not yet migrated to the
  extensible identity.
- ``DFTUtilsException`` is for the unrecoverable case: it is thrown, not
  returned, and it is what crosses the Python boundary (where an exception is
  the natural error channel) with its ``code()``/``domain``/``condition``
  intact so a ``try``/``except`` on the Python side can still branch on the
  same identity a C++ caller would.

Reserving exceptions for the genuinely unrecoverable, and using a value
result everywhere a caller is expected to branch on failure, keeps the
common failure paths (a malformed query, a missing file) as an ordinary
returned value that a coroutine can check with a plain ``if`` before
deciding whether to keep going or propagate - no exception has to survive a
suspension point on that path at all.

See also
--------

- :doc:`../cpp_api/runtime` for the generated API reference.
- :doc:`coroutine-caveats` for the coroutine lifetime rules this codebase is
  otherwise careful about.
- :doc:`architecture` for how the domain layers relate to each other.
