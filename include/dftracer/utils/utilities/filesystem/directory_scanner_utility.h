#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H

#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/runtime/coro/adaptive_gate.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/coro/when_all.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/utilities/filesystem/types.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dftracer::utils::utilities::filesystem {

/// Directories that hold generated `.pfw.gz` output, never input traces, and
/// so must be skipped by a recursive scan or it would ingest derived copies as
/// source files (double-counting events). Two kinds:
/// - Index-artifact dirs (`.dftindex`, `.dftindex-cache`, `.dftindex_staging`
///   and older `.dftindex-views`): index data, rollups and materialized views.
/// - `split/`: the default output of the `dftracer_split` CLI. A
///   recursive scan re-run after a split would otherwise pick up both the
///   original trace and its split copy.
/// - `schemas/`: record schema specs (`*.json`, `*.yaml`) beside an index.
inline bool is_excluded_scan_dir(const fs::path& p) {
    const std::string name = p.filename().string();
    return name.rfind(".dftindex", 0) == 0 || name == "split" ||
           name == "schemas";
}

/// The stat batches a directory scan has run in this process. A scan of one
/// directory runs one batch per slice of its entries, so the count is above
/// one when the stats went through the executor in parallel.
inline std::atomic<std::size_t>& scan_stat_batches() {
    static std::atomic<std::size_t> batches{0};
    return batches;
}

/// The threads that have run a stat batch in this process. A scan whose
/// batches run on the thread that awaited it stats its files on that thread's
/// run loop, one at a time. For tests.
class ScanStatThreads {
   public:
    void note() {
        std::lock_guard<std::mutex> lock(mu_);
        ids_.insert(std::this_thread::get_id());
    }
    std::size_t count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return ids_.size();
    }
    bool contains(std::thread::id id) const {
        std::lock_guard<std::mutex> lock(mu_);
        return ids_.count(id) > 0;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        ids_.clear();
    }

   private:
    mutable std::mutex mu_;
    dftracer::utils::dstd::Set<std::thread::id> ids_;
};

inline ScanStatThreads& scan_stat_threads() {
    static ScanStatThreads threads;
    return threads;
}

struct DirectoryScannerUtilityInput {
    fs::path path;
    bool recursive = false;
    bool populate_size = true;

    DirectoryScannerUtilityInput() = default;

    explicit DirectoryScannerUtilityInput(fs::path p, bool rec = false,
                                          bool with_size = true)
        : path(std::move(p)), recursive(rec), populate_size(with_size) {}

    bool operator==(const DirectoryScannerUtilityInput& other) const {
        return path == other.path && recursive == other.recursive &&
               populate_size == other.populate_size;
    }

    bool operator!=(const DirectoryScannerUtilityInput& other) const {
        return !(*this == other);
    }
};

/**
 * @brief Utility that scans a directory (optionally recursively) and returns
 * metadata about each file/subdirectory found.
 */
class DirectoryScannerUtility {
   public:
    /**
     * @brief Scan directory and return list of file entries.
     *
     * A recursive scan fans out per subdirectory over @p ctx so many
     * directories are read concurrently (fast on parallel filesystems like
     * Lustre).
     *
     * @throws fs::filesystem_error if directory doesn't exist or is
     * inaccessible
     */
    coro::CoroTask<std::vector<FileEntry>> operator()(
        CoroScope& ctx, const DirectoryScannerUtilityInput& input) const {
        dftracer::utils::InputFileStat root_stat;
        if (!dftracer::utils::stat_input(input.path, root_stat)) {
            std::error_code root_ec(errno, std::generic_category());
            if (root_ec == std::errc::no_such_file_or_directory)
                throw fs::filesystem_error("Directory does not exist",
                                           input.path, root_ec);
            throw fs::filesystem_error("Cannot stat directory", input.path,
                                       root_ec);
        }
        if (!root_stat.is_directory) {
            throw fs::filesystem_error(
                "Path is not a directory", input.path,
                std::make_error_code(std::errc::not_a_directory));
        }

        if (input.recursive) {
            Executor* exec = ctx.get_executor();
            auto gate = std::make_shared<coro::AdaptiveGate>(
                exec ? exec->get_num_threads() : 1);
            co_return co_await scan_parallel(ctx, std::move(gate), input.path,
                                             input.populate_size);
        }

        std::vector<fs::directory_entry> raw_entries;
        for (const auto& entry : fs::directory_iterator(input.path)) {
            raw_entries.push_back(entry);
        }

        std::vector<coro::SpawnFuture<FileEntry>> tasks;
        tasks.reserve(raw_entries.size());
        for (auto& entry : raw_entries) {
            auto entry_copy = std::move(entry);
            tasks.push_back(
                ctx.spawn([entry_copy = std::move(entry_copy),
                           populate_size = input.populate_size](
                              CoroScope&) mutable -> coro::CoroTask<FileEntry> {
                    co_return FileEntry(entry_copy, populate_size);
                }));
        }
        std::vector<FileEntry> entries =
            co_await coro::when_all(std::move(tasks));

        co_return entries;
    }

    /// Scope-less overload: opens its own CoroScope on the current executor.
    coro::CoroTask<std::vector<FileEntry>> operator()(
        const DirectoryScannerUtilityInput& input) const {
        return with_scope(*this, input);
    }

   private:
    static constexpr std::size_t STAT_BATCH = 16;

    struct StatBatch {
        std::vector<FileEntry> entries;
        std::vector<fs::path> subdirs;
    };

    /// Classifies and stats one slice of a directory listing. A symlink to a
    /// directory is listed, not entered.
    static StatBatch stat_batch(const std::vector<fs::directory_entry>& listing,
                                std::size_t begin, std::size_t end,
                                bool populate_size) {
        scan_stat_batches().fetch_add(1, std::memory_order_relaxed);
        scan_stat_threads().note();
        StatBatch out;
        for (std::size_t i = begin; i < end; ++i) {
            const fs::directory_entry& entry = listing[i];
            std::error_code ec;
            if (!entry.is_symlink(ec) && entry.is_directory(ec)) {
                if (is_excluded_scan_dir(entry.path())) continue;
                out.entries.emplace_back(entry, populate_size);
                out.subdirs.push_back(entry.path());
            } else if (populate_size) {
                out.entries.emplace_back(entry.path(), true);
            } else {
                out.entries.emplace_back(entry, false);
            }
        }
        return out;
    }

    /// Recursive parallel scan: list one directory level, stat its entries in
    /// batches over @p ctx, as many at once as @p gate admits, spawn a child
    /// scan per subdirectory, then merge in listing order. Uses error codes
    /// instead of exceptions so an unreadable directory is skipped rather than
    /// aborting the whole scan.
    static coro::CoroTask<std::vector<FileEntry>> scan_parallel(
        CoroScope& ctx, std::shared_ptr<coro::AdaptiveGate> gate, fs::path dir,
        bool populate_size) {
        std::vector<fs::directory_entry> listing;
        std::error_code ec;
        fs::directory_iterator it(dir, ec);
        const fs::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) listing.push_back(*it);

        std::vector<coro::SpawnFuture<StatBatch>> batches;
        batches.reserve(listing.size() / STAT_BATCH + 1);
        auto shared_listing =
            std::make_shared<const std::vector<fs::directory_entry>>(
                std::move(listing));
        for (std::size_t begin = 0; begin < shared_listing->size();
             begin += STAT_BATCH) {
            const std::size_t stop =
                std::min(begin + STAT_BATCH, shared_listing->size());
            batches.push_back(
                ctx.spawn([gate, shared_listing, begin, stop, populate_size](
                              CoroScope&) -> coro::CoroTask<StatBatch> {
                    auto admitted = co_await gate->enter(stop - begin);
                    co_return stat_batch(*shared_listing, begin, stop,
                                         populate_size);
                }));
        }
        auto stated = co_await coro::when_all(std::move(batches));

        std::vector<FileEntry> files;
        std::vector<coro::SpawnFuture<std::vector<FileEntry>>> subdirs;
        for (auto& batch : stated) {
            files.insert(files.end(),
                         std::make_move_iterator(batch.entries.begin()),
                         std::make_move_iterator(batch.entries.end()));
            for (auto& sub : batch.subdirs) {
                subdirs.push_back(ctx.spawn(
                    [gate, p = std::move(sub), populate_size](CoroScope& child)
                        -> coro::CoroTask<std::vector<FileEntry>> {
                        co_return co_await scan_parallel(child, gate, p,
                                                         populate_size);
                    }));
            }
        }

        if (!subdirs.empty()) {
            auto nested = co_await coro::when_all(std::move(subdirs));
            for (auto& sub : nested) {
                files.insert(files.end(), std::make_move_iterator(sub.begin()),
                             std::make_move_iterator(sub.end()));
            }
        }
        co_return files;
    }
};

/// Scans `input` with a `Scanner` (a scanner utility that takes a CoroScope)
/// on a pool of its own with `executor_threads` workers and `io_threads` I/O
/// threads, and blocks until done. A CLI drives its run coroutine with get()
/// on the main thread, whose run loop is one thread, so a scan awaited there
/// stats every file one at a time. Here the stat batches fan out over the pool,
/// which grows only as far as the scan's AdaptiveGate admits batches.
template <class Scanner, class Input>
coro::CoroTask<void> scan_on_pool_task(CoroScope& scope, Input input,
                                       std::vector<FileEntry>* entries) {
    Scanner scanner;
    *entries = co_await scanner(scope, input);
}

template <class Scanner, class Input>
std::vector<FileEntry> scan_on_pool(std::size_t executor_threads,
                                    std::size_t io_threads, Input input) {
    std::vector<FileEntry> entries;
    ExecutorConfig config;
    config.num_threads = executor_threads;
    config.min_workers = 1;
    config.io_pool_size = io_threads;
    Runtime pool(config, /*enable_watchdog=*/false);
    pool.run_blocking("scan trace files", [&](CoroScope& scope) {
        return scan_on_pool_task<Scanner, Input>(scope, std::move(input),
                                                 &entries);
    });
    return entries;
}

}  // namespace dftracer::utils::utilities::filesystem

namespace std {
template <>
struct hash<
    dftracer::utils::utilities::filesystem::DirectoryScannerUtilityInput> {
    std::size_t operator()(
        const dftracer::utils::utilities::filesystem::
            DirectoryScannerUtilityInput& dir) const noexcept {
        ::dftracer::utils::utilities::hash::HasherUtility hasher;
        hasher.update(dir.path.string());
        hasher.update(dir.recursive);
        return hasher.get_hash().value;
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H
