:description: Coordinate ranks with a Communicator: groups, collectives, channels, object calls, bulk transfers, deadlines, counters, traces and fault injection, on one process or under MPI.

Coordinate ranks with a Communicator
====================================

.. admonition:: Goal

   Make several ranks work together: agree on a value, exchange data and share memory.
   Write the code once.
   Run it as threads of one process or as MPI ranks.

A rank is one participant.
A ``Communicator`` holds all ranks of one run.
A ``Group`` is a set of ranks that make the same calls in the same order.
Every call returns an ``Op<T>``, a handle that you can test, wait for, cancel or ``co_await``.

The headers are in ``dftracer/utils/comm/``, and :doc:`../../cpp_api/comm` lists them.
The programs in this guide are in ``examples/comm/``.
``ctest --preset tests -R examples/comm`` runs them, and the code on this page is quoted from those files.

.. code-block:: cpp

   #include <dftracer/utils/comm/channel.h>
   #include <dftracer/utils/comm/group.h>

   using namespace dftracer::utils;

Start a communicator
--------------------

``Communicator::start_local(ranks, executor)`` runs every rank as a coroutine of this process.
Each rank gets its own ``Group`` handle from ``world(rank)``.
Run the ranks together, because a collective ends only when all ranks made the call.

.. literalinclude:: ../../../../examples/comm/comm_basics.cpp
   :language: cpp
   :start-after: // [start]
   :end-before: // [start]
   :dedent: 4

This helper runs one coroutine for every rank:

.. literalinclude:: ../../../../examples/comm/comm_basics.cpp
   :language: cpp
   :start-after: // [run_ranks]
   :end-before: // [run_ranks]

Groups and split
----------------

``world(rank)`` is the group of all ranks.
``local(rank)`` is the group of the ranks that share a node.
``split(color, key)`` is collective.
Ranks with the same color form a new group, and ``key`` orders them.
A rank that passes ``Group::NO_COLOR`` gets an empty group.

.. literalinclude:: ../../../../examples/comm/comm_basics.cpp
   :language: cpp
   :start-after: // [groups]
   :end-before: // [groups]

Collectives
-----------

Every rank must call the same collective in the same order.
A rank that calls another one gets an error.
It does not wait.

- ``barrier`` and ``monitored_barrier``.
  The second one names the ranks that did not arrive when its deadline passes.
- ``broadcast(root, value)``, ``scatter`` and ``gather``.
- ``all_gather`` gives one buffer and one view per rank.
- ``reduce`` and ``all_reduce`` with ``ReduceOp::Sum``, ``Min`` or ``Max``, or with your own merge function.
- ``reduce_scatter``, ``all_to_all``, ``exchange_by``, ``scan``, ``exclusive_scan`` and ``neighbors``.

A reduction merges the values in rank order.
A ``double`` sum has the same bits on every rank and on every run.
Set ``CallOpts::reduce_order`` to ``ReduceOrder::Any`` to merge in arrival order.
That uses less memory, but a float result can change between runs.

.. literalinclude:: ../../../../examples/comm/comm_basics.cpp
   :language: cpp
   :start-after: // [collectives]
   :end-before: // [collectives]

Channels
--------

A channel is a named stream of values between ranks.
Both ends open the same name.
A channel carries a trivially copyable type of fixed size, or ``dstd::Vector<std::byte>`` for bytes of any size.
The two ends check the name and the size at the first message.
A mismatch is an error that names the channel and both ranks.

The messages of one sender on one channel arrive in the order they were sent.
A channel and a collective are independent: nothing orders one against the other.

``ChannelOpts`` gives the depth of the stream and what a send does when it is full.
The depth is never unbounded.

- ``Overflow::Block``: the send waits for a receive.
- ``Overflow::Fail``: the send fails at once with ``ChannelError::Full``.
- ``Overflow::DropOldest``: the oldest message is dropped.

``recv_from(peer)``, ``recv`` (any sender) and ``recv_in_rank_order`` read messages.
``recv_into(buffer)`` reads bytes into your own buffer.
A buffer that is too short fails with ``ChannelError::TooBig`` and the message stays first in the stream.
``close()`` ends the sends.
A receive drains what is queued and then fails with ``ChannelError::Closed``.

.. literalinclude:: ../../../../examples/comm/comm_basics.cpp
   :language: cpp
   :start-after: // [channels]
   :end-before: // [channels]

Objects and the codec rule
--------------------------

A value that is not trivially copyable, such as a string, uses the ``_object`` calls:
``all_gather_object``, ``gather_object``, ``broadcast_object`` and ``all_to_all_object``.
You give a ``Codec`` that says how to serialize the value.
``StringCodec`` and ``VectorCodec<T>`` are ready to use.

The rule is made for each pair of ranks:

- Two ranks in one process share one object.
  There is no copy and no codec call.
  The result is a ``SharedPtr<const T>``, so copy it to change it.
- A rank in another process gets the bytes.
  The value is serialized once, and each process deserializes it once.
  A codec must be deterministic.
  A value that does not decode aborts the group, because the ranks no longer agree.

.. literalinclude:: ../../../../examples/comm/comm_basics.cpp
   :language: cpp
   :start-after: // [object]
   :end-before: // [object]

Bulk transfers
--------------

A rank can let its peers read and write one buffer of its memory.

- ``expose(memory, access)`` gives a ``BufferRef``.
  It is not collective.
  The descriptor holds no pointer, so send it with a channel or a collective.
- ``pull(ref, dst, offset)`` copies the exposure into ``dst``.
- ``push(ref, src, offset)`` copies ``src`` into the exposure.
- ``unexpose(ref)`` closes it.
  Its ``Op`` ends when no transfer is left.
  Keep the memory valid until then.

``Access::ReadOnly`` refuses a push.
``Access::WriteOnly`` refuses a pull.
Only host memory works.
A call that names a device kind fails with ``Condition::Unsupported``.
A buffer that is too short fails with ``ChannelError::TooBig``.
No call cuts a range silently.
Both calls give the byte count.
Between processes, a large transfer moves in ranges.
The pulling rank grants credits from its memory budget, so a transfer of any size streams.

.. literalinclude:: ../../../../examples/comm/comm_basics.cpp
   :language: cpp
   :start-after: // [bulk]
   :end-before: // [bulk]

Deadlines and abort
-------------------

Every call takes a ``CallOpts``.
Set ``deadline_ns`` to a time on the steady clock, in nanoseconds.
Set ``stop`` to a ``StopToken``.
A token that is already stopped cancels the call before it starts.

What a failure does depends on the call:

- A deadline, a cancel or a stop on a collective aborts the whole group.
  Every pending and later call of the group, on every rank, ends with one error.
  The error names the rank that caused it.
  The peers already matched the call, so the group cannot go on without it.
- A deadline, a cancel or a stop on a channel call or a bulk transfer ends that call alone.
  The group stays usable.
- The abort of a group, or a rank that dies, ends all channel and bulk calls of the group.
- ``Op::cancel()`` returns at once.
  The call still ends exactly once, with its value, ``Cancelled`` or another error.
  A call that already ended keeps its outcome.

A group that is aborted cannot make new collectives.
Start a new communicator to go on, as this example does.

.. literalinclude:: ../../../../examples/comm/comm_basics.cpp
   :language: cpp
   :start-after: // [deadline]
   :end-before: // [deadline]

The error of that run names the call, how long it waited, and the ranks that arrived and the ranks that did not.

Counters and pending calls
--------------------------

``Communicator::stats()`` gives one row for each group and kind of call.
A row has the count of calls that started, ended well, failed and were cancelled, and the bytes sent and received.
``Communicator::pending()`` lists the collectives that did not end yet, each with its group, kind, sequence number, rank and age.
Use it to see which rank a hung run waits for.

.. literalinclude:: ../../../../examples/comm/comm_basics.cpp
   :language: cpp
   :start-after: // [stats]
   :end-before: // [stats]
   :dedent: 4

Trace the calls
---------------

Set ``DFTRACER_UTILS_COMM_TRACE`` to a directory before the communicator starts.
Each rank writes ``comm-<rank>-<pid>-<n>.pfw`` there when the communicator ends.
The file has one DFTracer event for each call that ended.
The event has ``cat`` ``comm``, the kind of call as ``name``, and the group, sequence number, bytes and status in ``args``.
The tools in this repository read it as any other ``.pfw`` trace.
When the variable is not set, a call pays one branch.

.. code-block:: bash

   mkdir -p /tmp/comm-trace
   DFTRACER_UTILS_COMM_TRACE=/tmp/comm-trace build/build-tests/src/comm_basics
   ls /tmp/comm-trace

Inject faults in tests
----------------------

``FaultMessaging`` wraps a messaging provider.
By rule, it delays, reorders or drops messages, and it can kill a rank.
The same ``seed`` gives the same decisions for the same messages.
One stream of one sender and one receiver stays in order, so a reorder shows only between streams.
Put it under the collectives, and a barrier, a channel and a bulk transfer all pass through the rules.

.. literalinclude:: ../../../../examples/comm/comm_fault.cpp
   :language: cpp
   :start-after: // [start]
   :end-before: // [start]

A delay rule slows every message and the barrier still ends:

.. literalinclude:: ../../../../examples/comm/comm_fault.cpp
   :language: cpp
   :start-after: // [delay]
   :end-before: // [delay]
   :dedent: 4

A killed rank fails every pending call of its groups with the same error, which names the rank.
A lost message is lost.
The call that waits for it ends by its deadline, and the error names the missing rank.

.. literalinclude:: ../../../../examples/comm/comm_fault.cpp
   :language: cpp
   :start-after: // [kill]
   :end-before: // [kill]
   :dedent: 4

Run the same code under MPI
---------------------------

``Communicator::start(BackendKind)`` starts one rank for this process.
``BackendKind::Local`` is one local rank.
``Mpi`` loads ``libdftracer_utils_comm_mpi`` at run time and gives one rank for each MPI process.
``Auto`` picks ``Mpi`` under an MPI or PMIx launcher and ``Local`` otherwise.
If a launcher started the process and the library is missing, ``Auto`` fails.
It does not run copies that cannot talk to each other.

.. literalinclude:: ../../../../examples/comm/comm_rank_report.cpp
   :language: cpp
   :start-after: // [start]
   :end-before: // [start]

The code that follows does not change:

.. literalinclude:: ../../../../examples/comm/comm_rank_report.cpp
   :language: cpp
   :start-after: // [sum]
   :end-before: // [sum]

The tools take ``--comm local|mpi|auto``.
``DFTRACER_UTILS_COMM`` does the same, and the option wins.
Use ``DFTRACER_UTILS_COMM_PATH`` to name the one directory that holds the MPI library.
The library must come from the same build as the comm library.
Build it with ``-DDFTRACER_UTILS_ENABLE_MPI=ON``.
:doc:`../scale/mpi` shows how to build and launch.

.. code-block:: bash

   build/build-tests/src/comm_rank_report local
   # in a build made with -DDFTRACER_UTILS_ENABLE_MPI=ON
   mpirun -n 4 <build-dir>/src/comm_rank_report mpi

A rank cannot cancel a native MPI request.
A call that ends early fails at once with the error of its group.
If a peer never matches a native call, the job ends with ``MPI_Abort`` and a non-zero status.
It does not hang.

See also
--------

- :doc:`../../concepts/architecture` for how comm is built: the provider seams, the backends and the rules above them.
- :doc:`../scale/mpi` to build and launch under MPI.
- :doc:`../../environment` for ``DFTRACER_UTILS_COMM``, ``DFTRACER_UTILS_COMM_PATH``, ``DFTRACER_UTILS_COMM_TRACE`` and ``DFTRACER_UTILS_COMM_MPI_CHUNK_BYTES``.
- :doc:`../plugins/inter-plugin-comms` to share data between plugins in one scan.
