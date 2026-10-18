#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/memory_config.h>
#include <dftracer/utils/memory/constants.h>
#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace dut = dftracer::utils;
namespace common = dftracer::utils::constants::memory_settings;
namespace mem = dftracer::utils::memory;

namespace {

constexpr std::size_t MIB = std::size_t{1} << 20;

struct EnvVar {
    EnvVar(const char* var, const char* value) : name(var) {
        ::setenv(var, value, 1);
    }
    ~EnvVar() { ::unsetenv(name); }
    const char* name;
};

void clear_memory_env() {
    for (const char* name :
         {common::BUDGET_ENV, common::HARD_LIMIT_ENV,
          common::LIMIT_HEADROOM_ENV, common::SPILL_HEADROOM_ENV}) {
        ::unsetenv(name);
    }
}

}  // namespace

TEST_CASE("the common and memory constant sets agree") {
    CHECK(std::strcmp(common::MEMORY_BACKEND_ENV,
                      mem::constants::MEMORY_BACKEND_ENV) == 0);
    CHECK(std::strcmp(common::DECAY_MS_ENV, mem::constants::DECAY_MS_ENV) == 0);
    CHECK(std::strcmp(common::BUDGET_ENV, mem::constants::BUDGET_ENV) == 0);
    CHECK(std::strcmp(common::HARD_LIMIT_ENV, mem::constants::HARD_LIMIT_ENV) ==
          0);
    CHECK(std::strcmp(common::LIMIT_HEADROOM_ENV,
                      mem::constants::LIMIT_HEADROOM_ENV) == 0);
    CHECK(std::strcmp(common::SPILL_HEADROOM_ENV,
                      mem::constants::SPILL_HEADROOM_ENV) == 0);
    CHECK(std::strcmp(common::DEBUG_ENV, mem::constants::DEBUG_ENV) == 0);
    CHECK(std::strcmp(common::EXIT_CHECK_ENV, mem::constants::EXIT_CHECK_ENV) ==
          0);

    CHECK(common::DEFAULT_DECAY_MS == mem::constants::DEFAULT_DECAY_MS);
    CHECK(common::DEFAULT_BUDGET_BYTES == mem::constants::DEFAULT_BUDGET_BYTES);
    CHECK(common::DEFAULT_HARD_LIMIT_BYTES ==
          mem::constants::DEFAULT_HARD_LIMIT_BYTES);
    CHECK(common::LIMIT_HEADROOM_PERCENT ==
          mem::constants::LIMIT_HEADROOM_PERCENT);
    CHECK(common::LIMIT_HEADROOM_MIN_BYTES ==
          mem::constants::LIMIT_HEADROOM_MIN_BYTES);
    CHECK(common::SPILL_HEADROOM_PERCENT ==
          mem::constants::SPILL_HEADROOM_PERCENT);
    CHECK(common::SPILL_HEADROOM_MIN_BYTES ==
          mem::constants::SPILL_HEADROOM_MIN_BYTES);
    CHECK(common::DEFAULT_DEBUG == mem::constants::DEFAULT_DEBUG);
    CHECK(common::DEFAULT_EXIT_CHECK == mem::constants::DEFAULT_EXIT_CHECK);
}

TEST_CASE("unset variables leave every field at its default") {
    clear_memory_env();
    auto config = dut::memory_config_from_env();
    REQUIRE(config);
    CHECK(config->budget == 0);
    CHECK(config->hard_cap == 0);
    CHECK(config->limit_headroom == 0);
    CHECK(config->spill_headroom == 0);
}

TEST_CASE("values read as bytes or with a unit") {
    clear_memory_env();
    EnvVar budget(common::BUDGET_ENV, "67108864");
    EnvVar cap(common::HARD_LIMIT_ENV, "1GiB");
    EnvVar limit(common::LIMIT_HEADROOM_ENV, "300MB");
    EnvVar spill(common::SPILL_HEADROOM_ENV, "5MB");
    auto config = dut::memory_config_from_env();
    REQUIRE(config);
    CHECK(config->budget == 64 * MIB);
    CHECK(config->hard_cap == 1024 * MIB);
    CHECK(config->limit_headroom == 300 * MIB);
    CHECK(config->spill_headroom == 5 * MIB);
}

TEST_CASE("a bad value is refused and names its variable") {
    clear_memory_env();
    for (const char* name :
         {common::BUDGET_ENV, common::HARD_LIMIT_ENV,
          common::LIMIT_HEADROOM_ENV, common::SPILL_HEADROOM_ENV}) {
        for (const char* bad : {"12 parsecs", "-5", "lots"}) {
            EnvVar var(name, bad);
            auto config = dut::memory_config_from_env();
            REQUIRE_FALSE(config);
            CHECK(config.error().code == dut::ErrorCode::InvalidArgument);
            CHECK(config.error().message.find(name) != std::string::npos);
            CHECK(config.error().message.find(bad) != std::string::npos);
        }
    }
}

TEST_CASE("memory statuses map to error codes") {
    using dut::ErrorCode;
    using mem::Status;
    static_assert(dut::error_code_of(Status::OverBudget) ==
                  ErrorCode::MemoryBudgetExceeded);
    CHECK(dut::error_code_of(Status::OverBudget) ==
          ErrorCode::MemoryBudgetExceeded);
    CHECK(dut::error_code_of(Status::OverCap) ==
          ErrorCode::MemoryBudgetExceeded);
    CHECK(dut::error_code_of(Status::Pending) ==
          ErrorCode::MemoryBudgetExceeded);
    CHECK(dut::error_code_of(Status::PoolTableFull) == ErrorCode::Internal);
    CHECK(dut::error_code_of(Status::SlotTableFull) == ErrorCode::Internal);
    CHECK(dut::error_code_of(Status::Ok) == ErrorCode::Unknown);

    CHECK(std::string(dut::error_code_name(ErrorCode::MemoryBudgetExceeded)) ==
          "MEMORY_BUDGET_EXCEEDED");
    CHECK(dut::condition_of(ErrorCode::MemoryBudgetExceeded) ==
          dut::Condition::Internal);
}

TEST_CASE("a failed reservation maps to the budget error") {
    mem::Config c;
    c.budget = 64 * MIB;
    mem::configure(c);
    mem::PoolHandle pool = mem::Manager::get().root().child("config");
    mem::Reservation over = pool->reserve(65 * MIB);
    REQUIRE_FALSE(over);
    CHECK(dut::error_code_of(over.status()) ==
          dut::ErrorCode::MemoryBudgetExceeded);
    mem::configure(mem::Config{});
}

// The only test case that calls configure_memory_from_env: it applies once
// per process.
TEST_CASE("env values reach memory::configure once per process") {
    clear_memory_env();
    EnvVar budget(common::BUDGET_ENV, "512MB");
    EnvVar cap(common::HARD_LIMIT_ENV, "8GB");
    EnvVar limit(common::LIMIT_HEADROOM_ENV, "10MB");
    EnvVar spill(common::SPILL_HEADROOM_ENV, "5MB");
    REQUIRE(dut::configure_memory_from_env());
    mem::Config applied = mem::config();
    CHECK(applied.budget == 512 * MIB);
    CHECK(applied.hard_cap == 8 * 1024 * MIB);
    CHECK(applied.limit_headroom == 10 * MIB);
    CHECK(applied.spill_headroom == 5 * MIB);

    ::setenv(common::BUDGET_ENV, "nonsense", 1);
    CHECK(dut::configure_memory_from_env());
    CHECK(mem::config().budget == 512 * MIB);
    mem::configure(mem::Config{});
}
