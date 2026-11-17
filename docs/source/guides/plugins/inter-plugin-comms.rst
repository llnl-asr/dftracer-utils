:description: Have plugins share data within one scan by publishing and consuming batch-scoped ports.

How to communicate between plugins
====================================

.. admonition:: Goal
   :class: goal

   Have one plugin publish a value during a batch and another plugin
   consume it, within the one shared scan ``--plugin a --plugin b`` already
   runs. See :doc:`../../plugins` for the plugin model this builds on.

**Ports** (``DFTU_SVC_PORTS``) are the one mechanism: a producer publishes a
value during a batch and a consumer reads it back in the same batch. A port is
a name, and the two ends are wired by naming the same one. The ``dftu.``
prefix is reserved for the host; a port that ever needs a version carries it
in the name.

Ports: publish and consume
----------------------------

A port is a batch-scoped slot, identified by its name, that resets between
batches. A producer writes to it during the batch fold; a
consumer reads it back during the same batch.

.. tab-set::

   .. tab-item:: C++ (SDK)

      The ``Host`` wrapper (``dftracer/utils/plugins/plugin.h``) exposes ports
      as a typed pair, ``publish_port<T>``/``consume_port<T>``, built once from
      a port name and used every batch through ``send``/``recv``:

      .. code-block:: cpp

         template <class T> OutPort<T> publish_port(dstd::CStringView cap_id) const;
         template <class T> InPort<T> consume_port(dstd::CStringView cap_id) const;

         // OutPort<T>: void send(const T& v) const;
         // InPort<T>:  dstd::Optional<T> recv() const;

      ``T`` must be trivially copyable; the host copies ``sizeof(T)`` bytes per
      publish. A producer built with ``dftracer::utils::plugins::make_plugin<Slice>``
      publishes from its ``step`` and names the port in a static ``provides()``:

      .. code-block:: cpp

         #include <dftracer/utils/plugins/plugin.h>

         constexpr const char* PORT_CAP = "com.example.perbatch";

         struct ProducerSlice {
             static auto provides() {
                 return std::array<const char*, 1>{PORT_CAP};
             }

             explicit ProducerSlice(const dftracer::utils::plugins::Config&) {}
             void step(dataframe::FrameView b, dftracer::utils::plugins::Host h) {
                 std::uint64_t with_dur = 0;
                 if (const auto dur = b.column("dur")) {
                     for (std::int64_t row = 0; row < b.row_count(); ++row)
                         if (!dur->is_null(row)) ++with_dur;
                 }
                 h.publish_port<std::uint64_t>(PORT_CAP).send(with_dur);
             }
             void merge(ProducerSlice&) {}
             void finalize(dftracer::utils::plugins::Host) {}
         };

         DFTU_PLUGIN("producer", "1.0",
                     [](dftracer::utils::plugins::PluginBuilder& p,
                        const dftracer::utils::plugins::Config& config)
                         -> dftracer::utils::dstd::Result<void> {
                         (void)config;
                         p.fold<ProducerSlice>();
                         return {};
                     })

      A consumer reads it back with the same port name, named in its own
      static ``consumes()`` so the host runs the producer first:

      .. code-block:: cpp

         struct ConsumerSlice {
             static auto consumes() {
                 return std::array<const char*, 1>{PORT_CAP};
             }

             explicit ConsumerSlice(const dftracer::utils::plugins::Config&) {}
             void step(dataframe::FrameView, dftracer::utils::plugins::Host h) {
                 if (auto with_dur = h.consume_port<std::uint64_t>(PORT_CAP).recv()) {
                     // ... use *with_dur for this batch
                 }
             }
             void merge(ConsumerSlice&) {}
             void finalize(dftracer::utils::plugins::Host) {}
         };

   .. tab-item:: C (raw ABI)

      A plugin fetches the raw port channel from the host through
      ``host->get_service(host->h, DFTU_SVC_PORTS)``:

      .. code-block:: c

         typedef struct dftu_svc_ports {
             uint64_t (*port_key)(void* h, const char* cap_id);
             void (*publish)(void* h, uint64_t key, const void* data, uint32_t len);
             const void* (*consume)(void* h, uint64_t key, uint32_t* out_len);
         } dftu_svc_ports;

      ``port_key`` interns a port name once (cache the returned
      ``uint64_t``); ``publish`` copies ``len`` bytes into the batch-scoped
      slot; ``consume`` returns a borrowed pointer valid only until the
      current ``on_batch`` returns (NULL if nothing was published this batch).
      A producer's ``on_batch`` reads the ``ph`` column to mirror
      ``Event::has_dur()`` (a duration means ``phase() == Complete``):

      .. code-block:: c

         static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                                     const dftu_dataframe* df,
                                     const dftu_plugin_host* host) {
             MyState* s = (MyState*)slice;
             const dftu_svc_ports* ports =
                 (const dftu_svc_ports*)host->get_service(host->h, DFTU_SVC_PORTS);
             (void)ctx;
             if (!s->port_key) s->port_key = ports->port_key(host->h, "com.example.perbatch");

             int64_t n = dftu_dataframe_num_rows(df);
             dftu_series* ph_col = dftu_dataframe_column(df, "ph");
             const int64_t* ph = (ph_col && dftu_series_type(ph_col) == DFTU_TYPE_INT64)
                 ? (const int64_t*)dftu_series_data(ph_col) : NULL;
             uint64_t with_dur = 0;
             int64_t i;
             if (ph)
                 for (i = 0; i < n; ++i)
                     if (ph[i] == DFTU_PH_COMPLETE) ++with_dur;
             if (ph_col) dftu_series_free(ph_col);
             ports->publish(host->h, s->port_key, &with_dur, sizeof(with_dur));
             return DFTU_OK;  /* synchronous */
         }

      and a consumer reads it back with the same key:

      .. code-block:: c

         static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                                     const dftu_dataframe* df,
                                     const dftu_plugin_host* host) {
             MyState* s = (MyState*)slice;
             const dftu_svc_ports* ports =
                 (const dftu_svc_ports*)host->get_service(host->h, DFTU_SVC_PORTS);
             (void)ctx;
             if (!s->port_key) s->port_key = ports->port_key(host->h, "com.example.perbatch");

             (void)df;
             uint32_t len = 0;
             const void* v = ports->consume(host->h, s->port_key, &len);
             if (v && len == sizeof(uint64_t)) {
                 uint64_t with_dur; memcpy(&with_dur, v, sizeof(with_dur));
                 /* ... use with_dur for this batch */
             }
             return DFTU_OK;
         }

   .. tab-item:: Python (JIT)

      A ``@jit.plugin`` class declares a port with ``jit.publish`` /
      ``jit.consume`` as a class attribute, then writes or reads it inside
      ``@jit.each_event``. A publish port accumulates a per-batch total with
      ``self.<port> += <expr>``; the host sums the per-event contributions and
      publishes the total once the batch finishes. A consume port is read as a
      plain scalar, ``self.<port>`` (``0`` when no producer has published yet):

      .. code-block:: python

         from dftracer.utils import jit

         @jit.plugin
         class Producer:
             sig = jit.publish("com.example.perbatch", of=jit.u64)

             @jit.each_event
             def step(self, e):
                 if e.has_dur:
                     self.sig += 1

         @jit.plugin
         class Consumer:
             sig = jit.consume("com.example.perbatch", of=jit.u64)

             @jit.each_event
             def step(self, e):
                 if self.sig > 0:               # a producer published this batch
                     pass                        # ... use self.sig for this batch

      ``of`` picks the wire width shared by both ends (``jit.u64`` / ``jit.i64``
      / ``jit.f64``); it must match on both the publisher and the consumer. A
      consume port is read-only and a publish port is write-only; writing to a
      consume port (or reading a publish port) raises ``JitError`` at
      decoration time. A missing producer degrades to reading ``0``. A JIT
      producer and a hand-written C++ or C consumer (or
      vice versa) interoperate freely: all three compile down to the same
      ``DFTU_SVC_PORTS`` machinery.

The ordering rule
-------------------

A consumer sees nothing published (an empty ``dstd::Optional`` in C++, NULL in
C, ``0`` in JIT) whenever the producer has not published for the current batch
- most commonly because it runs after the consumer. Name the port in the
consumer's ``consumes`` and the producer's ``provides`` (a single
NULL-terminated name array per plugin, one namespace shared with accumulator
names, section 6 of :doc:`../../plugins`): the host orders the fold from that
declaration, running every provider of a consumed name first, and refuses to
run the set at all if no loaded plugin provides it. There is no ``--plugin``
flag order to get right.

A value published in one batch does not carry over to the next: the host
clears every port between batches, so a consumer sees nothing published until
that batch's producer publishes again.

In C++, ``recv()`` copies the published bytes into the returned
``dstd::Optional<T>``, so the result is safe to keep past the current
``on_batch``/``step`` call without any manual copying. In C, ``consume``
returns a pointer that is only valid until ``on_batch`` returns; copy it out
if you need it longer.

Rewriting the batch for the plugins after you
-----------------------------------------------

A port carries a value beside the batch. To change the batch itself, so
that every plugin after yours reads your rows instead of the scanned ones,
give the plugin a ``transform`` in place of ``on_batch``:

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         struct Sessionize {
             explicit Sessionize(const Config&) {}
             // The rewritten batch (the host frees it), or an empty frame for
             // "unchanged". Takes the place of step().
             dstd::Result<OwnedDataFrame> transform(dataframe::FrameView df,
                                                    Host h);
             void merge(Sessionize&) {}
             void finalize(Host) {}
             static auto provides() { return std::array{"sessions.batch"}; }
         };

   .. tab-item:: C

      .. code-block:: c

         /* dftu_status (ctx, slice, df, host, dftu_dataframe** out); NULL in *out
            means "unchanged" */
         g_plugin.transform = sessionize;

The rewrite may drop rows, add rows, and add, drop or change columns. A
transform sees the whole batch (its ``reads`` is ignored); a plugin after it
gets its own ``reads`` projection of the rewritten frame, by name, and its
``plan_duql`` is applied again to the rewritten rows. A plugin before it in
fold order still sees the scan as it was. Order it the same way as a port:
a plugin that must see the rewrite ``consumes`` a name the transform
``provides``. Two transforms chain, each seeing the previous one's output.

Channels between stages
-----------------------

A port lives for one batch. When a plugin needs a stream that outlives a
batch, declare a channel in the plugin factory with ``Ports::open`` (the C ABI
``channel_open``). A bare name is yours, and the host prefixes it with
``plugin.<your name>.``. The plugin that reads it opens
``plugin.<owner>.<name>`` with the same credits, overflow mode and message
size. A ``Block`` channel adds an edge to the order graph from each plugin that
sets ``sends`` to each plugin that sets ``receives``, so the cycle rule covers
blocking channels. ``Fail`` and ``DropOldest`` channels never wait and add no
edge, so a request channel and a response channel between two plugins are
fine.

.. code-block:: cpp

   // factory of "owner"
   plugins::ChannelOptions options;
   options.credits = 8;
   options.overflow = plugins::Overflow::DropOldest;  // never a silent default
   options.max_msg_bytes = sizeof(Reading);
   DFTRACER_UTILS_TRY(plugins::ChannelId id,
                      host.ports().open("readings", options));

   // a task of "reader"
   plugins::Channel<Reading> readings(h.ports(), id);
   auto op = readings.recv();
   dstd::Result<Reading> got = co_await op;

Pick the overflow mode on purpose. ``Fail`` reports ``PortError::Full``;
``DropOldest`` keeps the newest messages; ``Block`` makes the sender wait for
a credit, and the host accepts it only when every end sets ``task_end``, so
that no end runs in the fused ``on_batch`` loop, and ``sends`` or ``receives``.
A ``Block`` send that starts from ``on_batch`` anyway fails at once with an
error that names the channel. ``Channel<T>::recv`` drops a message whose size
is not ``sizeof(T)`` and gives ``PortError::TooBig`` with both sizes, so one
wrong message never blocks the channel. For a value that many readers
poll, use a latest port (``latest_set``, ``latest_get``); it is a hint, and
``NotReady`` means nothing was set yet. To merge a value over all slices, use
a phase result: declare each generation in the factory, ``phase_contribute``
from the slices, ``phase_seal`` the generation, and read it with
``phase_result``. See :doc:`../../plugins` for the full list of rules.

See also
--------

- :doc:`../../plugins` for the plugin lifecycle (``make_slice`` /
  ``on_batch`` / ``merge`` / ``on_finalize``) these channels plug into.
- :doc:`../../jit` for the full JIT walkthrough, including ports and
  cross-worker shared handles.
- :doc:`../../cpp_api/plugins` for the full C ABI reference.
