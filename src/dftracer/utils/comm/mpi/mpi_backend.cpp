// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/bulk.h>
#include <dftracer/utils/comm/mpi/collectives.h>
#include <dftracer/utils/comm/mpi/mpi_backend.h>

namespace dftracer::utils::comm::mpi {

dstd::Result<Providers> start(const StartOptions& options) {
    auto context = Context::make(options.app_comm);
    if (!context) return dstd::Unexpected(context.error());
    Providers providers;
    providers.bootstrap = make_bootstrap(*context);
    providers.messaging = make_messaging(*context);
    providers.collectives = make_collectives(*context);
    providers.bulk = backend::make_message_bulk(providers.messaging);
    return providers;
}

}  // namespace dftracer::utils::comm::mpi
