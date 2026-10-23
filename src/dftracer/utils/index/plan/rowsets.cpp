#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/plan/rowsets.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <latch>
#include <map>
#include <memory>
#include <type_traits>

namespace dftracer::utils::index::plan {

namespace df = dftracer::utils::dataframe;

const RecordSchema* recorded_schema(const store::IndexDatabase& db,
                                    const std::string& trace) {
    const int fid =
        db.get_file_info_id(index::store::internal::get_logical_path(trace));
    if (fid < 0) return nullptr;
    const auto id = db.file_schema(fid);
    return id ? &get_schema(*id) : nullptr;
}

static SchemaDetection explain_trace_schema(const RowSetFile& f) {
    InputFileStat st;
    if (f.fd >= 0 && stat_input(f.trace, st))
        return explain_file_schema(f.trace, f.fd, st.size, st.mtime_ns);
    return explain_file_schema(f.trace);
}

static const RecordSchema& detect_trace_schema(const RowSetFile& f) {
    return *explain_trace_schema(f).chosen;
}

static bool trace_exists(const std::string& path) {
    InputFileStat st;
    return stat_input(path, st);
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

// Runs fn(i) for each i in [0, n) on the cores of the calling thread's
// executor, serially off an executor. The caller drains the loop too and waits
// for the helpers with its worker slot handed off. `fn` must not throw.
template <typename Fn>
void for_each_index(std::size_t n, Fn&& fn) {
    Executor* exec = Executor::current();
    const std::size_t workers =
        exec ? std::min(n, exec->get_num_threads()) : std::size_t{1};
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
    BlockingRegion blocking(exec);
    loop->done.wait();
}

}  // namespace

const RecordSchema& files_schema(const std::vector<RowSetFile>& files) {
    // A schema that cannot be read here (an unreadable index or trace) is
    // dftracer's; the scan itself reports a missing trace or a broken index.
    dstd::Map<std::string, std::optional<store::IndexDatabase>> dbs;
    dstd::Vector<const store::IndexDatabase*> db_of(files.size(), nullptr);
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        if (f.index.empty()) continue;
        auto [it, fresh] = dbs.try_emplace(f.index);
        if (fresh && fs::exists(f.index)) {
            load_index_schemas(f.index);
            try {
                it->second.emplace(f.index, store::IndexOpenMode::ReadOnly);
            } catch (const std::exception&) {
            }
        }
        if (it->second) db_of[i] = &*it->second;
    }
    dstd::Vector<const RecordSchema*> schemas(files.size(), nullptr);
    dstd::Vector<std::exception_ptr> errors(files.size());
    for_each_index(files.size(), [&](std::size_t i) {
        try {
            const auto& f = files[i];
            const RecordSchema* p = nullptr;
            // A recorded schema that is not registered is an error, not a
            // reason to detect again.
            if (db_of[i]) p = recorded_schema(*db_of[i], f.trace);
            if (!p && trace_exists(f.trace)) p = &detect_trace_schema(f);
            schemas[i] = p ? p : &get_schema("dftracer");
        } catch (...) {
            errors[i] = std::current_exception();
        }
    });
    for (const auto& e : errors)
        if (e) std::rethrow_exception(e);
    if (schemas.empty()) return get_schema("dftracer");
    const RecordSchema* found = schemas.front();
    if (std::all_of(schemas.begin(), schemas.end(),
                    [&](const RecordSchema* s) { return s == found; }))
        return *found;
    dstd::Vector<SchemaDetection> detections(files.size());
    for_each_index(files.size(), [&](std::size_t i) {
        try {
            if (trace_exists(files[i].trace))
                detections[i] = explain_trace_schema(files[i]);
        } catch (...) {
            errors[i] = std::current_exception();
        }
    });
    found = nullptr;
    std::size_t voter = 0;
    const RecordSchema* fallback = nullptr;
    for (std::size_t i = 0; i < schemas.size(); ++i) {
        if (errors[i]) std::rethrow_exception(errors[i]);
        const auto& d = detections[i];
        if (d.records == 0) {
            if (!fallback && d.objects > 0) fallback = schemas[i];
            continue;
        }
        if (found && found != schemas[i])
            throw DFTUtilsException::cat(
                ErrorCode::InvalidArgument,
                "a View reads files of one record schema: ", files[voter].trace,
                " is ", found->id, " and ", files[i].trace, " is ",
                schemas[i]->id);
        if (!found) {
            found = schemas[i];
            voter = i;
        }
    }
    if (found) return *found;
    return fallback ? *fallback : get_schema("generic");
}

std::optional<df::DataFrame> stored_rowset(const std::vector<RowSetFile>& files,
                                           std::string_view name) {
    if (files.empty()) return std::nullopt;
    std::map<std::string, std::optional<store::IndexDatabase>, std::less<>> dbs;
    std::vector<df::DataFrame> parts;
    try {
        for (const auto& f : files) {
            if (f.index.empty()) return std::nullopt;
            auto [it, fresh] = dbs.try_emplace(f.index);
            if (fresh)
                it->second.emplace(f.index, store::IndexOpenMode::ReadOnly);
            const store::IndexDatabase& db = *it->second;
            if (db.check_freshness(f.trace) !=
                store::IndexDatabase::Freshness::Fresh)
                return std::nullopt;
            const int fid =
                db.get_file_info_id(store::internal::get_logical_path(f.trace));
            if (fid < 0) return std::nullopt;
            auto bytes = db.rowset(fid, name);
            if (!bytes) return std::nullopt;
            auto frame = df::frame_from_native(*bytes);
            if (!frame) return std::nullopt;
            parts.push_back(std::move(*frame));
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (parts.size() == 1) return std::move(parts.front());
    std::vector<const df::DataFrame*> ptrs;
    for (const auto& p : parts) ptrs.push_back(&p);
    return df::concat(ptrs, df::ConcatHow::Diagonal);
}

bool has_stored_rowsets(const std::vector<RowSetFile>& files) {
    if (files.empty()) return false;
    std::map<std::string, std::optional<store::IndexDatabase>, std::less<>> dbs;
    try {
        for (const auto& f : files) {
            if (f.index.empty()) return false;
            auto [it, fresh] = dbs.try_emplace(f.index);
            if (fresh)
                it->second.emplace(f.index, store::IndexOpenMode::ReadOnly);
            const store::IndexDatabase& db = *it->second;
            if (db.check_freshness(f.trace) !=
                store::IndexDatabase::Freshness::Fresh)
                return false;
            const int fid =
                db.get_file_info_id(store::internal::get_logical_path(f.trace));
            if (fid < 0 ||
                !db.extension_current(fid, store::IndexExtension::Rowset))
                return false;
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

}  // namespace dftracer::utils::index::plan
