// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/tz.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

void dftu_tz_set_search_path(const char* paths, std::int32_t len) {
    using namespace dftracer::utils;
    if (paths == nullptr || len <= 0) {
        dataframe::tz::set_search_path({});
        return;
    }
    dataframe::tz::set_search_path(
        dstd::StringView(paths, static_cast<std::size_t>(len)));
}
