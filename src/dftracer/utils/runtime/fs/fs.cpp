// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/adaptive_gate.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/combinators.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/io/ops.h>

#include <algorithm>
#include <atomic>
#include <type_traits>
#include <utility>

namespace dftracer::utils::runtime::fs {

namespace {

using dstd::fs::FileKind;

constexpr std::size_t STAT_BATCH = 16;
constexpr std::size_t STREAM_SLOTS = 256;

struct Found {
    dstd::Path path;
    std::uint64_t size = 0;
    std::uint64_t mtime_ns = 0;
    FileKind kind = FileKind::Unknown;
};

using Batch = dstd::Expected<dstd::Vector<Found>, dstd::Error>;
using BatchChannel = coro::Channel<Batch>;
using BatchProducer = decltype(std::declval<BatchChannel&>().producer());

template <typename Fn>
struct IoCall {
    Fn* fn;
    dstd::Optional<std::invoke_result_t<Fn&>> result;

    static ssize_t run(void* context) noexcept {
        auto* call = static_cast<IoCall*>(context);
        call->result.emplace((*call->fn)());
        return 0;
    }
};

// A call with no I/O thread free stays queued and holds no worker.
template <typename Fn>
coro::CoroTask<std::invoke_result_t<Fn&>> on_io(Fn fn) {
    IoCall<Fn> call{&fn, dstd::nullopt};
    runtime::io::IoAwaitable op =
        runtime::io::call(&IoCall<Fn>::run, static_cast<void*>(&call));
    co_await op;
    co_return std::move(*call.result);
}

dstd::Expected<dstd::Vector<Entry>, dstd::Error> list_sync(
    dstd::PathView dir) noexcept {
    auto opened = dstd::fs::Dir::open(dir);
    if (!opened) return dstd::Unexpected(opened.error());
    auto entries = opened->entries();
    if (!entries) return dstd::Unexpected(entries.error());
    dstd::Vector<Entry> listing;
    for (const dstd::fs::DirEntry& entry : *entries) {
        FileKind kind = entry.kind;
        if (kind == FileKind::Unknown) {
            const auto found = opened->lstat(entry.name);
            if (found) kind = found->kind;
        }
        listing.push_back(Entry{dir / entry.name, kind});
    }
    if (entries->error() != 0) {
        return dstd::Unexpected(
            dstd::Error(dstd::Condition::Io,
                        common::str_cat("Cannot read directory: ", dir.str())));
    }
    return listing;
}

bool any_match(const dstd::Vector<dstd::String>& patterns,
               dstd::StringView name) noexcept {
    return std::any_of(patterns.begin(), patterns.end(),
                       [name](const dstd::String& pattern) {
                           return dstd::fs::match_name(pattern, name);
                       });
}

struct WalkPlan {
    coro::Scope* scope = nullptr;
    dstd::SharedPtr<BatchChannel> channel;
    coro::AdaptiveGate gate;
    dstd::Vector<dstd::String> patterns;
    dstd::Vector<dstd::String> skip_dirs;
    bool recursive = true;
    bool with_stat = true;
    bool skip_unreadable = false;
    bool every_entry = false;
    std::atomic<bool> stopped{false};

    WalkPlan(coro::Scope& s, std::size_t cap)
        : scope(&s),
          channel(coro::make_channel<Batch>(STREAM_SLOTS)),
          gate(cap) {}
};

// Owned by the frame of a stream, so that dropping the stream, read or not,
// lets producers parked on a full channel end.
class StreamEnd {
   public:
    explicit StreamEnd(dstd::SharedPtr<WalkPlan> plan)
        : plan_(std::move(plan)) {}
    StreamEnd(StreamEnd&&) noexcept = default;
    StreamEnd(const StreamEnd&) = delete;
    StreamEnd& operator=(const StreamEnd&) = delete;
    StreamEnd& operator=(StreamEnd&&) = delete;
    ~StreamEnd() {
        if (!plan_) return;
        plan_->stopped.store(true, std::memory_order_relaxed);
        plan_->channel->close();
    }
    BatchChannel& channel() const noexcept { return *plan_->channel; }

   private:
    dstd::SharedPtr<WalkPlan> plan_;
};

coro::CoroTask<void> report(const dstd::SharedPtr<WalkPlan>& plan,
                            BatchProducer& out, dstd::Error error) {
    plan->stopped.store(true, std::memory_order_relaxed);
    Batch failed = dstd::Unexpected(std::move(error));
    co_await out.send(std::move(failed));
}

// Fills the slice in place. Returns the first error that must end the stream.
dstd::Optional<dstd::Error> stat_range(const WalkPlan& plan,
                                       dstd::Vector<Found>& items,
                                       std::size_t begin, std::size_t stop,
                                       dstd::Vector<Found>& out) noexcept {
    for (std::size_t i = begin; i < stop; ++i) {
        Found& item = items[i];
        auto found = dstd::fs::stat(item.path.view());
        if (!found) {
            if (found.error().is(dstd::Condition::NotFound) ||
                plan.skip_unreadable) {
                continue;
            }
            return found.error();
        }
        if (found->kind != FileKind::File) continue;
        item.size = found->size;
        item.mtime_ns = found->mtime_ns;
        item.kind = FileKind::File;
        out.push_back(std::move(item));
    }
    return dstd::nullopt;
}

coro::CoroTask<void> stat_slice(dstd::SharedPtr<WalkPlan> plan,
                                dstd::SharedPtr<dstd::Vector<Found>> items,
                                std::size_t begin, std::size_t stop,
                                BatchProducer out) {
    if (plan->stopped.load(std::memory_order_relaxed)) co_return;
    dstd::Vector<Found> files;
    files.reserve(stop - begin);
    dstd::Optional<dstd::Error> failure;
    {
        auto admitted = co_await plan->gate.enter(stop - begin);
        auto fn = [&]() noexcept {
            return stat_range(*plan, *items, begin, stop, files);
        };
        auto call = on_io(fn);
        failure = co_await std::move(call);
    }
    if (!files.empty()) {
        Batch batch(std::move(files));
        co_await out.send(std::move(batch));
    }
    if (failure) {
        auto reported = report(plan, out, std::move(*failure));
        co_await std::move(reported);
    }
}

coro::CoroTask<void> walk_dir(dstd::SharedPtr<WalkPlan> plan, dstd::Path dir,
                              BatchProducer out) {
    if (plan->stopped.load(std::memory_order_relaxed)) co_return;
    auto list_fn = [&dir]() noexcept { return list_sync(dir.view()); };
    auto list_call = on_io(list_fn);
    auto listed = co_await std::move(list_call);
    if (!listed) {
        if (plan->skip_unreadable) co_return;
        auto reported = report(plan, out, std::move(listed.error()));
        co_await std::move(reported);
        co_return;
    }

    dstd::Vector<Found> ready;
    auto to_stat = dstd::make_shared<dstd::Vector<Found>>();
    for (Entry& entry : *listed) {
        const dstd::StringView name = entry.path.view().filename();
        if (entry.kind == FileKind::Directory) {
            if (any_match(plan->skip_dirs, name)) continue;
            if (plan->recursive) {
                plan->scope->spawn(
                    walk_dir(plan, entry.path, plan->channel->producer()));
            }
            if (plan->every_entry) {
                ready.push_back(Found{std::move(entry.path), 0, 0, entry.kind});
            }
            continue;
        }
        if (plan->every_entry) {
            ready.push_back(Found{std::move(entry.path), 0, 0, entry.kind});
            continue;
        }
        if (entry.kind != FileKind::File && entry.kind != FileKind::Symlink) {
            continue;
        }
        if (!plan->patterns.empty() && !any_match(plan->patterns, name)) {
            continue;
        }
        if (plan->with_stat || entry.kind == FileKind::Symlink) {
            to_stat->push_back(Found{std::move(entry.path), 0, 0, entry.kind});
        } else {
            ready.push_back(Found{std::move(entry.path), 0, 0, entry.kind});
        }
    }
    if (!ready.empty()) {
        Batch batch(std::move(ready));
        co_await out.send(std::move(batch));
    }
    for (std::size_t begin = 0; begin < to_stat->size(); begin += STAT_BATCH) {
        const std::size_t stop = std::min(begin + STAT_BATCH, to_stat->size());
        plan->scope->spawn(
            stat_slice(plan, to_stat, begin, stop, plan->channel->producer()));
    }
}

coro::CoroTask<void> walk_root(dstd::SharedPtr<WalkPlan> plan, dstd::Path root,
                               BatchProducer out) {
    auto stat_fn = [&root]() noexcept { return dstd::fs::stat(root.view()); };
    auto stat_call = on_io(stat_fn);
    auto found = co_await std::move(stat_call);
    if (!found) {
        auto reported = report(plan, out, std::move(found.error()));
        co_await std::move(reported);
        co_return;
    }
    if (found->kind != FileKind::Directory) {
        dstd::Error error(
            dstd::Condition::InvalidArgument,
            common::str_cat("Path is not a directory: ", root.str()));
        auto reported = report(plan, out, std::move(error));
        co_await std::move(reported);
        co_return;
    }
    auto walked = walk_dir(plan, std::move(root), std::move(out));
    co_await std::move(walked);
}

dstd::SharedPtr<WalkPlan> make_plan(coro::Scope& scope, std::size_t limit) {
    coro::Executor* executor = scope.executor();
    const std::size_t cap =
        limit != 0 ? limit
                   : (executor != nullptr ? executor->concurrency() : 1);
    return dstd::make_shared<WalkPlan>(scope, cap);
}

void start(const dstd::SharedPtr<WalkPlan>& plan, dstd::PathView root) {
    plan->scope->spawn(
        walk_root(plan, dstd::Path(root.str()), plan->channel->producer()));
}

dstd::SharedPtr<WalkPlan> start_find(coro::Scope& scope, dstd::PathView root,
                                     FindOptions options) {
    auto plan = make_plan(scope, options.limit);
    plan->patterns = std::move(options.patterns);
    plan->skip_dirs = std::move(options.skip_dirs);
    plan->recursive = options.recursive;
    plan->with_stat = options.with_stat;
    plan->skip_unreadable = options.skip_unreadable;
    start(plan, root);
    return plan;
}

coro::AsyncGenerator<dstd::Expected<Entry, dstd::Error>> entries_of(
    StreamEnd stream) {
    for (;;) {
        auto item = co_await stream.channel().receive();
        if (!item) co_return;
        if (!*item) {
            dstd::Expected<Entry, dstd::Error> failed =
                dstd::Unexpected(item->error());
            co_yield std::move(failed);
            co_return;
        }
        for (Found& found : **item) {
            Entry entry;
            entry.path = std::move(found.path);
            entry.kind = found.kind;
            dstd::Expected<Entry, dstd::Error> value(std::move(entry));
            co_yield std::move(value);
        }
    }
}

coro::AsyncGenerator<dstd::Expected<FoundFile, dstd::Error>> files_of(
    StreamEnd stream) {
    for (;;) {
        auto item = co_await stream.channel().receive();
        if (!item) co_return;
        if (!*item) {
            dstd::Expected<FoundFile, dstd::Error> failed =
                dstd::Unexpected(item->error());
            co_yield std::move(failed);
            co_return;
        }
        for (Found& found : **item) {
            FoundFile file;
            file.path = std::move(found.path);
            file.size = found.size;
            file.mtime_ns = found.mtime_ns;
            dstd::Expected<FoundFile, dstd::Error> value(std::move(file));
            co_yield std::move(value);
        }
    }
}

coro::CoroTask<dstd::Expected<dstd::Vector<FoundFile>, dstd::Error>> drain(
    StreamEnd stream) {
    dstd::Vector<FoundFile> files;
    for (;;) {
        auto item = co_await stream.channel().receive();
        if (!item) break;
        if (!*item) co_return dstd::Unexpected(item->error());
        for (Found& found : **item) {
            FoundFile file;
            file.path = std::move(found.path);
            file.size = found.size;
            file.mtime_ns = found.mtime_ns;
            files.push_back(std::move(file));
        }
    }
    std::sort(files.begin(), files.end(),
              [](const FoundFile& a, const FoundFile& b) {
                  return a.path.str() < b.path.str();
              });
    co_return files;
}

coro::CoroTask<dstd::Expected<dstd::Vector<Entry>, dstd::Error>> list_owned(
    dstd::Path path) {
    auto fn = [&path]() noexcept { return list_sync(path.view()); };
    auto call = on_io(fn);
    co_return co_await std::move(call);
}

coro::CoroTask<dstd::Expected<dstd::fs::FileStat, dstd::Error>> stat_owned(
    dstd::Path path) {
    auto fn = [&path]() noexcept { return dstd::fs::stat(path.view()); };
    auto call = on_io(fn);
    co_return co_await std::move(call);
}

coro::CoroTask<dstd::Expected<dstd::String, dstd::Error>> read_owned(
    dstd::Path path) {
    auto fn = [&path]() noexcept { return dstd::fs::read_file(path.view()); };
    auto call = on_io(fn);
    co_return co_await std::move(call);
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> write_owned(
    dstd::Path path, dstd::StringView data) {
    auto fn = [&path, data]() noexcept {
        return dstd::fs::write_file(path.view(), data);
    };
    auto call = on_io(fn);
    co_return co_await std::move(call);
}

struct StatSlice {
    std::size_t begin = 0;
    std::size_t stop = 0;
};

}  // namespace

coro::CoroTask<dstd::Expected<dstd::Vector<Entry>, dstd::Error>> list_dir(
    dstd::PathView path) {
    return list_owned(dstd::Path(path.str()));
}

coro::CoroTask<dstd::Expected<dstd::fs::FileStat, dstd::Error>> stat(
    dstd::PathView path) {
    return stat_owned(dstd::Path(path.str()));
}

coro::CoroTask<dstd::Expected<dstd::String, dstd::Error>> read_file(
    dstd::PathView path) {
    return read_owned(dstd::Path(path.str()));
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> write_file(
    dstd::PathView path, dstd::StringView data) {
    return write_owned(dstd::Path(path.str()), data);
}

coro::CoroTask<dstd::Vector<dstd::Expected<dstd::fs::FileStat, dstd::Error>>>
stat_many(dstd::Span<const dstd::Path> paths, std::size_t limit) {
    using Result = dstd::Expected<dstd::fs::FileStat, dstd::Error>;
    dstd::Vector<Result> results;
    results.reserve(paths.size());
    for (std::size_t i = 0; i < paths.size(); ++i) {
        results.push_back(dstd::fs::FileStat{});
    }
    dstd::Vector<StatSlice> slices;
    slices.reserve(paths.size() / STAT_BATCH + 1);
    for (std::size_t begin = 0; begin < paths.size(); begin += STAT_BATCH) {
        slices.push_back(
            StatSlice{begin, std::min(begin + STAT_BATCH, paths.size())});
    }
    Result* const out = results.data();
    auto job = [paths, out](StatSlice slice) -> coro::CoroTask<void> {
        auto fn = [paths, out, slice]() noexcept {
            for (std::size_t i = slice.begin; i < slice.stop; ++i) {
                out[i] = dstd::fs::stat(paths[i].view());
            }
            return true;
        };
        auto call = on_io(fn);
        co_await std::move(call);
    };
    coro::ParallelOptions options;
    options.max_in_flight = limit;
    auto all = coro::parallel_map(std::move(slices), job, options);
    co_await std::move(all);
    co_return results;
}

coro::AsyncGenerator<dstd::Expected<Entry, dstd::Error>> walk(
    coro::Scope& scope, dstd::PathView root, WalkOptions options) {
    auto plan = make_plan(scope, 0);
    plan->skip_dirs = std::move(options.skip_dirs);
    plan->recursive = options.recursive;
    plan->with_stat = false;
    plan->skip_unreadable = options.skip_unreadable;
    plan->every_entry = true;
    start(plan, root);
    return entries_of(StreamEnd(std::move(plan)));
}

coro::AsyncGenerator<dstd::Expected<FoundFile, dstd::Error>> find(
    coro::Scope& scope, dstd::PathView root, FindOptions options) {
    return files_of(StreamEnd(start_find(scope, root, std::move(options))));
}

coro::CoroTask<dstd::Expected<dstd::Vector<FoundFile>, dstd::Error>> find_all(
    coro::Scope& scope, dstd::PathView root, FindOptions options) {
    return drain(StreamEnd(start_find(scope, root, std::move(options))));
}

}  // namespace dftracer::utils::runtime::fs
