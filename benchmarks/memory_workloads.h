#ifndef DFTRACER_UTILS_BENCHMARKS_MEMORY_WORKLOADS_H
#define DFTRACER_UTILS_BENCHMARKS_MEMORY_WORKLOADS_H

#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/genesis/genesis.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * Workloads that the memory benches share: a synthetic frame source for
 * group_by, sort and join plans, and a generated genesis run directory.
 * Inline only. The including .cpp decides about
 * DFTRACER_UTILS_MEMORY_PROBE_DEFINE_NEW.
 */
namespace dftu::memory_workloads {

using namespace dftracer::utils;
using dftracer::utils::coro::CoroTask;
namespace df = dftracer::utils::dataframe;

constexpr std::int64_t MORSEL_ROWS = 8192;
constexpr std::uint64_t PLAN_BUDGET = 1ULL << 36;
constexpr std::uint64_t GENESIS_SHARE = 256ULL * 1024 * 1024;
constexpr std::int64_t RANKS = 4;

inline std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

class GenCursor : public df::Cursor {
   public:
    GenCursor(std::int64_t rows, std::int64_t keys, std::uint64_t seed)
        : rows_(rows), keys_(keys), seed_(seed) {}

    CoroTask<std::optional<df::Morsel>> next(std::int64_t) override {
        if (at_ >= rows_) co_return std::nullopt;
        const std::int64_t n = std::min(MORSEL_ROWS, rows_ - at_);
        std::vector<std::int64_t> k(static_cast<std::size_t>(n));
        std::vector<std::int64_t> t(static_cast<std::size_t>(n));
        std::vector<std::int64_t> v(static_cast<std::size_t>(n));
        for (std::int64_t i = 0; i < n; ++i) {
            const std::uint64_t h =
                mix(seed_ + static_cast<std::uint64_t>(at_ + i));
            const auto j = static_cast<std::size_t>(i);
            k[j] = static_cast<std::int64_t>(h %
                                             static_cast<std::uint64_t>(keys_));
            t[j] = at_ + i;
            v[j] = static_cast<std::int64_t>(h >> 20) % 1000003;
        }
        at_ += n;
        df::Morsel m;
        m.rows = n;
        m.columns.push_back(df::Series::flat_i64(k.data(), n));
        m.columns.push_back(df::Series::flat_i64(t.data(), n));
        m.columns.push_back(df::Series::flat_i64(v.data(), n));
        co_return m;
    }

   private:
    std::int64_t rows_;
    std::int64_t keys_;
    std::uint64_t seed_;
    std::int64_t at_ = 0;
};

class GenSource : public df::Source {
   public:
    GenSource(std::int64_t rows, std::int64_t keys, std::uint64_t seed)
        : rows_(rows), keys_(keys), seed_(seed) {}

    df::Schema schema() const override {
        const std::int64_t zero = 0;
        const df::DataType i = df::Series::flat_i64(&zero, 1).data_type();
        df::Schema out;
        out.fields = {df::Field{"k", i, true}, df::Field{"t", i, true},
                      df::Field{"v", i, true}};
        return out;
    }

    df::ScanResult scan(const df::ScanRequest& req) const override {
        df::ScanResult r;
        r.cursor = std::make_unique<GenCursor>(rows_, keys_, seed_);
        r.filters.assign(req.filters.size(), df::Pushed::No);
        return r;
    }

   private:
    std::int64_t rows_;
    std::int64_t keys_;
    std::uint64_t seed_;
};

inline df::LazyFrame gen(std::int64_t rows, std::int64_t keys,
                         std::uint64_t seed) {
    return df::LazyFrame::scan(std::make_shared<GenSource>(rows, keys, seed));
}

inline CoroTask<std::int64_t> drain_count(df::LazyFrame lf) {
    std::int64_t n = 0;
    auto g = lf.stream(MORSEL_ROWS);
    while (auto frame = co_await g.next()) n += frame->num_rows();
    co_return n;
}

inline CoroTask<void> genesis_run(CoroScope& ctx, std::string root) {
    std::vector<std::string> roots{std::move(root)};
    auto discovery = co_await trace::genesis::discover(ctx, std::move(roots));
    if (discovery.groups.empty()) {
        std::fprintf(stderr, "genesis found no run group\n");
        std::exit(1);
    }
    for (auto& group : discovery.groups) {
        auto result = co_await trace::genesis::process_group(
            ctx, std::move(group), GENESIS_SHARE);
        std::size_t lines = 0;
        for (auto& run : result.runs) lines += run.lines().size();
        if (!result.skips.empty() || lines == 0) {
            std::fprintf(stderr, "genesis group skipped or empty\n");
            std::exit(1);
        }
    }
}

inline void write_gz(const std::string& path, const std::string& content) {
    fs::create_directories(fs::path(path).parent_path());
    utilities::fileio::compress::GzipMemberCompressor comp(1);
    auto member = comp.compress_member(
        reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
    if (!member) {
        std::fprintf(stderr, "compress failed\n");
        std::exit(1);
    }
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(member->data()),
               static_cast<std::streamsize>(member->size()));
}

inline std::string genesis_line(std::int64_t pid, const std::string& name,
                                std::int64_t ts, std::int64_t dur,
                                const char* cat = "CPP_APP") {
    return R"({"name":")" + name + R"(","cat":")" + cat + R"(","pid":)" +
           std::to_string(pid) + R"(,"tid":)" + std::to_string(pid) +
           R"(,"ts":)" + std::to_string(ts) + R"(,"dur":)" +
           std::to_string(dur) + R"(,"ph":1,"args":{"hhash":"h1"}})" + "\n";
}

inline std::string genesis_text(std::int64_t calls) {
    static const char* const FUNCS[] = {"SolveVelocity", "ForceMult",
                                        "SolveEnergy", "cudaMemcpy"};
    const std::int64_t span = calls * 10 + 1000;
    std::string out = "[\n";
    for (std::int64_t pid = 1; pid <= RANKS; ++pid) {
        out += genesis_line(pid, "start", 0, 0, "dftracer");
        out += genesis_line(pid, "end", span, 0, "dftracer");
    }
    for (std::int64_t pid = 1; pid <= RANKS; ++pid) {
        out += genesis_line(pid, "main", 0, span);
        for (std::int64_t i = 0; i < calls / RANKS; ++i)
            out += genesis_line(pid, FUNCS[i % 4], 10 + i * 10, 5);
    }
    return out;
}

}  // namespace dftu::memory_workloads

#endif
