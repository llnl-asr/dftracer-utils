// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/transport.h>

namespace dftracer::utils::comm {

int LocalTransport::rank() const { return 0; }

int LocalTransport::size() const { return 1; }

dstd::Vector<dstd::String> LocalTransport::all_gather(
    dstd::StringView payload) {
    return {dstd::String(payload)};
}

}  // namespace dftracer::utils::comm
