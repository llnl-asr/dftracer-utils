#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_PROVIDER_SOURCE_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_PROVIDER_SOURCE_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/lazyframe.h>

#include <memory>

namespace dftracer::utils::dataframe {

// Wrap a raw dftu_source_vt/self pair as the same Source adapter
// dftu_provider_register's registry uses, without going through the
// registry or the plugin system. Lets a test drive the scan() C-ABI
// marshaling (dftu_scan_request, the out_pushed array, projection
// verification) directly against a hand-written vtable. `vt` is copied;
// `self` must outlive every scan opened against the returned Source.
std::shared_ptr<Source> make_provider_source(const dftu_source_vt& vt,
                                             void* self);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_PROVIDER_SOURCE_H
