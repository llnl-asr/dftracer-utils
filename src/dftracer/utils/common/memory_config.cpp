#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/memory_config.h>
#include <dftracer/utils/common/str_format.h>

#include <mutex>
#include <optional>
#include <string>

namespace dftracer::utils {

namespace {

// Unset or empty leaves the field at zero; a value that does not parse is an
// error that names the variable.
Result<std::size_t> read_bytes(const char* name) {
    auto raw = Env::get<std::string_view>(name);
    if (!raw) return std::size_t{0};
    auto parsed = parse_bytes(*raw);
    if (!parsed) {
        return make_error(ErrorCode::InvalidArgument,
                          str_cat(name, ": '", *raw,
                                  "' is not a byte size (use e.g. 4294967296, "
                                  "512MB, 4GB)"));
    }
    return static_cast<std::size_t>(*parsed);
}

}  // namespace

Result<memory::Config> memory_config_from_env() {
    namespace s = constants::memory_settings;
    memory::Config config;
    struct Field {
        const char* name;
        std::size_t* target;
    };
    const Field fields[] = {
        {s::BUDGET_ENV, &config.budget},
        {s::HARD_LIMIT_ENV, &config.hard_cap},
        {s::LIMIT_HEADROOM_ENV, &config.limit_headroom},
        {s::SPILL_HEADROOM_ENV, &config.spill_headroom},
    };
    for (const Field& f : fields) {
        auto bytes = read_bytes(f.name);
        if (!bytes) return dstd::Unexpected(std::move(bytes).error());
        *f.target = *bytes;
    }
    return config;
}

Result<void> configure_memory_from_env() {
    static std::once_flag once;
    static Result<void> first;
    std::call_once(once, [] {
        auto config = memory_config_from_env();
        if (!config) {
            first = dstd::Unexpected(std::move(config).error());
            return;
        }
        memory::configure(*config);
        namespace s = constants::memory_settings;
        auto report = Env::get<int>(s::REPORT_ENV);
        memory::set_exit_report(report ? *report != 0 : s::DEFAULT_REPORT);
    });
    return first;
}

}  // namespace dftracer::utils
