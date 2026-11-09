// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/memory_config.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/string.h>

#include <mutex>

namespace dftracer::utils::common {

namespace {

// Unset or empty leaves the field at zero. A value that does not parse is an
// error that names the variable.
dstd::Result<std::size_t> read_bytes(const char* name) {
    auto raw = Env::get<dstd::StringView>(name);
    if (!raw) return std::size_t{0};
    auto parsed = parse_bytes(*raw);
    if (!parsed) {
        return make_error<
            "{}: '{}' is not a byte size (use e.g. 4294967296, 512MB, 4GB)">(
            ErrorCode::InvalidArgument, name, *raw);
    }
    return static_cast<std::size_t>(*parsed);
}

}  // namespace

dstd::Result<memory::Config> memory_config_from_env() {
    memory::Config config;
    struct Field {
        const char* name;
        std::size_t* target;
    };
    const Field fields[] = {
        {constants::memory_settings::BUDGET_ENV, &config.budget},
        {constants::memory_settings::HARD_LIMIT_ENV, &config.hard_cap},
        {constants::memory_settings::LIMIT_HEADROOM_ENV,
         &config.limit_headroom},
        {constants::memory_settings::SPILL_HEADROOM_ENV,
         &config.spill_headroom},
    };
    for (const Field& f : fields) {
        auto bytes = read_bytes(f.name);
        if (!bytes) return dstd::Unexpected(std::move(bytes).error());
        *f.target = *bytes;
    }
    return config;
}

dstd::Result<void> configure_memory_from_env() {
    static std::once_flag once;
    static dstd::Result<void> first;
    std::call_once(once, [] {
        auto config = memory_config_from_env();
        if (!config) {
            first = dstd::Unexpected(std::move(config).error());
            return;
        }
        memory::configure(*config);
        auto report = Env::get<int>(constants::memory_settings::REPORT_ENV);
        memory::detail::set_exit_report(
            report ? *report != 0 : constants::memory_settings::DEFAULT_REPORT);
    });
    return first;
}

}  // namespace dftracer::utils::common
