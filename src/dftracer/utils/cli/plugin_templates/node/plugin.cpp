// SPDX-License-Identifier: MIT
// The @NAME@ node plugin, made by `dftracer_plugin new`.
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/plugin/node.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

/// A plan node takes an input cursor and gives an output cursor. This one
/// passes every frame through unchanged, the smallest thing a node can do: the
/// columns it is called on are the columns it returns.
struct PassThrough {
    void output_schema(const SchemaView& in, const OpArgs&,
                       SchemaBuilder& out) const {
        in.copy_all(out);
    }

    struct Cursor {
        InputCursor in;

        dstd::Result<dstd::Optional<OwnedDataFrame>> next(std::int64_t rows) {
            return in.next(rows);
        }
    };

    dstd::Result<dstd::UniquePtr<Cursor>> open(InputCursor in, const OpArgs&,
                                               std::uint64_t,
                                               const Host&) const {
        return dstd::make_unique<Cursor>(Cursor{std::move(in)});
    }
};

DFTU_PLUGIN("@NAME@", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.node<PassThrough>("@NAME@.pass",
                                    dstd::make_unique<PassThrough>());
                return {};
            })
