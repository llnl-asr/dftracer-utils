#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/trace_viewer_detail.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/python/streaming_iterator.h>
#endif

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::python::trace_viewer_detail {

namespace dataframe = dftracer::utils::dataframe;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
namespace {

// Drives View::stream() (raw-event morsels via LazyFrame's streaming cursor)
// into the StreamingState queue. push() blocks when the bounded queue is
// full; the Python consumer drains it with the GIL released, converting to
// Arrow only if it calls .to_arrow() on a chunk.
dftracer::utils::coro::CoroTask<void> run_viewer_stream(
    dftracer::utils::CoroScope& scope,
    std::shared_ptr<
        dftracer::utils::python::StreamingState<dataframe::DataFrame>>
        state,
    std::vector<std::string> files, std::string index_dir, ViewerPlan plan,
    std::int64_t batch_size) {
    (void)scope;
    try {
        View v = build_view_from_data(files, index_dir, plan, /*aggregate=*/
                                      false);
        if (!plan.select.empty()) v = v.select(plan.select);
        if (!plan.sort_col.empty())
            v = v.sort_by(plan.sort_col, plan.sort_desc);
        if (!plan.topk_col.empty())
            v = v.topk(plan.topk_col, plan.topk_k, plan.topk_largest);
        if (plan.limit) v = v.limit(plan.limit);
        if (plan.offset) v = v.offset(plan.offset);

        auto gen = v.stream(batch_size);
        while (auto df = co_await gen.next()) {
            if (state->cancelled()) break;
            const std::size_t bytes =
                static_cast<std::size_t>(df->num_rows()) *
                static_cast<std::size_t>(df->num_columns() + 1) * 16;
            if (!state->push(std::move(*df), bytes)) break;
        }
        state->complete();
    } catch (...) {
        state->fail(std::current_exception());
    }
}

}  // namespace
#endif

PyObject* tv_stream(TraceViewerObject* self, PyObject* args, PyObject* kwds) {
#ifndef DFTRACER_UTILS_ENABLE_ARROW
    PyErr_SetString(PyExc_RuntimeError,
                    "stream() requires the arrow-enabled build");
    return nullptr;
#else
    static const char* kwlist[] = {"batch_size", "workers", "normalize", "dict",
                                   nullptr};
    long long batch_size = 65536;
    long long workers = 0;
    int normalize = 0;
    int dict_strings = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|LLpp",
                                     const_cast<char**>(kwlist), &batch_size,
                                     &workers, &normalize, &dict_strings))
        return nullptr;
    if (batch_size <= 0) batch_size = 65536;
    Runtime* rt = resolve_runtime(self);
    (void)workers;
    (void)normalize;
    (void)dict_strings;

    std::vector<std::string> files = extract_files(self);
    std::string index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);

    // 0 (unset) falls back to the RAM-fraction default.
    auto state = std::make_shared<
        dftracer::utils::python::StreamingState<dataframe::DataFrame>>(
        dftracer::utils::compute_memory_budget(plan.memory_budget));

    auto* iter_obj =
        (dftracer::utils::python::ArrowStreamingIteratorObject*)
            dftracer::utils::python::ArrowStreamingIteratorType.tp_new(
                &dftracer::utils::python::ArrowStreamingIteratorType, nullptr,
                nullptr);
    if (!iter_obj) return nullptr;
    iter_obj->cpp_state->state = state;
    iter_obj->cpp_state->pull_df =
        [state]() -> std::optional<dataframe::DataFrame> {
        return state->pull();
    };
    iter_obj->cpp_state->get_error = [state]() -> std::exception_ptr {
        return state->error();
    };
    iter_obj->cpp_state->cancel = [state]() { state->cancel(); };

    Py_BEGIN_ALLOW_THREADS rt->submit(
        dftracer::utils::run_coro_scope(
            rt->executor(), run_viewer_stream, state, std::move(files),
            std::move(index_dir), std::move(plan), (std::int64_t)batch_size),
        "trace_viewer_stream");
    Py_END_ALLOW_THREADS return (PyObject*)iter_obj;
#endif
}

}  // namespace dftracer::utils::python::trace_viewer_detail
