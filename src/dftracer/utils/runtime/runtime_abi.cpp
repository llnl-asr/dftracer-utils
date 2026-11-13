// SPDX-License-Identifier: MIT
#include <dftracer/utils/runtime/abi.h>
#include <dftracer/utils/runtime/runtime.h>

using namespace dftracer::utils;

extern "C" {

dftu_runtime* dftu_default_runtime(void) {
    return reinterpret_cast<dftu_runtime*>(&default_runtime());
}

dftu_runtime* dftu_runtime_new(int32_t threads, int32_t io_threads) {
    RuntimeSettings settings;
    settings.threads = threads > 0 ? static_cast<std::size_t>(threads) : 0;
    settings.io_threads =
        io_threads > 0 ? static_cast<std::size_t>(io_threads) : 0;
    return reinterpret_cast<dftu_runtime*>(
        &configure_default_runtime(settings));
}

void dftu_runtime_free(dftu_runtime* rt) { (void)rt; }

}  // extern "C"
