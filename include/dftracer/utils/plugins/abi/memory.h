// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_MEMORY_H
#define DFTRACER_UTILS_PLUGINS_ABI_MEMORY_H

/** @file
 * dftu.svc.memory: the host's memory manager lent to a plugin. Optional
 * service group, fetched via dftu_plugin_host::get_service(DFTU_SVC_MEMORY)
 * from the run-time host a fold callback receives, or from the host a
 * provider's scan or a node's open receives; the build-phase host does not
 * answer it. Include dftracer/utils/plugins/abi.h rather than this file
 * directly.
 */

#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_MEMORY "dftu.svc.memory"

/** A claim on the memory budget, held in the plugin's pool. */
typedef struct dftu_reservation dftu_reservation;

/** Every plugin has one pool, named after the plugin, and the bytes of alloc
 * and the bytes of its reservations show in the pool's gauges under that name.
 * The plugin's pools are separate from each other.
 *
 * Blocks: alloc, realloc and free take the size and the alignment, and the
 * host keeps no header. The size and align passed to realloc and free must be
 * the ones the block was made with. A block belongs to the plugin until it
 * frees it, and a block that is still live when the plugin unloads is reported
 * as a leak. Alignment is a power of two up to 4096; any other value, and an
 * allocation the host cannot make, gives NULL and no message. alloc does not
 * count against a reservation.
 *
 * Budget: a reservation counts toward the memory budget of the process but
 * allocates nothing, so reserve the bytes a state is about to hold before the
 * work that allocates them. A slice that holds growing state keeps one
 * reservation, grows it with try_grow as the state grows and gives bytes back
 * with shrink. try_grow is all or nothing: past the budget of the scan, or the
 * budget of the process, it returns DFTU_E_NO_MEMORY with a message on `ctx`
 * and leaves the reservation as it was. The scan's budget is one budget for all
 * the reservations of the plugin together, not one per reservation. Before it
 * fails, the host calls the reclaimer of that reservation once with the number
 * of bytes missing and counts the bytes the reclaimer reports as freed against
 * the reservation, so a reclaimer must not call shrink itself; if that frees
 * enough, the grow then succeeds. A reclaimer runs on the thread that owns the
 * reservation: inside try_grow, and at a batch boundary (after a step or a
 * merge) when another holder of the process budget asked the plugin's pool for
 * bytes, so it must not call try_grow or release on the same reservation. For
 * a cursor the boundary is between two next() calls, on the thread that drives
 * the cursor. A reclaimer may free its own state, call alloc, realloc and free,
 * and release another reservation of the plugin; it must not call reserve,
 * try_grow, shrink or set_reclaimer, or release its own reservation.
 *
 * A reservation and its calls are not thread safe: one thread uses a given
 * reservation at a time. A reservation that is still held when the plugin
 * unloads, or when the cursor it was made for is destroyed, is released by the
 * host, which logs an error. Release every reservation before the slice or
 * cursor that holds it is destroyed. */
typedef struct dftu_svc_memory {
    uint32_t struct_size; /**< sizeof(dftu_svc_memory) */
    /** A block of `size` bytes aligned to `align`, or NULL. */
    void* (*alloc)(void* h, uint64_t size, uint32_t align);
    /** Resizes the block `p` of `old_size` bytes to `new_size` bytes and keeps
       the first min(old_size, new_size) bytes. A NULL `p` is alloc. On NULL the
       block `p` is untouched and still the plugin's. */
    void* (*realloc)(void* h, void* p, uint64_t old_size, uint64_t new_size,
                     uint32_t align);
    /** Frees a block of `size` bytes from alloc or realloc; NULL is allowed. */
    void (*free)(void* h, void* p, uint64_t size, uint32_t align);
    /** Opens an empty reservation in `*r`. `label` is for the host's logs and
       may be NULL; the host copies it. Release it with release. */
    dftu_status (*reserve)(dftu_ctx* ctx, void* h, dftu_reservation** r,
                           const char* label);
    /** Adds `bytes` to the reservation, or returns DFTU_E_NO_MEMORY. */
    dftu_status (*try_grow)(dftu_ctx* ctx, dftu_reservation* r, uint64_t bytes);
    /** Gives `bytes` back, at most what the reservation holds. */
    void (*shrink)(dftu_reservation* r, uint64_t bytes);
    /** Gives every byte back and frees the reservation; NULL is allowed. */
    void (*release)(dftu_reservation* r);
    /** Sets the function the host calls when a grow needs room: it frees up
       to `want` bytes of what the reservation covers (writes part of the state
       out, compacts, drops a cache) and returns how many it freed; 0 says
       nothing could go. `fn` NULL clears it. */
    void (*set_reclaimer)(dftu_reservation* r,
                          uint64_t (*fn)(void* ud, uint64_t want), void* ud);
} dftu_svc_memory;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_MEMORY_H */
