// SPDX-License-Identifier: MIT
// The @NAME@ udf plugin, made by `dftracer_plugin new`.
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstddef>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

/// A scalar UDF runs on whole columns, never row by row. A row with a null
/// argument gives a null result without the UDF running for it. The argument
/// is a borrowed view of the column, so a read copies nothing.
struct Doubled {
    dataframe::TypeId type = dataframe::TypeId::Unknown;

    /// Called once per query with the argument types: check them and declare
    /// the result type.
    static dstd::Result<dstd::UniquePtr<Doubled>> bind(UdfBind& bind) {
        const dataframe::TypeId type = bind.type(0);
        if (type != dataframe::TypeId::Int64 &&
            type != dataframe::TypeId::Uint64 &&
            type != dataframe::TypeId::Float64)
            return dstd::Unexpected<dstd::Error>(
                invalid_argument("doubled takes a number column"));
        bind.result(dataframe::TypeId::Float64);
        return dstd::make_unique<Doubled>(Doubled{type});
    }

    dstd::Result<Column> exec(UdfBatch& batch) const {
        const auto rows = static_cast<std::int64_t>(batch.rows());
        const ColumnView values = batch.arg(0);
        dstd::Vector<double> out(static_cast<std::size_t>(rows));
        switch (type) {
            case dataframe::TypeId::Int64: {
                const dstd::Span<const std::int64_t> v =
                    values.values<std::int64_t>();
                for (std::int64_t r = 0; r < rows; ++r)
                    out[static_cast<std::size_t>(r)] =
                        2 * static_cast<double>(v[static_cast<std::size_t>(r)]);
                break;
            }
            case dataframe::TypeId::Uint64: {
                const dstd::Span<const std::uint64_t> v =
                    values.values<std::uint64_t>();
                for (std::int64_t r = 0; r < rows; ++r)
                    out[static_cast<std::size_t>(r)] =
                        2 * static_cast<double>(v[static_cast<std::size_t>(r)]);
                break;
            }
            default: {
                const dstd::Span<const double> v = values.values<double>();
                for (std::int64_t r = 0; r < rows; ++r)
                    out[static_cast<std::size_t>(r)] =
                        2 * v[static_cast<std::size_t>(r)];
                break;
            }
        }
        Column column = Column::from_double(std::move(out));
        if (column.is_null())
            return dstd::Unexpected<dstd::Error>(
                invalid_argument("doubled could not build a column"));
        return column;
    }
};

DFTU_PLUGIN("@NAME@", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.udf<Doubled>("@NAME@.doubled");
                return {};
            })
