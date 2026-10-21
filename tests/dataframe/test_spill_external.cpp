// Spill files show their live bytes in the "spill" external slot. On a disk
// they are reported only, so they never take budget from a reservation.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils::dataframe;
using dftracer::utils::coro::CoroTask;
namespace memory = dftracer::utils::memory;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t MIB = std::size_t{1} << 20;
constexpr std::int64_t MORSELS = 64;
constexpr std::int64_t ROWS = 2048;
constexpr std::int64_t LOOK_AT = 60;
// The sort holds MORSELS * ROWS * 16 bytes = 2 MiB: half the global budget and
// four times the sort's own.
constexpr std::size_t GLOBAL_BUDGET = MIB;
constexpr std::size_t SORT_BUDGET = GLOBAL_BUDGET / 2;
constexpr std::size_t SMALL_SORT_BUDGET = SORT_BUDGET / 4;

std::size_t spill_slot_bytes() {
    std::size_t total = 0;
    for (const memory::ExternalStats& e : memory::external_stats())
        if (e.name == "spill") total += e.bytes;
    return total;
}

// One splitmix64 step: spread bits, so spill files do not compress and their
// size follows the row count on every codec.
std::int64_t mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return static_cast<std::int64_t>(x ^ (x >> 31));
}

// `morsels` morsels of `rows` rows of two columns of mixed bits; `hook` runs
// before morsel i is made.
class Hooked : public Source {
   public:
    using Hook = std::function<void(std::int64_t)>;

    Hooked(std::int64_t morsels, std::int64_t rows, Hook hook)
        : morsels_(morsels), rows_(rows), hook_(std::move(hook)) {}

    Schema schema() const override {
        const std::int64_t zero = 0;
        const DataType i = Series::flat_i64(&zero, 1).data_type();
        Schema out;
        out.fields = {Field{"k", i, true}, Field{"v", i, true}};
        return out;
    }

    ScanResult scan(const ScanRequest& req) const override {
        ScanResult r;
        r.cursor = std::make_unique<Rows>(morsels_, rows_, hook_);
        r.filters.assign(req.filters.size(), Pushed::No);
        return r;
    }

   private:
    class Rows : public Cursor {
       public:
        Rows(std::int64_t morsels, std::int64_t rows, Hook hook)
            : morsels_(morsels), rows_(rows), hook_(std::move(hook)) {}
        CoroTask<std::optional<Morsel>> next(std::int64_t) override {
            if (at_ >= morsels_) co_return std::nullopt;
            if (hook_) hook_(at_);
            std::vector<std::int64_t> k(static_cast<std::size_t>(rows_)),
                v(static_cast<std::size_t>(rows_));
            for (std::int64_t i = 0; i < rows_; ++i) {
                const std::int64_t row = at_ * rows_ + i;
                k[static_cast<std::size_t>(i)] =
                    mix(static_cast<std::uint64_t>(row));
                v[static_cast<std::size_t>(i)] =
                    mix(~static_cast<std::uint64_t>(row));
            }
            ++at_;
            Morsel m;
            m.rows = rows_;
            m.columns.push_back(Series::flat_i64(k.data(), rows_));
            m.columns.push_back(Series::flat_i64(v.data(), rows_));
            co_return m;
        }

       private:
        std::int64_t morsels_, rows_;
        Hook hook_;
        std::int64_t at_ = 0;
    };

    std::int64_t morsels_, rows_;
    Hook hook_;
};

struct Budget {
    explicit Budget(std::size_t bytes) {
        memory::Config c;
        c.budget = bytes;
        memory::configure(c);
    }
    ~Budget() { memory::configure(memory::Config{}); }
};

// A spill directory under the working directory, which is a disk in a build
// tree; the system temp directory may be tmpfs.
struct SpillDirVar {
    fs::path dir;
    SpillDirVar() {
        dir = fs::current_path() /
              ("spill_external_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        ::setenv("DFTRACER_UTILS_SPILL_DIR", dir.c_str(), 1);
    }
    ~SpillDirVar() {
        ::unsetenv("DFTRACER_UTILS_SPILL_DIR");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

}  // namespace

TEST_CASE("spill bytes show as external") {
    SpillDirVar var;
    const bool on_disk = !dftracer::utils::is_memory_filesystem(
        dftracer::utils::filesystem_kind(var.dir.string()));
    Budget budget(GLOBAL_BUDGET);
    REQUIRE(spill_slot_bytes() == 0);

    std::size_t seen = 0;
    std::size_t stats_external = 0;
    std::size_t free_budget = 0;
    memory::Status reserved = memory::Status::SlotTableFull;
    auto source = std::make_shared<Hooked>(MORSELS, ROWS, [&](std::int64_t i) {
        if (i != LOOK_AT) return;
        seen = spill_slot_bytes();
        stats_external = memory::Manager::get().stats().external;
        memory::Manager& m = memory::Manager::get();
        const std::size_t held = m.root().reserved();
        free_budget = m.budget() > held ? m.budget() - held : 0;
        memory::PoolHandle pool = m.root().child("probe");
        REQUIRE(pool);
        reserved = pool->try_reserve(free_budget / 2).status();
    });
    {
        const DataFrame out = LazyFrame::scan(source)
                                  .sort_by("k")
                                  .memory_budget(SORT_BUDGET)
                                  .collect()
                                  .get();
        CHECK(out.num_rows() == MORSELS * ROWS);
    }

    CHECK(seen > 0);
    CHECK(stats_external >= seen);
    if (on_disk) {
        // The spilled bytes exceed what the budget had left, so a reserve
        // that fits only succeeds when they are not counted.
        REQUIRE(seen > free_budget);
        CHECK(reserved == memory::Status::Ok);
    } else {
        MESSAGE("the working directory is tmpfs; spill bytes count there");
    }
    CHECK(spill_slot_bytes() == 0);
}

TEST_CASE("merged sort runs leave the spill slot before the query ends") {
    SpillDirVar var;
    REQUIRE(spill_slot_bytes() == 0);
    std::size_t before_merge = 0;
    auto source = std::make_shared<Hooked>(MORSELS, ROWS, [&](std::int64_t i) {
        if (i == MORSELS - 1) before_merge = spill_slot_bytes();
    });
    // The small budget cuts about 64 runs and merges them four at a time, so
    // two merge passes run before the first row comes out.
    std::unique_ptr<Cursor> cursor = LazyFrame::scan(source)
                                         .sort_by("k")
                                         .memory_budget(SMALL_SORT_BUDGET)
                                         .open_cursor();
    std::optional<Morsel> first =
        dftracer::utils::default_runtime().submit(cursor->next(ROWS)).get();
    REQUIRE(first.has_value());
    const std::size_t after_merge = spill_slot_bytes();
    REQUIRE(before_merge > 0);
    CHECK(after_merge > 0);
    // Without a release the merged runs add to the runs they replace, which
    // at least doubles the gauge; with it the gauge holds about one copy.
    CHECK(after_merge < before_merge + before_merge / 2);
    first.reset();
    cursor.reset();
    CHECK(spill_slot_bytes() == 0);
}
