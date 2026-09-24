#ifndef DFTRACER_UTILS_INDEX_STORE_TYPES_H
#define DFTRACER_UTILS_INDEX_STORE_TYPES_H

// Umbrella for every index result/record type. Consumers that need one concern
// include the specific header (types/bloom.h, types/statistics.h, ...); this
// pulls in all of them.
#include <dftracer/utils/index/gzip/member.h>
#include <dftracer/utils/index/schemas/dft/statistics.h>
#include <dftracer/utils/index/store/file.h>

#endif  // DFTRACER_UTILS_INDEX_STORE_TYPES_H
