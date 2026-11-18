// SPDX-License-Identifier: MIT
// A source and a node written with the C++ SDK that have explain hooks and add
// to run counters, so the SDK sets the explain entry of the source and of the
// node and plugins::counter reaches the host.
//
//   sdk_explain.src   three rows (id 1..3), counts `rows`
//   sdk_explain.pass  passes its input on, counts `frames`

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

class SdkSource {
   public:
    dstd::Vector<dstd::String> names() const { return {"id"}; }

    class Cursor {
       public:
        dstd::Result<dstd::Optional<OwnedDataFrame>> next(std::int64_t) {
            if (done_) return dstd::nullopt;
            done_ = true;
            FrameBuilder builder;
            builder.add("id", Column::from_int64({1, 2, 3}));
            auto built = builder.build();
            if (!built) return std::move(built).error();
            counter("rows", 3);
            return dstd::Optional<OwnedDataFrame>(std::move(*built));
        }

       private:
        bool done_ = false;
    };

    dstd::Result<ScanResult<Cursor>> scan(const ScanView&, const Host&) const {
        return ScanResult<Cursor>{dstd::make_unique<Cursor>(), {}};
    }

    dstd::Result<void> explain(const ScanView& view,
                               const ExplainOut& out) const {
        out.add("mode", "sdk");
        out.add("filters_offered", dstd::to_string(view.filters.size()));
        return ok();
    }
};

class SdkPass {
   public:
    void output_schema(const SchemaView& in, const OpArgs&,
                       SchemaBuilder& out) const {
        in.copy_all(out);
    }

    class Cursor {
       public:
        explicit Cursor(InputCursor in) : in_(std::move(in)) {}
        dstd::Result<dstd::Optional<OwnedDataFrame>> next(
            std::int64_t max_rows) {
            auto got = in_.next(max_rows);
            if (!got) return std::move(got).error();
            if (*got) counter("frames", 1);
            return std::move(*got);
        }

       private:
        InputCursor in_;
    };

    dstd::Result<dstd::UniquePtr<Cursor>> open(InputCursor in, const OpArgs&,
                                               std::uint64_t,
                                               const Host&) const {
        return dstd::make_unique<Cursor>(std::move(in));
    }

    dstd::Result<void> explain(const OpArgs&, const ExplainOut& out) const {
        out.add("mode", "sdk");
        return ok();
    }
};

}  // namespace

DFTU_PLUGIN("sdk_explain", "1.0",
            [](PluginBuilder& p, const Config&) -> dstd::Result<void> {
                p.source<SdkSource>("sdk_explain.src",
                                    dstd::make_unique<SdkSource>());
                p.node<SdkPass>("sdk_explain.pass",
                                dstd::make_unique<SdkPass>());
                return {};
            })
