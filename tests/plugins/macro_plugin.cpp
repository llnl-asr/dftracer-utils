// SPDX-License-Identifier: MIT
// A whole plugin in one DFTU_PLUGIN call: it declares its name and version,
// registers its fold, and returns no error. The SDK writes the two exported
// symbols, so this file names no C ABI type, call or constant.

#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

struct MacroSlice {
    explicit MacroSlice(const Config&) {}

    void step(const Batch&, Host) {}
    void merge(MacroSlice&) {}
    void finalize(Host) {}
};

}  // namespace

// The body holds a structured binding and a braced list, whose commas sit
// outside parentheses: the macro takes its body as a variadic argument, so they
// do not end it.
DFTU_PLUGIN("macro_plugin", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                auto [first, second] = dstd::Pair<int, int>{1, 2};
                const dstd::Vector<int> sizes{first, second};
                const dstd::Vector<dstd::Pair<int, int>> pairs{{first, second}};
                (void)sizes;
                (void)pairs;
                p.fold<MacroSlice>();
                return {};
            })
