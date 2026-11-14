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
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

    dstd::Result<void> write(const dftu_dataframe* batch) override {
        if (batch == nullptr)
            return dstd::Error::from_format<"toy_sink: no frame">(
                dstd::Condition::InvalidArgument);
        ++g_written;
        if (g_fail_after >= 0 && g_written > g_fail_after)
            return dstd::Error::from_format<"toy_sink: write refused">(
                dstd::Condition::InvalidArgument);
        dftu_series* ids = dftu_dataframe_column(batch, "id");
        if (ids != nullptr) {
            const auto* data =
                static_cast<const std::int64_t*>(dftu_series_data(ids));
            const std::int64_t n = dftu_series_length(ids);
            for (std::int64_t i = 0; i < n && g_row_count < 64; ++i)
                g_rows[g_row_count++] = data[i];
            dftu_series_free(ids);
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
        const dftu_value* root = view.options;
        if (root != nullptr && root->kind == DFTU_VAL_OBJECT &&
            root->count > 0 && root->as.members[0].value != nullptr &&
            root->as.members[0].value->kind == DFTU_VAL_STR) {
            std::snprintf(g_option_key, sizeof(g_option_key), "%s",
                          root->as.members[0].key);
            std::snprintf(g_option_value, sizeof(g_option_value), "%s",
                          root->as.members[0].value->as.str);
        }
        if (root != nullptr && root->kind == DFTU_VAL_OBJECT) {
            g_fail_after = -1;
            for (std::uint64_t i = 0; i < root->count; ++i) {
                const dftu_value* v = root->as.members[i].value;
                if (v != nullptr && v->kind == DFTU_VAL_STR &&
                    std::snprintf(nullptr, 0, "%s", root->as.members[i].key) >
                        0 &&
                    std::strcmp(root->as.members[i].key, "fail") == 0)
                    g_fail_after = std::atoi(v->as.str);
            }
        }
        return dstd::UniquePtr<plugins::SinkWriter>(
            dstd::make_unique<ToyWriter>());
    }
};

}  // namespace

DFTU_PLUGIN_INFO("toy_sink", "1.0")

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

extern "C" DFTU_PLUGIN_EXPORT dftu_status
dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
                dftu_plugin** out) {
    return plugins::plugin(ctx, h, config)
        .sink("toy_sink.out",
              dstd::UniquePtr<plugins::Sink>(dstd::make_unique<ToySink>()))
        .build(out);
}
