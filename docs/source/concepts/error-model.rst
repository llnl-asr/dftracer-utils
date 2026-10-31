:description: Why errors carry a (domain, code) identity instead of one global enum, and when the codebase uses a value result versus an exception.

The error model
================

What this explains: why dftracer-utils errors carry a ``(domain, code)`` identity instead of one global enum, and when the codebase uses a value result versus an exception.

The problem with one flat error enum
------------------------------------

One global error enum forces every subsystem to either reuse a few coarse categories that lose information, or add cases to a header that it does not own.
Then every new subsystem's errors are a merge conflict away from every other subsystem's.
The error type is built so that a subsystem defines its own codes in its own header, without editing anything outside it.
Every caller still gets a small, closed set of categories that it can switch on without knowing those codes.

Domain, code, and condition
---------------------------

A ``dstd::Error`` (``dstd/error.h``) is 16 bytes and carries three things and an optional message.

- ``domain()`` identifies the subsystem that raised it, as an FNV-1a hash of a readable name (``ErrorDomain``, built with ``make_error_domain``).
  The hash is the identity used for matching and in the C ABI.
  The name rides along only because a hash cannot be printed back.
  Every name starts with ``dftracer.utils.``, such as ``dftracer.utils.core`` and ``dftracer.utils.duql``.
- ``code()`` is that subsystem's own enum value, which means something only together with its domain.
  The duql library's ``DuqlErrc::Pattern`` and another subsystem's code ``1`` are unrelated even when their numbers match.
- ``condition()`` is a ``dstd::Condition``, a small, closed, cross-domain enum (``Unknown``, ``Internal``, ``InvalidArgument``, ``NotFound``, ``Io``, ``Parse``, ``Compression``, ``Timeout``, ``Unsupported``, ``Cancelled``).
  A caller that does not know which subsystem failed can still switch on it.
- ``message()`` is the readable text.
  An error with no message takes no heap memory, and copies of an error with a message share one reference-counted block.

``is(Condition)`` matches the category, and ``is_code(code)`` matches one exact code of one domain.

A subsystem opts in with a ``constexpr`` table of ``ErrorCodeInfo`` entries (domain, code, condition and name), one per code, and an ``error_info`` function that ADL finds in the enum's own namespace:

.. code-block:: cpp

   inline constexpr dstd::ErrorDomain ERROR_DOMAIN =
       dstd::make_error_domain("dftracer.utils.duql");

   enum class DuqlErrc : std::int32_t { Parse, Pattern, Unsupported };

   inline constexpr dstd::ErrorCodeInfo DUQL_ERROR_INFOS[] = {
       {&ERROR_DOMAIN, 0, dstd::Condition::Parse, "PARSE"},
       {&ERROR_DOMAIN, 1, dstd::Condition::InvalidArgument, "PATTERN"},
       {&ERROR_DOMAIN, 2, dstd::Condition::Unsupported, "UNSUPPORTED"},
   };

   constexpr const dstd::ErrorCodeInfo& error_info(DuqlErrc code) noexcept {
       return DUQL_ERROR_INFOS[static_cast<std::size_t>(code)];
   }

``make_error(DuqlErrc::Pattern, "...")`` then finds the domain and the condition from the enum type alone.
The ``dstd::ErrorEnum`` concept checks that ``error_info`` exists, so a code of one subsystem cannot be attached to the domain of another, and the compiler checks it.
An error holds a pointer to its table entry, so making one costs no lookup.
``duql/errc.h`` is the worked example, and ``ErrorCode`` in ``common/error.h`` is the core domain's table.

.. mermaid::

   graph LR
       Enum["Subsystem enum<br/>(such as DuqlErrc)"] --> Info["error_info(e)<br/>-> ErrorCodeInfo"]
       Info --> Domain["domain"]
       Info --> Cond["condition"]
       Info --> Error["dstd::Error{entry, message}"]

Values versus exceptions
------------------------

Two channels carry an error, chosen by whether the immediate caller is expected to handle the failure.

- ``Result<T>`` (``dstd::Expected<T, dstd::Error>``) is the channel for a failure in the ordinary course of use, such as a query that does not parse or a key that is not found.
  The function returns it, and the caller checks it.
  ``make_error(code, message)`` builds the error side, and ``DFTU_TRY`` propagates it in a coroutine.
- ``DFTUtilsException`` is for the case the caller cannot handle.
  It is thrown, not returned, and it holds a ``dstd::Error``.
  It crosses the Python boundary with its ``code()``, ``domain()`` and ``condition()`` intact, so a ``try`` and ``except`` in Python can branch on the same identity as a C++ caller.

Keeping exceptions for the failures that cannot be handled keeps the common failure paths (a malformed query, a missing file) as ordinary returned values.
A coroutine checks such a value with a plain ``if`` before it decides whether to go on, so no exception has to survive a suspension point on that path.

See also
--------

- :doc:`../cpp_api/runtime` for the generated API reference.
- :doc:`coroutine-caveats` for the coroutine lifetime rules this codebase is otherwise careful about.
- :doc:`architecture` for how the domain layers relate to each other.
