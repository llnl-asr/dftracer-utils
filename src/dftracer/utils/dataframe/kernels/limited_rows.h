// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_LIMITED_ROWS_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_LIMITED_ROWS_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/string_reader.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::dataframe::kernels {

// The strings whose regex match reached its work limit; their rows turn null.
class LimitedRows {
   public:
    void add(dstd::StringView s) {
        std::lock_guard<std::mutex> lock(mu_);
        rows_.emplace(s);
    }

    // `out` with every row of `v` whose string is listed made null.
    dftu_series* apply(dftu_series* out, const dftu_series* v) const {
        if (!out || rows_.empty()) return out;
        dftu_series* ok = string_mask(v, [this](dstd::StringView s) {
            return !rows_.contains(dstd::String(s));
        });
        if (!ok) return out;
        const std::size_t bytes = buffer_bytes(TypeId::Bool, v->length);
        auto valid = new_buffer(bytes);
        for (std::size_t k = 0; k < bytes; ++k) {
            const std::uint8_t have = out->validity.block()
                                          ? out->validity.data()[k]
                                          : std::uint8_t{0xFF};
            writable(valid)[k] =
                static_cast<std::uint8_t>(ok->data.data()[k] & have);
        }
        std::int64_t nulls = 0;
        for (std::int64_t i = 0; i < v->length; ++i)
            if (!((valid.data()[i >> 3] >> (i & 7)) & 1)) ++nulls;
        delete ok;
        out->validity = std::move(valid);
        out->null_count = nulls;
        return out;
    }

   private:
    std::mutex mu_;
    dstd::HashSet<dstd::String> rows_;
};

}  // namespace dftracer::utils::dataframe::kernels

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_LIMITED_ROWS_H
