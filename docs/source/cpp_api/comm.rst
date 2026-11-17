:description: Reference for the C++ communicator: Communicator, Group, Op, channels, collectives, the codec, bulk transfers, call counters and the backend providers.

Communicator
============

.. seealso::

   :doc:`../guides/runtime/communicator` for how to use it, and
   :doc:`../concepts/architecture` for how it is built.

The communicator in ``dftracer::utils::comm``: ``Communicator`` starts a run and makes ``Group`` handles, and every call of a group returns an ``Op`` that ends exactly once.
``Channel`` carries values between ranks, the ``Codec`` concept moves objects that are not trivially copyable, and ``BufferRef`` names memory that a rank exposed for ``pull`` and ``push``.
``CallStats`` and ``PendingCall`` report what the calls did.

.. include:: /cpp_api/_generated/comm.rst.inc

Backend providers
-----------------

A backend gives the communicator four providers behind small interfaces: ``Bootstrap``, ``Messaging``, ``Collectives`` and ``Bulk``, in ``dftracer::utils::comm::backend``.
``FaultMessaging`` wraps any ``Messaging`` to delay, reorder, drop or kill by seeded rule, for tests.

.. include:: /cpp_api/_generated/comm.backend.rst.inc
