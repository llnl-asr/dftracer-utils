// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_PORTS_H
#define DFTRACER_UTILS_PLUGINS_ABI_PORTS_H

/** @file
 * dftu.svc.ports: batch-scoped producer -> consumer ports, bounded channels
 * between plugins, latest-value hints and phase results, lent to a plugin.
 * Optional service group, fetched via
 * dftu_plugin_host::get_service(DFTU_SVC_PORTS). Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 *
 * A payload is bytes, a fixed-size value that carries its own version, or a
 * frame. It holds no pointer and no platform-sized integer.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_PORTS "dftu.svc.ports"

/** The outcome of a port call that can refuse. */
typedef int32_t dftu_port_status;
enum {
    DFTU_PORT_OK = 0,
    DFTU_PORT_FULL = 1,     /**< no room; nothing was stored */
    DFTU_PORT_TOO_BIG = 2,  /**< the message or the buffer is too small; the
                                call reports the size it needs */
    DFTU_PORT_CLOSED = 3,   /**< the channel is closed (and drained, for a
                                receive) */
    DFTU_PORT_NOT_READY = 4 /**< nothing was published or set yet */
};

/** What a send does when the channel has no free credit. */
typedef int32_t dftu_overflow;
enum {
    /** The send waits for a credit. Allowed only when both ends declare
       DFTU_CHANNEL_F_TASK_END, so the two ends run in separately scheduled
       tasks; the host refuses the plan otherwise. */
    DFTU_OVERFLOW_BLOCK = 0,
    DFTU_OVERFLOW_FAIL = 1,       /**< the send reports DFTU_PORT_FULL */
    DFTU_OVERFLOW_DROP_OLDEST = 2 /**< the oldest message is dropped */
};

/** Bits of dftu_channel_desc::field_mask. A field whose bit is clear takes its
   default: 16 credits, DFTU_OVERFLOW_FAIL and 65536 bytes a message. */
enum {
    DFTU_CHANNEL_F_CREDITS = 1,
    DFTU_CHANNEL_F_OVERFLOW = 2,
    DFTU_CHANNEL_F_MAX_MSG_BYTES = 4,
    /** The caller touches the channel only from tasks it starts, never from
       on_batch. The plan check trusts this bit. The host also refuses, at run
       time, a DFTU_OVERFLOW_BLOCK send started from on_batch or transform, with
       an error that names the channel, so a plugin that lied does not
       deadlock. */
    DFTU_CHANNEL_F_TASK_END = 8,
    /** The declaring plugin sends on the channel. */
    DFTU_CHANNEL_F_SENDS = 16,
    /** The declaring plugin receives on the channel. A DFTU_OVERFLOW_BLOCK
       channel needs every end to set SENDS, RECEIVES or both: only a Block
       send waits, so the host orders the plugins from each sender to each
       receiver and refuses a cycle, and a Fail or DropOldest channel adds no
       order. */
    DFTU_CHANNEL_F_RECEIVES = 32
};

/** The declaration of one channel end. `name` is a logical name
   ([a-z0-9._/-], never under "dftu."). The host prefixes the name of the
   declaring plugin, `plugin.<name>.`, so a bare name is the plugin's own
   channel; another plugin that reads it names it in full,
   `plugin.<owner>.<name>`. Every end of one channel declares the same credits,
   overflow and max_msg_bytes, or the plan is refused. */
typedef struct dftu_channel_desc {
    uint32_t struct_size;   /**< sizeof(dftu_channel_desc) */
    uint32_t field_mask;
    const char* name;
    uint32_t credits;       /**< messages the channel holds */
    dftu_overflow overflow;
    uint32_t max_msg_bytes; /**< the longest message */
    uint32_t reserved;      /**< zero */
} dftu_channel_desc;

/** The named merge of a phase result. DFTU_MERGE_SUM, MIN and MAX take
   DFTU_T_I64, DFTU_T_U64 or DFTU_T_F64 and a payload of 8 bytes.
   DFTU_MERGE_UNION takes DFTU_T_U64 and a payload that is a multiple of 8
   bytes, a set of values; the result is the sorted distinct values.
   DFTU_MERGE_CONCAT takes DFTU_T_BYTES; the result is the payloads appended in
   the order the host received them, which across slices is not defined. */
typedef int32_t dftu_merge_op;
enum {
    DFTU_MERGE_SUM = 0,
    DFTU_MERGE_MIN = 1,
    DFTU_MERGE_MAX = 2,
    DFTU_MERGE_UNION = 3,
    DFTU_MERGE_CONCAT = 4
};
/** What the host seeds a phase result generation with before the scan, from
   the input files of the run; see dftu_svc_ports::phase_seed. Every seed is a
   DFTU_T_U64 scalar, 8 bytes. The values cover every non-metadata event of the
   files, whatever filter the view has. An empty input gives 0. */
typedef int32_t dftu_phase_seed;
enum {
    /** The earliest event start above 0, in the trace's time unit (events
       recorded without a clock, at ts 0, are left out). Takes DFTU_MERGE_MIN.
       */
    DFTU_PHASE_SEED_TS_MIN = 1,
    /** The latest event end, ts + dur, in the trace's time unit. Takes
       DFTU_MERGE_MAX. */
    DFTU_PHASE_SEED_TS_MAX = 2,
    /** The number of events. Takes DFTU_MERGE_SUM. */
    DFTU_PHASE_SEED_EVENT_COUNT = 3
};
/** Batch-scoped ports for intra-batch producer -> consumer communication. A
   port is a name; a producer and a consumer are wired by naming the same one,
   and a name that ever needs a version carries it in the name. The bus resets
   between batches. A consume result is borrowed until the current on_batch
   returns (copy to retain) and NULL if the producer has not published this
   batch. The host runs a producer before every consumer of its ports; that
   order comes from the plugins' declared dftu_plugin::provides /
   dftu_plugin::consumes, not from registration order.
 */
typedef struct dftu_svc_ports {
    uint32_t struct_size; /**< sizeof(dftu_svc_ports) */
    /** Stable key for the port named `name` (lowercase ASCII [a-z0-9._/-]).
       The "dftu." namespace belongs to the host and is refused to plugins. */
    uint64_t (*port_key)(void* h, const char* name);
    void (*publish)(void* h, uint64_t key, const void* data, uint32_t len);
    const void* (*consume)(void* h, uint64_t key, uint32_t* out_len);

    /** The status form of publish. Copies `len` bytes under `key` for this
       batch. *out_status is DFTU_PORT_OK, DFTU_PORT_FULL when the batch's port
       bytes are used up, DFTU_PORT_CLOSED outside a scan. */
    dftu_status (*try_publish)(dftu_ctx* ctx, void* h, uint64_t key,
                               const void* data, uint32_t len,
                               dftu_port_status* out_status);
    /** The status form of consume. Copies what was published this batch into
       `buf` (room for `cap`). *out_status is DFTU_PORT_OK,
       DFTU_PORT_NOT_READY when nothing was published, or DFTU_PORT_TOO_BIG;
       *out_len is the size in the first and the last case. */
    dftu_status (*try_consume)(dftu_ctx* ctx, void* h, uint64_t key, void* buf,
                               uint32_t cap, uint32_t* out_len,
                               dftu_port_status* out_status);

    /** Open the channel `desc` names, giving its handle in *out_channel. In a
       plugin factory this declares the end, and the host checks every end of
       the channel at plan time: a duplicate name, a mismatch of credits,
       overflow or size, a name no plugin declares, a cycle through channels,
       and DFTU_OVERFLOW_BLOCK between ends that share the fused scan. After
       the plan it gives the handle of a declared channel. */
    dftu_status (*channel_open)(dftu_ctx* ctx, void* h,
                                const dftu_channel_desc* desc,
                                uint64_t* out_channel);
    /** Start a send of `len` bytes. The host copies them before the task
       completes. The task completes with DFTU_OK once *out_status is set:
       DFTU_PORT_OK, DFTU_PORT_FULL (DFTU_OVERFLOW_FAIL, no credit),
       DFTU_PORT_TOO_BIG (over max_msg_bytes) or DFTU_PORT_CLOSED. With
       DFTU_OVERFLOW_BLOCK the task waits for a credit. *out_status must stay
       valid until the task completes. */
    dftu_status (*channel_send)(dftu_ctx* ctx, void* h, uint64_t channel,
                                const void* data, uint32_t len,
                                dftu_port_status* out_status,
                                dftu_task** out_task);
    /** Start a receive into `buf` (room for `cap`). The task waits for a
       message and completes with DFTU_OK once *out_status and *out_len are
       set: DFTU_PORT_OK with the message length; DFTU_PORT_TOO_BIG when `buf`
       is short, with the needed length, and the message stays queued; or
       DFTU_PORT_CLOSED when the channel is closed and drained. `buf`,
       *out_status and *out_len must stay valid until the task completes. */
    dftu_status (*channel_recv)(dftu_ctx* ctx, void* h, uint64_t channel,
                                void* buf, uint32_t cap, uint32_t* out_len,
                                dftu_port_status* out_status,
                                dftu_task** out_task);
    /** A send that never waits; a full channel reports DFTU_PORT_FULL
       whatever its overflow mode, except DFTU_OVERFLOW_DROP_OLDEST. */
    dftu_status (*channel_try_send)(dftu_ctx* ctx, void* h, uint64_t channel,
                                    const void* data, uint32_t len,
                                    dftu_port_status* out_status);
    /** End the sends. A receive still gets what was queued, then
       DFTU_PORT_CLOSED. The host closes a channel when every plugin that
       opened it with DFTU_CHANNEL_F_SENDS has finished on_finalize (or failed
       it, or has none), and every channel when the scan ends. */
    dftu_status (*channel_close)(dftu_ctx* ctx, void* h, uint64_t channel);

    /** A latest port is a hint: the last value set under `name`, and a read
       never waits. The host numbers each set with a 64-bit generation that
       counts from 1. The name is prefixed as a channel name is. */
    dftu_status (*latest_set)(dftu_ctx* ctx, void* h, const char* name,
                              const void* data, uint32_t len,
                              uint64_t* out_generation);
    /** Copies the latest value into `buf` (room for `cap`) with its
       generation. *out_status is DFTU_PORT_OK, DFTU_PORT_NOT_READY before the
       first set, or DFTU_PORT_TOO_BIG with *out_len the needed size. */
    dftu_status (*latest_get)(dftu_ctx* ctx, void* h, const char* name,
                              void* buf, uint32_t cap, uint32_t* out_len,
                              uint64_t* out_generation,
                              dftu_port_status* out_status);

    /** Declare generation `generation` of the phase result `name` in a plugin
       factory, with its merge and value type; 0 asks the host for the next
       one. The host assigns generations in order from 1: a declaration that
       skips one fails the plan. *out_generation is the one assigned. */
    dftu_status (*phase_declare)(dftu_ctx* ctx, void* h, const char* name,
                                 dftu_merge_op op, dftu_type type,
                                 uint64_t generation, uint64_t* out_generation);
    /** Merge `data` into the open generation with the declared op and type.
       *out_status is DFTU_PORT_OK, DFTU_PORT_TOO_BIG for a payload the op
       refuses, or DFTU_PORT_CLOSED once the generation is sealed. */
    dftu_status (*phase_contribute)(dftu_ctx* ctx, void* h, const char* name,
                                    uint64_t generation, dftu_merge_op op,
                                    dftu_type type, const void* data,
                                    uint32_t len, dftu_port_status* out_status);
    /** Close generation `generation` to contributions. Generations seal in
       order; sealing one whose predecessor is open is DFTU_E_INVALID. */
    dftu_status (*phase_seal)(dftu_ctx* ctx, void* h, const char* name,
                              uint64_t generation);
    /** Copies the merged result of a sealed generation into `buf`. *out_status
       is DFTU_PORT_OK, DFTU_PORT_NOT_READY while it is open, or
       DFTU_PORT_TOO_BIG with *out_len the needed size. A generation with no
       contribution gives an empty result. */
    dftu_status (*phase_result)(dftu_ctx* ctx, void* h, const char* name,
                                uint64_t generation, void* buf, uint32_t cap,
                                uint32_t* out_len,
                                dftu_port_status* out_status);

    /** The receive of a channel of fixed-size values. Like channel_recv, but
       a message whose length is not `cap` is taken off the queue and dropped,
       and *out_status is DFTU_PORT_TOO_BIG with *out_len the length of the
       dropped message, so a wrong-sized message never blocks the channel and
       never fills `buf` short. */
    dftu_status (*channel_recv_exact)(dftu_ctx* ctx, void* h, uint64_t channel,
                                      void* buf, uint32_t cap,
                                      uint32_t* out_len,
                                      dftu_port_status* out_status,
                                      dftu_task** out_task);

    /** In a plugin factory, make generation `generation` of the phase result
       `name`, already declared with phase_declare, a seed: the host fills and
       seals it before the scan from the index statistics of the input files,
       from a cache keyed by the files' stat and sample hash, or from a first
       host pass over the files, in that order, and never from a stale source.
       The phase must have been declared with the merge the seed takes
       (DFTU_PHASE_SEED_TS_MIN with DFTU_MERGE_MIN, TS_MAX with MAX,
       EVENT_COUNT with SUM) and DFTU_T_U64, and every earlier generation must
       be a seed too, since generations seal in order; otherwise the call fails
       and the plan error names the phase. A plugin reads the value with
       phase_result, which is ready from the first batch, and contributing to a
       seeded generation is refused with an error that names it. Refused
       outside a plugin factory. */
    dftu_status (*phase_seed)(dftu_ctx* ctx, void* h, const char* name,
                              uint64_t generation, dftu_phase_seed seed);
} dftu_svc_ports;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_PORTS_H */
