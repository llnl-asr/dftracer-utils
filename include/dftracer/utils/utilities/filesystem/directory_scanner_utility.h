// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/coro/adaptive_gate.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
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
/// - `split/`: the default output of the `dftracer_split` CLI.
/// - `schemas/`: record schema specs (`*.json`, `*.yaml`) beside an index.
inline bool is_excluded_scan_dir(dstd::PathView p) {
    const dstd::StringView name = p.filename();
    return name.rfind(".dftindex", 0) == 0 || name == "split" ||
           name == "schemas";
}

/// The stat batches directory scans have run in this process, one per slice of
/// a directory's entries. For tests.
inline std::atomic<std::size_t>& scan_stat_batches() {
    static std::atomic<std::size_t> batches{0};
    return batches;
}

/// The threads that have run a stat batch in this process. For tests.
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
        dftracer::utils::dstd::Set<std::thread::id> dropped;
        std::lock_guard<std::mutex> lock(mu_);
        dropped.swap(ids_);
    }

   private:
    mutable std::mutex mu_;
    dftracer::utils::dstd::Set<std::thread::id> ids_;
};

inline ScanStatThreads& scan_stat_threads() {
    ScanStatThreads& threads =
        dftracer::utils::dstd::Singleton<ScanStatThreads>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dftracer::utils::dstd::add_shutdown_hook(
            dftracer::utils::dstd::ShutdownStage::FreeCaches, [] {
                dftracer::utils::dstd::Singleton<ScanStatThreads>::get()
                    .clear();
            });
        return true;
    }();
    return threads;
}

struct DirectoryScannerUtilityInput {
    dstd::Path path;
    bool recursive = false;
    bool populate_size = true;

    DirectoryScannerUtilityInput() = default;

    explicit DirectoryScannerUtilityInput(dstd::StringView p, bool rec = false,
                                          bool with_size = true)
        : path(p), recursive(rec), populate_size(with_size) {}

    bool operator==(const DirectoryScannerUtilityInput& other) const {
        return path.str() == other.path.str() && recursive == other.recursive &&
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
     * @throws common::DFTUtilsException if directory doesn't exist, is not a
     * directory, or cannot be read
     */
    coro::CoroTask<std::vector<FileEntry>> operator()(
        coro::Scope& ctx, const DirectoryScannerUtilityInput& input) const {
        dftracer::utils::common::InputFileStat root_stat;
        if (!dftracer::utils::common::stat_input(input.path, root_stat)) {
            const int root_errno = errno;
            throw common::DFTUtilsException(
                root_errno == ENOENT ? common::ErrorCode::NotFound
                                     : common::ErrorCode::Io,
                common::str_cat(root_errno == ENOENT
                                    ? "Directory does not exist: "
                                    : "Cannot stat directory: ",
                                input.path.str()));
        }
        if (!root_stat.is_directory) {
            throw common::DFTUtilsException(
                common::ErrorCode::InvalidArgument,
                common::str_cat("Path is not a directory: ", input.path.str()));
        }

        if (input.recursive) {
            coro::Executor* exec = ctx.executor();
            auto gate = std::make_shared<coro::AdaptiveGate>(
                exec ? exec->concurrency() : 1);
            co_return co_await scan_parallel(ctx, std::move(gate), input.path,
                                             input.populate_size);
        }

        dstd::Vector<Listed> raw_entries = list_dir(input.path.view(), true);

        dstd::Vector<coro::SpawnFuture<FileEntry>> tasks;
        tasks.reserve(raw_entries.size());
        for (auto& entry : raw_entries) {
            tasks.push_back(
                ctx.spawn_future([entry_copy = std::move(entry),
                                  populate_size = input.populate_size]() mutable
                                     -> coro::CoroTask<FileEntry> {
                    co_return FileEntry(std::move(entry_copy.path),
                                        entry_copy.kind, populate_size);
                }));
        }
        auto entries = co_await coro::when_all(std::move(tasks));

        co_return std::vector<FileEntry>(
            std::make_move_iterator(entries.begin()),
            std::make_move_iterator(entries.end()));
    }

    /// Scope-less overload: opens its own scope on the current executor.
    coro::CoroTask<std::vector<FileEntry>> operator()(
        const DirectoryScannerUtilityInput& input) const {
        return coro::with_scope(
            [this, input](coro::Scope& s) { return (*this)(s, input); });
    }

   private:
    static constexpr std::size_t STAT_BATCH = 16;

    struct Listed {
        dstd::Path path;
        dstd::fs::FileKind kind;
    };

    struct StatBatch {
        std::vector<FileEntry> entries;
        dstd::Vector<dstd::Path> subdirs;
    };

    /// The entries of `dir` with the kind readdir reported, or an lstat of
    /// the entry when it reported none. A strict listing throws when the
    /// directory cannot be read, a lenient one keeps what was read.
    static dstd::Vector<Listed> list_dir(dstd::PathView dir, bool strict) {
        dstd::Vector<Listed> listing;
        auto opened = dstd::fs::Dir::open(dir);
        if (!opened) {
            if (strict) throw common::DFTUtilsException(opened.error());
            return listing;
        }
        auto entries = opened->entries();
        if (!entries) {
            if (strict) throw common::DFTUtilsException(entries.error());
            return listing;
        }
        for (const dstd::fs::DirEntry& entry : *entries) {
            dstd::fs::FileKind kind = entry.kind;
            if (kind == dstd::fs::FileKind::Unknown) {
                const auto found = opened->lstat(entry.name);
                if (found) kind = found->kind;
            }
            listing.push_back({dir / entry.name, kind});
        }
        if (strict && entries->error() != 0) {
            throw common::DFTUtilsException(
                common::ErrorCode::Io,
                common::str_cat("Cannot read directory: ", dir.str()));
        }
        return listing;
    }

    /// Classifies and stats one slice of a directory listing. A symlink to a
    /// directory is listed, not entered.
    static StatBatch stat_batch(const dstd::Vector<Listed>& listing,
                                std::size_t begin, std::size_t end,
                                bool populate_size) {
        scan_stat_batches().fetch_add(1, std::memory_order_relaxed);
        scan_stat_threads().note();
        StatBatch out;
        for (std::size_t i = begin; i < end; ++i) {
            const Listed& entry = listing[i];
            if (entry.kind == dstd::fs::FileKind::Directory) {
                if (is_excluded_scan_dir(entry.path.view())) continue;
                out.entries.emplace_back(entry.path, entry.kind, populate_size);
                out.subdirs.push_back(entry.path);
            } else if (populate_size) {
                out.entries.emplace_back(entry.path.str(), true);
            } else {
                out.entries.emplace_back(entry.path, entry.kind, false);
            }
        }
        return out;
    }

    /// Recursive parallel scan merged in listing order. Stat batches run over
    /// @p ctx, as many at once as @p gate admits. An unreadable directory is
    /// skipped rather than aborting the scan.
    static coro::CoroTask<std::vector<FileEntry>> scan_parallel(
        coro::Scope& ctx, std::shared_ptr<coro::AdaptiveGate> gate,
        dstd::Path dir, bool populate_size) {
        dstd::Vector<Listed> listing = list_dir(dir.view(), false);

        dstd::Vector<coro::SpawnFuture<StatBatch>> batches;
        batches.reserve(listing.size() / STAT_BATCH + 1);
        auto shared_listing =
            dstd::make_shared<const dstd::Vector<Listed>>(std::move(listing));
        for (std::size_t begin = 0; begin < shared_listing->size();
             begin += STAT_BATCH) {
            const std::size_t stop =
                std::min(begin + STAT_BATCH, shared_listing->size());
            batches.push_back(ctx.spawn_future(
                [gate, shared_listing, begin, stop,
                 populate_size]() -> coro::CoroTask<StatBatch> {
                    auto admitted = co_await gate->enter(stop - begin);
                    co_return stat_batch(*shared_listing, begin, stop,
                                         populate_size);
                }));
        }
        auto stated = co_await coro::when_all(std::move(batches));

        std::vector<FileEntry> files;
        dstd::Vector<coro::SpawnFuture<std::vector<FileEntry>>> subdirs;
        for (auto& batch : stated) {
            files.insert(files.end(),
                         std::make_move_iterator(batch.entries.begin()),
                         std::make_move_iterator(batch.entries.end()));
            for (auto& sub : batch.subdirs) {
                subdirs.push_back(ctx.spawn_future(
                    [gate, &ctx, p = std::move(sub), populate_size]()
                        -> coro::CoroTask<std::vector<FileEntry>> {
                        co_return co_await scan_parallel(ctx, gate, p,
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

/// Scans `input` with a `Scanner` (a scanner utility that takes a @ref
/// dftracer::utils::coro::Scope "Scope") on a pool of its own with
/// `executor_threads` workers and `io_threads` I/O threads, and blocks until
/// done. Stat batches fan out over the pool, which a scan awaited on a CLI's
/// one-thread main run loop cannot do.
template <class Scanner, class Input>
coro::CoroTask<void> scan_on_pool_task(coro::Scope& scope, Input input,
                                       std::vector<FileEntry>* entries) {
    Scanner scanner;
    *entries = co_await scanner(scope, input);
}

template <class Scanner, class Input>
std::vector<FileEntry> scan_on_pool(std::size_t executor_threads,
                                    std::size_t io_threads, Input input) {
    std::vector<FileEntry> entries;
    RuntimeSettings settings;
    settings.threads = executor_threads;
    settings.io_threads = io_threads;
    configure_default_runtime(settings).block_on(coro::named(
        "scan trace files", coro::with_scope([&](coro::Scope& scope) {
            return scan_on_pool_task<Scanner, Input>(scope, std::move(input),
                                                     &entries);
        })));
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
        hasher.update(dir.path.str());
        hasher.update(dir.recursive);
        return hasher.get_hash().value;
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H
