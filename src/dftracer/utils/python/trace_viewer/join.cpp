#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/dataframe.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/trace_viewer_detail.h>
#include <dftracer/utils/trace/comparator/compare_view.h>
#include <dftracer/utils/trace/views/result_join.h>

#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::python::trace_viewer_detail {

// Map a join-type name to the enum. Returns false on an unknown name.
bool parse_join_type(const char* how,
                     dftracer::utils::trace::views::JoinType* out) {
    namespace views = dftracer::utils::trace::views;
    const std::string h(how);
    if (h == "inner")
        *out = views::JoinType::INNER;
    else if (h == "left")
        *out = views::JoinType::LEFT;
    else if (h == "right")
        *out = views::JoinType::RIGHT;
    else if (h == "full")
        *out = views::JoinType::FULL;
    else if (h == "semi")
        *out = views::JoinType::LEFT_SEMI;
    else if (h == "anti")
        *out = views::JoinType::LEFT_ANTI;
    else
        return false;
    return true;
}

// Aggregate this viewer and `other`, then equi-join their result tables on the
// shared group key. Raises ValueError on a bad `how` or when the two group-key
// schemas differ; TypeError when `other` is not a TraceViewer.
PyObject* tv_join(TraceViewerObject* self, PyObject* args, PyObject* kwds) {
#ifndef DFTRACER_UTILS_ENABLE_ARROW
    PyErr_SetString(PyExc_RuntimeError,
                    "join() requires the arrow-enabled build");
    return nullptr;
#else
    namespace views = dftracer::utils::trace::views;
    PyObject* other = nullptr;
    const char* how = "inner";
    static const char* kwlist[] = {"other", "how", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|s",
                                     const_cast<char**>(kwlist), &other, &how))
        return nullptr;
    if (!PyObject_TypeCheck(other, &TraceViewerType)) {
        PyErr_SetString(PyExc_TypeError, "join() other must be a TraceViewer");
        return nullptr;
    }
    views::JoinType type = views::JoinType::INNER;
    if (!parse_join_type(how, &type)) {
        PyErr_Format(PyExc_ValueError,
                     "join() how must be inner|left|right|full|semi|anti, "
                     "got '%s'",
                     how);
        return nullptr;
    }

    TraceViewerObject* o = (TraceViewerObject*)other;
    Runtime* rt = resolve_runtime(self);
    auto lf = extract_files(self);
    auto li = extract_index_dir(self);
    ViewerPlan lp = *plan_of(self);
    auto rf = extract_files(o);
    auto ri = extract_index_dir(o);
    ViewerPlan rp = *plan_of(o);
    DataFrame joined;
    if (!run_blocking([&] {
            AggregatedView lv = build_agg_view(lf, li, lp);
            AggregatedView rv = build_agg_view(rf, ri, rp);
            joined = rt->submit(lv.join(rv, type)).get();
        }))
        return nullptr;
    if (joined.num_columns() == 0) {  // mismatched group-key schemas
        PyErr_SetString(
            PyExc_ValueError,
            "join() requires both views to share a group-key schema");
        return nullptr;
    }
    return dftracer::utils::python::wrap_dataframe(std::move(joined));
#endif
}

// compare(other): aggregate this viewer and `other` with THIS viewer's
// group_by + agg plan, in parallel, and return the comparison DataFrame (group
// key, l_/r_ per metric, plus delta_/pct_). Wraps
// trace::comparator::CompareView.
PyObject* tv_compare(TraceViewerObject* self, PyObject* args, PyObject* kwds) {
    namespace comparator = dftracer::utils::trace::comparator;
    PyObject* other = nullptr;
    static const char* kwlist[] = {"other", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O",
                                     const_cast<char**>(kwlist), &other))
        return nullptr;
    if (!PyObject_TypeCheck(other, &TraceViewerType)) {
        PyErr_SetString(PyExc_TypeError,
                        "compare() other must be a TraceViewer");
        return nullptr;
    }
    TraceViewerObject* o = (TraceViewerObject*)other;
    ViewerPlan lp = *plan_of(self);
    if (lp.agg.empty()) {
        PyErr_SetString(PyExc_ValueError,
                        "compare() needs a group_by + agg plan on the baseline "
                        "viewer (both sides aggregate the same way)");
        return nullptr;
    }
    Runtime* rt = resolve_runtime(self);
    auto lf = extract_files(self);
    auto li = extract_index_dir(self);
    auto rf = extract_files(o);
    auto ri = extract_index_dir(o);
    DataFrame result;
    if (!run_blocking([&] {
            View base = build_view_from_data(lf, li, lp, /*aggregate=*/false);
            View variant =
                build_view_from_data(rf, ri, lp, /*aggregate=*/false);
            result = rt->submit(comparator::CompareView::of(std::move(base),
                                                            std::move(variant))
                                    .group_by(lp.group_by)
                                    .agg(lp.agg)
                                    .collect())
                         .get();
        }))
        return nullptr;
    return dftracer::utils::python::wrap_dataframe(std::move(result));
}

}  // namespace dftracer::utils::python::trace_viewer_detail
