// SPDX-License-Identifier: MIT
/* Test-only plugin exercising the C++ sink SDK (PluginBuilder::sink<S>(),
 * plugin/file_format.h). The sink is "toy_sink.out".
 *
 * The writer counts its constructions and destructions and records the options
 * it was bound with, so the test can see from outside that the host releases
 * every writer it binds, on finalize and on abort alike, and that the options
 * tree reaches the plugin as string members. toy_sink_stats() exports the
 * counters; the test reads it with dlsym.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace dftracer::utils;

namespace {

std::int64_t g_made = 0;
std::int64_t g_killed = 0;
std::int64_t g_written = 0;
std::int64_t g_finalized = 0;
std::int64_t g_aborted = 0;
std::int64_t g_bound_columns = 0;
std::int64_t g_rows[64] = {0};
std::int64_t g_row_count = 0;
std::int64_t g_fail_after = -1;
char g_option_key[64];
char g_option_value[64];

class ToyWriter : public plugins::SinkWriter {
   public:
    ToyWriter() { ++g_made; }
    ~ToyWriter() override { ++g_killed; }

    dstd::Result<void> write(plugins::FrameView batch) override {
        ++g_written;
        if (g_fail_after >= 0 && g_written > g_fail_after)
            return plugins::invalid_argument(
                dstd::fmt<"toy_sink: write refused">());
        const dstd::Optional<plugins::ColumnView> ids = batch.column("id");
        if (ids) {
            const dstd::Span<const std::int64_t> data =
                ids->values<std::int64_t>();
            for (std::size_t i = 0; i < data.size() && g_row_count < 64; ++i)
                g_rows[g_row_count++] = data[i];
        }
        return {};
    }

    dstd::Result<void> finalize() override {
        ++g_finalized;
        return {};
    }

    void abort() override { ++g_aborted; }
};

class ToySink : public plugins::Sink {
   public:
    dstd::Result<dstd::UniquePtr<plugins::SinkWriter>> bind(
        const plugins::BindView& view) override {
        g_bound_columns = static_cast<std::int64_t>(view.names.size());
        // The first key of the options object, whatever it is, so the test can
        // see that the tree reached the plugin.
        g_fail_after = -1;
        const plugins::ConfigValue root = view.options.value();
        if (root.is_object() && root.size() > 0) {
            for (std::uint32_t i = 0; i < root.size(); ++i) {
                const dstd::StringView key = root.key_at(i);
                const dstd::StringView value = root.value_at(i).as_str();
                if (i == 0 && value.data() != nullptr) {
                    std::snprintf(g_option_key, sizeof(g_option_key), "%.*s",
                                  static_cast<int>(key.size()), key.data());
                    std::snprintf(g_option_value, sizeof(g_option_value),
                                  "%.*s", static_cast<int>(value.size()),
                                  value.data());
                }
                if (key == "fail" && value.data() != nullptr)
                    g_fail_after = std::atoi(dstd::String(value).c_str());
            }
        }
        return dstd::UniquePtr<plugins::SinkWriter>(
            dstd::make_unique<ToyWriter>());
    }
};

}  // namespace

extern "C" {
DFTU_PLUGIN_EXPORT void toy_sink_stats(std::int64_t* out);
DFTU_PLUGIN_EXPORT const char* toy_sink_option_key();
DFTU_PLUGIN_EXPORT const char* toy_sink_option_value();
DFTU_PLUGIN_EXPORT const std::int64_t* toy_sink_rows();
DFTU_PLUGIN_EXPORT std::int64_t toy_sink_row_count();
}

extern "C" DFTU_PLUGIN_EXPORT void toy_sink_stats(std::int64_t* out) {
    out[0] = g_made;
    out[1] = g_killed;
    out[2] = g_written;
    out[3] = g_finalized;
    out[4] = g_aborted;
    out[5] = g_bound_columns;
}

extern "C" DFTU_PLUGIN_EXPORT const char* toy_sink_option_key() {
    return g_option_key;
}
extern "C" DFTU_PLUGIN_EXPORT const char* toy_sink_option_value() {
    return g_option_value;
}

// The values of the `id` column every written frame carried, in order.
extern "C" DFTU_PLUGIN_EXPORT const std::int64_t* toy_sink_rows() {
    return g_rows;
}

extern "C" DFTU_PLUGIN_EXPORT std::int64_t toy_sink_row_count() {
    return g_row_count;
}

DFTU_PLUGIN("toy_sink", "1.0",
            [](plugins::PluginBuilder& p,
               const plugins::Config& config) -> dstd::Result<void> {
                (void)config;
                p.sink("toy_sink.out", dstd::UniquePtr<plugins::Sink>(
                                           dstd::make_unique<ToySink>()));
                return {};
            })
