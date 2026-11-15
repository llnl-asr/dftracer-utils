// SPDX-License-Identifier: MIT
/* Test-only plugin whose body registers a fold and a UDF and then returns an
 * error. The load fails, so the fold the builder still holds must be released
 * by the builder itself and the UDF must be rolled back by the host.
 */

#include <dftracer/utils/plugins/plugin.h>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

struct RefusedSlice {
    explicit RefusedSlice(const Config&) {}
    void step(const dftu_dataframe*, Host) {}
    void merge(RefusedSlice&) {}
    void finalize(Host) {}
};

// Registered but never called: the load fails before any scan runs.
struct RefusedUdf {
    static dstd::Result<dstd::UniquePtr<RefusedUdf>> bind(UdfBind& bind) {
        bind.result(dataframe::TypeId::Int64);
        return dstd::make_unique<RefusedUdf>();
    }

    dstd::Result<Column> exec(UdfBatch& batch) const {
        dstd::Vector<std::int64_t> values(
            static_cast<std::size_t>(batch.rows()), 0);
        return Column::from_int64(std::move(values));
    }
};

}  // namespace

DFTU_PLUGIN("error_after_register_plugin", "1.0",
            [](PluginBuilder& p, const Config&) -> dstd::Result<void> {
                p.fold<RefusedSlice>();
                p.udf<RefusedUdf>("refused");
                return invalid_argument(
                    "error_after_register_plugin: refusing to load");
            })
