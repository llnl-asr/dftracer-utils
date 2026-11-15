// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_PROVIDER_REGISTRY_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_PROVIDER_REGISTRY_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::dataframe {

/// A source registered by name. dftu_lazyframe_from_provider builds a plan over
/// the Source it makes.
class ProviderFactory {
   public:
    virtual ~ProviderFactory() = default;

    /// A Source over this provider. Every call may share the provider's state,
    /// which lives until the last Source made from it is gone.
    virtual dstd::SharedPtr<Source> make_source() = 0;
};

/// Registers `factory` under `name`. A name already taken is refused, never
/// replaced.
dstd::Result<void> register_provider(dstd::StringView name,
                                     dstd::SharedPtr<ProviderFactory> factory);

/// Removes the provider registered as `name`; false when there is none.
/// Sources already made from it keep it alive.
bool unregister_provider(dstd::StringView name);

/// The provider registered as `name`, or null.
dstd::SharedPtr<ProviderFactory> find_provider(dstd::StringView name);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_PROVIDER_REGISTRY_H
