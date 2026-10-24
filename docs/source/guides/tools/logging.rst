:description: Set the native log level and color from Python, the CLI, or C++; one shared logger governs verbosity across the whole process.

Control log output
===================

.. admonition:: Goal
   :class: goal

   Set how verbose dftracer-utils is, and whether its log lines are
   colored, from Python, the CLI, or C++. Every layer (the C++ core, the CLI
   binaries, and the Python extension) shares one logger, so a level set from
   any of them affects the whole process.

Levels are, from quietest to loudest suppressed: ``off`` disables all
messages; going down the list enables progressively more detail:
``trace``, ``debug``, ``info``, ``warn``, ``error``. The default level is
``info``.

From Python
-----------

.. code-block:: python

   from dftracer.utils import get_log_level, set_log_level, set_log_color

   set_log_level("debug")     # trace | debug | info | warn | error | off
   print(get_log_level())     # "debug"

   set_log_color("always")    # auto (default) | always | never

``set_log_level`` raises ``ValueError`` on an unrecognized name. There is no
per-module level; it is one global threshold for the native logger.

From the command line
----------------------

Every CLI binary (``dftracer_index``, ``dftracer_stats``, ``dftracer_server``,
and the rest) accepts ``--log-level``:

.. code-block:: bash

   dftracer_index ./traces --log-level debug

A CLI ``--log-level`` overrides the environment variable below for that
process.

Environment variables
----------------------

Set these before the process starts (native binaries, and the Python
extension on import):

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Variable
     - Effect
   * - ``DFTRACER_UTILS_LOG_LEVEL``
     - Initial log level: ``trace``, ``debug``, ``info``, ``warn``, ``error``,
       or ``off``.
   * - ``NO_COLOR``
     - Disable colored output (any value), following the `NO_COLOR
       <https://no-color.org/>`_ convention.
   * - ``FORCE_COLOR``, ``CLICOLOR_FORCE``
     - Force colored output even when stderr is not a terminal.

These are read once at logger initialization; ``set_log_level``/
``set_log_color`` (or ``--log-level``) override them afterward.

Where output goes
------------------

Log lines go to stderr by default. Color is auto-detected (on when stderr is
a terminal) unless overridden by ``NO_COLOR``/``FORCE_COLOR``/
``CLICOLOR_FORCE`` or ``set_log_color``.

The C++ API
-----------

The logger lives in ``dftracer/utils/common/logging.h``, namespace
``dftracer::utils::logger``:

.. code-block:: cpp

   #include <dftracer/utils/common/logging.h>

   using namespace dftracer::utils::logger;

   init();                    // reads DFTRACER_UTILS_LOG_LEVEL and the color env vars
   set_level(Level::Debug);   // Trace | Debug | Info | Warn | Error | Off
   set_color(ColorMode::Auto);
   Level current = get_level();

Call ``init()`` once at process startup (every ``dftracer_*`` CLI binary and
the Python extension already do this); ``set_level``/``set_color`` can be
called any time afterward to change the threshold at runtime. Application
code logs through the ``DFTRACER_UTILS_LOG_*`` macros
(``DFTRACER_UTILS_LOG_INFO(...)``, ``_DEBUG``, ``_WARN``, ``_ERROR``, and
``_TRACE``), which are gated both at compile time and at the current runtime
level.

Trace-level logging (and the coroutine auto-tracing it enables) is compiled
in by default; a minimal build can strip it entirely with
``-DDFTRACER_UTILS_LOGGER_LEVEL_TRACE=OFF`` at configure time. This only
controls whether trace calls compile to nothing - the runtime level (``off``
by default effectively, since the default level is ``info``) still gates
whether they print.

See also
--------

- :doc:`../../getting-started/index` - the build presets, including
  ``debug`` (verbose logging enabled by default).
