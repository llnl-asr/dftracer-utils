#ifndef DFTRACER_UTILS_DUQL_INTERNAL_DUQL_HANDLE_H
#define DFTRACER_UTILS_DUQL_INTERNAL_DUQL_HANDLE_H

#include <dftracer/utils/duql/abi.h>
#include <dftracer/utils/duql/query.h>

namespace dftracer::utils::duql {

// Access the Query wrapped by a dftu_duql handle. Internal bridge so a C ABI
// in another translation unit (the dataframe columnar backend) can execute a
// parsed query without re-declaring the opaque handle struct.
const Query& duql_handle_unwrap(const dftu_duql* h);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_INTERNAL_DUQL_HANDLE_H
