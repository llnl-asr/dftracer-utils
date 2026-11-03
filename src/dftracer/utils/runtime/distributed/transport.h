// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_DISTRIBUTED_TRANSPORT_H
#define DFTRACER_UTILS_RUNTIME_DISTRIBUTED_TRANSPORT_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

namespace dftracer::utils::distributed {

/// Abstracts how ranks in a distributed run exchange data, so a computation is
/// written once and runs local, over MPI, over RPC, or anything else by
/// swapping the implementation. Only the collectives a gather-then-merge
/// reduction needs are exposed; add more as callers require. The interface
/// itself has no MPI (or other transport) dependency, so it lives in core and
/// everything can link it.
class Transport {
   public:
    virtual ~Transport() = default;

    virtual int rank() const = 0;
    virtual int size() const = 0;

    /// Gather each rank's `payload`; every rank receives all payloads ordered
    /// by rank. The building block for distributed reductions (aggregate,
    /// etc.).
    virtual dstd::Vector<dstd::String> all_gather(dstd::StringView payload) = 0;

    /// Synchronize all ranks. Default no-op (correct for a single rank).
    virtual void barrier() {}
};

/// Degenerate single-process transport: one rank, gather returns just its own
/// payload. Lets the same code path run without any distribution backend.
class LocalTransport : public Transport {
   public:
    int rank() const override { return 0; }
    int size() const override { return 1; }
    dstd::Vector<dstd::String> all_gather(dstd::StringView payload) override {
        return {dstd::String(payload)};
    }
};

}  // namespace dftracer::utils::distributed

#endif  // DFTRACER_UTILS_RUNTIME_DISTRIBUTED_TRANSPORT_H
