// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_BENCHMARKS_MEMORY_WORKLOADS_H
#define DFTRACER_UTILS_BENCHMARKS_MEMORY_WORKLOADS_H

#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/genesis/genesis.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

/**
 * Workloads that the memory benches share: a synthetic frame source for
 * group_by, sort and join plans, and a generated genesis run directory.
 * Inline only. The including .cpp decides about
 * DFTRACER_UTILS_MEMORY_PROBE_DEFINE_NEW.
 */
namespace dftu::memory_workloads {

/// Prints the error and exits with 1 when a setup call failed.
template <class T>
inline void require_ok(const dftracer::utils::dstd::Expected<
                           T, dftracer::utils::dstd::Error>& result,
                       const char* what) {
    if (result.has_value()) return;
    const auto& error = result.error();
    std::fprintf(stderr, "%s failed: %.*s %.*s\n", what,
                 static_cast<int>(error.name().size()), error.name().data(),
                 static_cast<int>(error.message().size()),
                 error.message().data());
    std::exit(1);
}

constexpr std::int64_t BATCH_SIZE = 8192;
constexpr std::uint64_t PLAN_BUDGET = 1ULL << 36;
constexpr std::uint64_t GENESIS_SHARE = 256ULL * 1024 * 1024;
constexpr std::int64_t RANKS = 4;

inline std::uint64_t mix(std::uint64_t x) {
    return dftracer::utils::dstd::hash::fnv1a_mix(x);
}

class GenCursor : public dftracer::utils::dataframe::Cursor {
   public:
    GenCursor(std::int64_t rows, std::int64_t keys, std::uint64_t seed)
        : rows_(rows), keys_(keys), seed_(seed) {}

    dftracer::utils::coro::CoroTask<dftracer::utils::dataframe::MorselResult>
    next(std::int64_t) override {
        if (at_ >= rows_)
            co_return dftracer::utils::dataframe::MorselResult(
                dftracer::utils::dstd::nullopt);
        const std::int64_t n = std::min(BATCH_SIZE, rows_ - at_);
        dftracer::utils::dstd::Vector<std::int64_t> k(
            static_cast<std::size_t>(n));
        dftracer::utils::dstd::Vector<std::int64_t> t(
            static_cast<std::size_t>(n));
        dftracer::utils::dstd::Vector<std::int64_t> v(
            static_cast<std::size_t>(n));
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
        dftracer::utils::dataframe::Morsel m;
        m.rows = n;
        m.columns.push_back(
            dftracer::utils::dataframe::Series::flat_i64(k.data(), n));
        m.columns.push_back(
            dftracer::utils::dataframe::Series::flat_i64(t.data(), n));
        m.columns.push_back(
            dftracer::utils::dataframe::Series::flat_i64(v.data(), n));
        co_return dftracer::utils::dataframe::MorselResult(std::move(m));
    }

   private:
    std::int64_t rows_;
    std::int64_t keys_;
    std::uint64_t seed_;
    std::int64_t at_ = 0;
};

class GenSource : public dftracer::utils::dataframe::Source {
   public:
    GenSource(std::int64_t rows, std::int64_t keys, std::uint64_t seed)
        : rows_(rows), keys_(keys), seed_(seed) {}

    dftracer::utils::dataframe::Schema schema() const override {
        const std::int64_t zero = 0;
        const dftracer::utils::dataframe::DataType i =
            dftracer::utils::dataframe::Series::flat_i64(&zero, 1).data_type();
        dftracer::utils::dataframe::Schema out;
        out.fields = {dftracer::utils::dataframe::Field{"k", i, true},
                      dftracer::utils::dataframe::Field{"t", i, true},
                      dftracer::utils::dataframe::Field{"v", i, true}};
        return out;
    }

    dftracer::utils::dataframe::ScanResult scan(
        const dftracer::utils::dataframe::ScanRequest& req) const override {
        dftracer::utils::dataframe::ScanResult r;
        r.cursor =
            dftracer::utils::dstd::make_unique<GenCursor>(rows_, keys_, seed_);
        r.filters.assign(req.filters.size(),
                         dftracer::utils::dataframe::Pushed::No);
        return r;
    }

   private:
    std::int64_t rows_;
    std::int64_t keys_;
    std::uint64_t seed_;
};

inline dftracer::utils::dataframe::LazyFrame gen(std::int64_t rows,
                                                 std::int64_t keys,
                                                 std::uint64_t seed) {
    return dftracer::utils::dataframe::LazyFrame::scan(
        dftracer::utils::dstd::make_shared<GenSource>(rows, keys, seed));
}

inline dftracer::utils::coro::CoroTask<std::int64_t> drain_count(
    dftracer::utils::dataframe::LazyFrame lf) {
    std::int64_t n = 0;
    auto g = lf.stream(BATCH_SIZE);
    while (auto frame = co_await g.next()) {
        if (!*frame) {
            std::fprintf(stderr, "drain_count: %s\n", frame->error().c_str());
            std::exit(1);
        }
        n += (*frame)->num_rows();
    }
    co_return n;
}

inline dftracer::utils::coro::CoroTask<void> genesis_run(
    dftracer::utils::coro::Scope& ctx, dftracer::utils::dstd::String root) {
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> roots{
        std::move(root)};
    auto discovery = co_await dftracer::utils::trace::genesis::discover(
        ctx, std::move(roots));
    if (discovery.groups.empty()) {
        std::fprintf(stderr, "genesis found no run group\n");
        std::exit(1);
    }
    for (auto& group : discovery.groups) {
        auto result = co_await dftracer::utils::trace::genesis::process_group(
            ctx, std::move(group), GENESIS_SHARE);
        std::size_t lines = 0;
        for (auto& run : result.runs) lines += run.lines().size();
        if (!result.skips.empty() || lines == 0) {
            std::fprintf(stderr, "genesis group skipped or empty\n");
            std::exit(1);
        }
    }
}

inline void write_gz(const dftracer::utils::dstd::String& path,
                     const dftracer::utils::dstd::String& content) {
    require_ok(dftracer::utils::dstd::fs::create_directories(
                   dftracer::utils::dstd::PathView(path).parent()),
               "create_directories");
    dftracer::utils::utilities::fileio::compress::GzipMemberCompressor comp(1);
    auto member = comp.compress_member(
        reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
    if (!member) {
        std::fprintf(stderr, "compress failed\n");
        std::exit(1);
    }
    if (!dftracer::utils::dstd::fs::write_file(
            path, dftracer::utils::dstd::StringView(
                      reinterpret_cast<const char*>(member->data()),
                      member->size()))) {
        std::fprintf(stderr, "write failed\n");
        std::exit(1);
    }
}

inline dftracer::utils::dstd::String genesis_line(
    std::int64_t pid, const dftracer::utils::dstd::String& name,
    std::int64_t ts, std::int64_t dur, const char* cat = "CPP_APP") {
    return R"({"name":")" + name + R"(","cat":")" + cat + R"(","pid":)" +
           dftracer::utils::dstd::to_string(pid) + R"(,"tid":)" +
           dftracer::utils::dstd::to_string(pid) + R"(,"ts":)" +
           dftracer::utils::dstd::to_string(ts) + R"(,"dur":)" +
           dftracer::utils::dstd::to_string(dur) +
           R"(,"ph":1,"args":{"hhash":"h1"}})" + "\n";
}

inline dftracer::utils::dstd::String genesis_text(std::int64_t calls) {
    static const char* const FUNCS[] = {"SolveVelocity", "ForceMult",
                                        "SolveEnergy", "cudaMemcpy"};
    const std::int64_t span = calls * 10 + 1000;
    dftracer::utils::dstd::String out = "[\n";
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
