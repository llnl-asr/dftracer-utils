// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_STORE_TYPES_H
#define DFTRACER_UTILS_TRACE_INDEX_STORE_TYPES_H

// Umbrella for every index result/record type. Consumers that need one concern
// include the specific header (types/bloom.h, types/statistics.h, ...); this
// pulls in all of them.
#include <dftracer/utils/trace/index/gzip/member.h>
#include <dftracer/utils/trace/index/schemas/dft/statistics.h>
#include <dftracer/utils/trace/index/store/file.h>

#endif  // DFTRACER_UTILS_TRACE_INDEX_STORE_TYPES_H
