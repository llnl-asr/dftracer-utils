// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/runtime/spawn_detached.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/plan/rowsets.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>

#include <algorithm>
#include <atomic>
#include <latch>
#include <type_traits>
#include <vector>

namespace dftracer::utils::trace::index::plan {

dstd::Result<const RecordSchema*> recorded_schema(const Snapshot& snapshot,
                                                  dstd::CStringView trace) {
    const auto file = snapshot.file(fileio::get_logical_path(trace));
    if (!file) return static_cast<const RecordSchema*>(nullptr);
    const auto id = trace::index::schema(snapshot, *file);
    if (!id) return id.error();
    if (!*id) return static_cast<const RecordSchema*>(nullptr);
    return get_schema(**id);
}

// The file's entry in the snapshot, when the index holds it for the trace as
// the trace is now.
static dstd::Optional<FileRef> current_file(const Snapshot& snapshot,
                                            const RowSetFile& f) {
    common::InputFileStat st;
    SourceStat now;
    if (common::stat_input(f.trace, st)) {
        now.mtime = static_cast<std::uint64_t>(st.mtime);
        now.size = st.size;
    }
    const dstd::String logical = fileio::get_logical_path(f.trace);
    if (snapshot.freshness(logical, now) != Freshness::Current)
        return dstd::nullopt;
    return snapshot.file(logical);
}

static dstd::Result<SchemaDetection> explain_trace_schema(const RowSetFile& f) {
    common::InputFileStat st;
    if (f.file && common::stat_input(f.trace, st))
        return explain_file_schema(f.trace, *f.file, st.size, st.mtime_ns);
    return explain_file_schema(f.trace);
}

static dstd::Result<const RecordSchema*> detect_trace_schema(
    const RowSetFile& f) {
    auto detection = explain_trace_schema(f);
    if (!detection) return detection.error();
    return detection->chosen;
}

static bool trace_exists(const dstd::String& path) {
    common::InputFileStat st;
    return common::stat_input(path, st);
}

namespace {

struct IndexLoop {
    IndexLoop(std::size_t count, std::ptrdiff_t helpers,
              void (*invoke)(void*, std::size_t), void* ctx)
        : n(count), call(invoke), state(ctx), done(helpers) {}
    std::atomic<std::size_t> next{0};
    std::size_t n;
    void (*call)(void*, std::size_t);
    void* state;
    std::latch done;

    void drain() {
        for (std::size_t i; (i = next.fetch_add(1)) < n;) call(state, i);
    }
};

coro::CoroTask<void> index_loop_helper(dstd::SharedPtr<IndexLoop> loop) {
    loop->drain();
    loop->done.count_down();
    co_return;
}

// Runs fn(i) for each i in [0, n) on the calling thread's executor, serially
// off an executor. The caller drains the loop too and waits for the helpers
// with its worker slot handed off. `fn` must not throw.
template <typename Fn>
void for_each_index(std::size_t n, Fn&& fn) {
    coro::Executor* exec = coro::Executor::current();
    const std::size_t workers =
        exec ? std::min(n, exec->concurrency()) : std::size_t{1};
    if (workers <= 1) {
        for (std::size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    auto invoke = [](void* ctx, std::size_t i) {
        (*static_cast<std::remove_reference_t<Fn>*>(ctx))(i);
    };
    auto loop = dstd::make_shared<IndexLoop>(
        n, static_cast<std::ptrdiff_t>(workers - 1), invoke, &fn);
    for (std::size_t w = 1; w < workers; ++w)
        if (!spawn_detached(exec, index_loop_helper(loop)))
            loop->done.count_down();
    loop->drain();
    coro::BlockingRegion blocking(exec);
    loop->done.wait();
}

}  // namespace

dstd::Result<const RecordSchema*> files_schema(
    const dstd::Vector<RowSetFile>& files) {
    // A missing trace with no recorded schema is read as dftracer here, and
    // the scan reports the missing trace.
    dstd::Map<dstd::String, dstd::Optional<Snapshot>> snapshots;
    dstd::Vector<const Snapshot*> snapshot_of(files.size(), nullptr);
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        if (f.index.empty()) continue;
        auto [it, fresh] = snapshots.try_emplace(f.index);
        if (fresh) {
            const auto present =
                dstd::fs::exists(dstd::PathView(store_of(f.index)));
            if (!present.has_value()) return present.error();
            if (*present) {
                if (auto loaded = load_index_schemas(f.index); !loaded)
                    return loaded.error();
                auto opened = open_snapshot(f.index);
                if (!opened) return opened.error();
                it->second.emplace(std::move(*opened));
            }
        }
        if (it->second) snapshot_of[i] = &*it->second;
    }
    dstd::Vector<const RecordSchema*> schemas(files.size(), nullptr);
    dstd::Vector<dstd::Optional<dstd::Error>> errors(files.size());
    for_each_index(files.size(), [&](std::size_t i) {
        const auto& f = files[i];
        const RecordSchema* p = nullptr;
        // A recorded schema that is not registered is an error, not a
        // reason to detect again.
        if (snapshot_of[i]) {
            auto recorded = recorded_schema(*snapshot_of[i], f.trace);
            if (!recorded) {
                errors[i] = recorded.error();
                return;
            }
            p = *recorded;
        }
        if (!p && trace_exists(f.trace)) {
            auto detected = detect_trace_schema(f);
            if (!detected) {
                errors[i] = detected.error();
                return;
            }
            p = *detected;
        }
        if (!p) {
            DFTRACER_UTILS_LOG_WARN(
                "rowsets: {} is missing and has no recorded schema; assume "
                "dftracer",
                f.trace);
            auto assumed = get_schema("dftracer");
            if (!assumed) {
                errors[i] = assumed.error();
                return;
            }
            p = *assumed;
        }
        schemas[i] = p;
    });
    for (auto& e : errors)
        if (e) return std::move(*e);
    if (schemas.empty()) return get_schema("dftracer");
    const RecordSchema* found = schemas.front();
    if (std::all_of(schemas.begin(), schemas.end(),
                    [&](const RecordSchema* s) { return s == found; }))
        return found;
    dstd::Vector<SchemaDetection> detections(files.size());
    for_each_index(files.size(), [&](std::size_t i) {
        if (!trace_exists(files[i].trace)) return;
        auto detection = explain_trace_schema(files[i]);
        if (!detection) {
            errors[i] = detection.error();
            return;
        }
        detections[i] = std::move(*detection);
    });
    found = nullptr;
    std::size_t voter = 0;
    const RecordSchema* fallback = nullptr;
    for (std::size_t i = 0; i < schemas.size(); ++i) {
        if (errors[i]) return std::move(*errors[i]);
        const auto& d = detections[i];
        if (d.records == 0) {
            if (!fallback && d.objects > 0) fallback = schemas[i];
            continue;
        }
        if (found && found != schemas[i])
            return common::make_error<
                "a View reads files of one record schema: {} is {} and {} is "
                "{}">(common::ErrorCode::InvalidArgument, files[voter].trace,
                      found->id, files[i].trace, schemas[i]->id);
        if (!found) {
            found = schemas[i];
            voter = i;
        }
    }
    if (found) return found;
    // No file holds a record, so no file votes. Detection leaves such a file
    // to the schema of its objects, else generic.
    if (!fallback) {
        auto generic = get_schema("generic");
        if (!generic) return generic.error();
        fallback = *generic;
    }
    DFTRACER_UTILS_LOG_DEBUG("rowsets: no file holds a record; use schema {}",
                             fallback->id);
    return fallback;
}

dstd::Optional<dataframe::DataFrame> stored_rowset(
    const dstd::Vector<RowSetFile>& files, dstd::StringView name) {
    if (files.empty()) return dstd::nullopt;
    dstd::Map<dstd::String, dstd::Optional<Snapshot>, std::less<>> snapshots;
    dstd::Vector<dataframe::DataFrame> parts;
    for (const auto& f : files) {
        if (f.index.empty()) return dstd::nullopt;
        auto [it, fresh] = snapshots.try_emplace(f.index);
        if (fresh) {
            auto opened = open_snapshot(f.index);
            if (!opened) return dstd::nullopt;
            it->second.emplace(std::move(*opened));
        }
        const Snapshot& snapshot = *it->second;
        const auto file = current_file(snapshot, f);
        if (!file) return dstd::nullopt;
        auto bytes = trace::index::rowset(snapshot, *file, name);
        if (!bytes || !*bytes) return dstd::nullopt;
        auto frame = dataframe::frame_from_native(**bytes);
        if (!frame) return dstd::nullopt;
        parts.push_back(std::move(*frame));
    }
    if (parts.size() == 1) return std::move(parts.front());
    dstd::Vector<const dataframe::DataFrame*> ptrs;
    for (const auto& p : parts) ptrs.push_back(&p);
    auto joined = dataframe::concat(ptrs, dataframe::ConcatHow::Diagonal);
    if (!joined) return dstd::nullopt;
    return std::move(*joined);
}

bool has_stored_rowsets(const dstd::Vector<RowSetFile>& files) {
    if (files.empty()) return false;
    dstd::Map<dstd::String, dstd::Optional<Snapshot>, std::less<>> snapshots;
    for (const auto& f : files) {
        if (f.index.empty()) return false;
        auto [it, fresh] = snapshots.try_emplace(f.index);
        if (fresh) {
            auto opened = open_snapshot(f.index);
            if (!opened) return false;
            it->second.emplace(std::move(*opened));
        }
        const Snapshot& snapshot = *it->second;
        const auto file = current_file(snapshot, f);
        if (!file || !trace::index::extension_current(
                         snapshot, *file, store::IndexExtension::Rowset))
            return false;
    }
    return true;
}

}  // namespace dftracer::utils::trace::index::plan
