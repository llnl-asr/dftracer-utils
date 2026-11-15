// SPDX-License-Identifier: MIT
/* Test-only plugin exercising the C++ node SDK (PluginBuilder::node<N>(),
 * plugin/node.h) through a duql `call` stage. It registers two plan nodes that
 * pass every column and every row through unchanged: "toy_node.pass", and
 * "toy_node.dup", whose name the test also gives to a frame op, to show which
 * of the two a `call` reaches.
 */

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

class PassThrough {
   public:
    dstd::Result<void> output_schema(const SchemaView& in, const OpArgs&,
                                     SchemaBuilder& out) const {
        in.copy_all(out);
        return {};
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

}  // namespace

DFTU_PLUGIN_INFO("toy_node", "1.0")

extern "C" DFTU_PLUGIN_EXPORT dftu_status
dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
                dftu_plugin** out) {
    return plugin(ctx, h, config)
        .node<PassThrough>("toy_node.pass", dstd::make_unique<PassThrough>())
        .node<PassThrough>("toy_node.dup", dstd::make_unique<PassThrough>())
        .build(out);
}
