#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/dataframe/internal/lazyframe_handle.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/query/internal/query_handle.h>
#include <dftracer/utils/trace/views/abi.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_source.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

// The opaque dftu_view handle owns a C++ View (AggregatedView slices to View;
// they share the same plan and terminals, so nothing is lost).
struct dftu_view {
    dftracer::utils::trace::views::View v;
};

namespace {

using dftracer::utils::Runtime;
using dftracer::utils::dataframe::dataframe_handle_wrap;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::lazyframe_handle_wrap;
using dftracer::utils::query::query_handle_unwrap;
using dftracer::utils::trace::views::AggOp;
using dftracer::utils::trace::views::AggSpec;
using dftracer::utils::trace::views::GroupKey;
using dftracer::utils::trace::views::View;
using dftracer::utils::trace::views::ViewFile;
using dftracer::utils::trace::views::ViewSource;

dftu_view* wrap(View&& v) { return new dftu_view{std::move(v)}; }

Runtime& runtime_of(dftu_runtime* rt) {
    return rt ? *reinterpret_cast<Runtime*>(rt)
              : dftracer::utils::default_runtime();
}

GroupKey::Kind to_kind(dftu_group_key_kind k) {
    switch (k) {
        case DFTU_GROUP_KEY_NAME:
            return GroupKey::Kind::Name;
        case DFTU_GROUP_KEY_CAT:
            return GroupKey::Kind::Cat;
        case DFTU_GROUP_KEY_PID:
            return GroupKey::Kind::Pid;
        case DFTU_GROUP_KEY_TID:
            return GroupKey::Kind::Tid;
        case DFTU_GROUP_KEY_FHASH:
            return GroupKey::Kind::Fhash;
        case DFTU_GROUP_KEY_HHASH:
            return GroupKey::Kind::Hhash;
        case DFTU_GROUP_KEY_IO_CAT:
            return GroupKey::Kind::IoCat;
        case DFTU_GROUP_KEY_ACC_PAT:
            return GroupKey::Kind::AccPat;
        case DFTU_GROUP_KEY_FILE_PATH:
            return GroupKey::Kind::FilePath;
        case DFTU_GROUP_KEY_FILE_NAME:
            return GroupKey::Kind::FileName;
        case DFTU_GROUP_KEY_HOST_NAME:
            return GroupKey::Kind::HostName;
        case DFTU_GROUP_KEY_RANK:
            return GroupKey::Kind::Rank;
        case DFTU_GROUP_KEY_ARG:
            return GroupKey::Kind::Arg;
        case DFTU_GROUP_KEY_FIELD:
            return GroupKey::Kind::Field;
    }
    return GroupKey::Kind::Name;
}

GroupKey::Transform to_transform(dftu_group_transform t) {
    switch (t) {
        case DFTU_GROUP_TRANSFORM_NONE:
            return GroupKey::Transform::None;
        case DFTU_GROUP_TRANSFORM_DIRNAME:
            return GroupKey::Transform::Dirname;
        case DFTU_GROUP_TRANSFORM_BASENAME:
            return GroupKey::Transform::Basename;
        case DFTU_GROUP_TRANSFORM_LOWER:
            return GroupKey::Transform::Lower;
        case DFTU_GROUP_TRANSFORM_BUCKET:
            return GroupKey::Transform::Bucket;
    }
    return GroupKey::Transform::None;
}

AggOp to_agg_op(dftu_view_agg_op op) {
    switch (op) {
        case DFTU_VIEW_AGG_COUNT:
            return AggOp::Count;
        case DFTU_VIEW_AGG_SUM:
            return AggOp::Sum;
        case DFTU_VIEW_AGG_MIN:
            return AggOp::Min;
        case DFTU_VIEW_AGG_MAX:
            return AggOp::Max;
        case DFTU_VIEW_AGG_MEAN:
            return AggOp::Mean;
        case DFTU_VIEW_AGG_VAR:
            return AggOp::Var;
        case DFTU_VIEW_AGG_STD:
            return AggOp::Std;
        case DFTU_VIEW_AGG_ARG_MAX:
            return AggOp::ArgMax;
        case DFTU_VIEW_AGG_SUM_SQ:
            return AggOp::SumSq;
        case DFTU_VIEW_AGG_PCT:
            return AggOp::Pct;
        case DFTU_VIEW_AGG_SKEW:
            return AggOp::Skew;
        case DFTU_VIEW_AGG_KURT:
            return AggOp::Kurt;
        case DFTU_VIEW_AGG_HIST:
            return AggOp::Hist;
        case DFTU_VIEW_AGG_SET_UNION:
            return AggOp::SetUnion;
        case DFTU_VIEW_AGG_BUSY:
            return AggOp::Busy;
        case DFTU_VIEW_AGG_CONCURRENCY:
            return AggOp::Concurrency;
        case DFTU_VIEW_AGG_UTILIZATION:
            return AggOp::Utilization;
        case DFTU_VIEW_AGG_ACTIVE:
            return AggOp::Active;
    }
    return AggOp::Count;
}

// dftu_view_agg_op must mirror AggOp (view.h) value-for-value, the way
// dataframe/op.h's OpKind/OpTok mirror their C enums, so the two cannot drift.
static_assert(static_cast<int>(AggOp::Count) == DFTU_VIEW_AGG_COUNT);
static_assert(static_cast<int>(AggOp::Sum) == DFTU_VIEW_AGG_SUM);
static_assert(static_cast<int>(AggOp::Min) == DFTU_VIEW_AGG_MIN);
static_assert(static_cast<int>(AggOp::Max) == DFTU_VIEW_AGG_MAX);
static_assert(static_cast<int>(AggOp::Mean) == DFTU_VIEW_AGG_MEAN);
static_assert(static_cast<int>(AggOp::Var) == DFTU_VIEW_AGG_VAR);
static_assert(static_cast<int>(AggOp::Std) == DFTU_VIEW_AGG_STD);
static_assert(static_cast<int>(AggOp::ArgMax) == DFTU_VIEW_AGG_ARG_MAX);
static_assert(static_cast<int>(AggOp::SumSq) == DFTU_VIEW_AGG_SUM_SQ);
static_assert(static_cast<int>(AggOp::Pct) == DFTU_VIEW_AGG_PCT);
static_assert(static_cast<int>(AggOp::Skew) == DFTU_VIEW_AGG_SKEW);
static_assert(static_cast<int>(AggOp::Kurt) == DFTU_VIEW_AGG_KURT);
static_assert(static_cast<int>(AggOp::Hist) == DFTU_VIEW_AGG_HIST);
static_assert(static_cast<int>(AggOp::SetUnion) == DFTU_VIEW_AGG_SET_UNION);
static_assert(static_cast<int>(AggOp::Busy) == DFTU_VIEW_AGG_BUSY);
static_assert(static_cast<int>(AggOp::Concurrency) ==
              DFTU_VIEW_AGG_CONCURRENCY);
static_assert(static_cast<int>(AggOp::Utilization) ==
              DFTU_VIEW_AGG_UTILIZATION);
static_assert(static_cast<int>(AggOp::Active) == DFTU_VIEW_AGG_ACTIVE);

}  // namespace

extern "C" {

dftu_view* dftu_view_from_files(const char* const* paths,
                                const char* const* index_paths, int32_t n) {
    if (!paths || n < 0) return nullptr;
    try {
        std::vector<ViewFile> files;
        files.reserve(static_cast<std::size_t>(n));
        for (int32_t i = 0; i < n; ++i) {
            if (!paths[i]) return nullptr;
            ViewFile f;
            f.file_path = paths[i];
            if (index_paths && index_paths[i]) f.index_path = index_paths[i];
            files.push_back(std::move(f));
        }
        return wrap(View::from_files(std::move(files)));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_view* dftu_view_from_directory(const char* dir, const char* index_path,
                                    dftu_runtime* rt) {
    if (!dir) return nullptr;
    try {
        View v =
            runtime_of(rt)
                .submit(View::from_directory(dir, index_path ? index_path : ""))
                .get();
        if (v.plan().files.empty()) return nullptr;
        return wrap(std::move(v));
    } catch (const std::exception&) {
        return nullptr;
    }
}

void dftu_view_free(dftu_view* v) { delete v; }

dftu_view* dftu_view_filter(const dftu_view* v, const dftu_query* q) {
    if (!v || !q) return nullptr;
    try {
        return wrap(v->v.filter(query_handle_unwrap(q)));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_view* dftu_view_select(const dftu_view* v, const char* const* cols,
                            int32_t n) {
    if (!v || n < 0 || (n > 0 && !cols)) return nullptr;
    try {
        std::vector<std::string> c;
        c.reserve(static_cast<std::size_t>(n));
        for (int32_t i = 0; i < n; ++i) {
            if (!cols[i]) return nullptr;
            c.emplace_back(cols[i]);
        }
        return wrap(v->v.select(std::move(c)));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_view* dftu_view_limit(const dftu_view* v, uint64_t n) {
    if (!v) return nullptr;
    try {
        return wrap(v->v.limit(n));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_view* dftu_view_offset(const dftu_view* v, uint64_t n) {
    if (!v) return nullptr;
    try {
        return wrap(v->v.offset(n));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_view* dftu_view_sort_by(const dftu_view* v, const char* column,
                             int32_t descending) {
    if (!v || !column) return nullptr;
    try {
        return wrap(v->v.sort_by(column, descending != 0));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_view* dftu_view_group_by(const dftu_view* v, const dftu_group_key* keys,
                              int32_t n) {
    if (!v || n < 0 || (n > 0 && !keys)) return nullptr;
    try {
        std::vector<GroupKey> ks;
        ks.reserve(static_cast<std::size_t>(n));
        for (int32_t i = 0; i < n; ++i) {
            GroupKey k;
            k.kind = to_kind(keys[i].kind);
            if (keys[i].arg) k.arg = keys[i].arg;
            k.transform = to_transform(keys[i].transform);
            if (k.transform == GroupKey::Transform::Bucket) {
                if (keys[i].n_transform_args > 0 && !keys[i].transform_args)
                    return nullptr;
                for (int32_t j = 0; j < keys[i].n_transform_args; ++j) {
                    if (!keys[i].transform_args[j]) return nullptr;
                    k.transform_args.emplace_back(keys[i].transform_args[j]);
                }
            }
            ks.push_back(std::move(k));
        }
        return wrap(v->v.group_by(std::move(ks)));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_view* dftu_view_agg(const dftu_view* v, const dftu_view_agg_spec* specs,
                         int32_t n) {
    if (!v || n < 0 || (n > 0 && !specs)) return nullptr;
    try {
        std::vector<AggSpec> ag;
        ag.reserve(static_cast<std::size_t>(n));
        for (int32_t i = 0; i < n; ++i) {
            ag.emplace_back(to_agg_op(specs[i].op),
                            specs[i].field ? specs[i].field : "",
                            specs[i].out ? specs[i].out : "",
                            specs[i].by ? specs[i].by : "", specs[i].q);
        }
        return wrap(v->v.agg(std::move(ag)));
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_dataframe* dftu_view_collect(const dftu_view* v, dftu_runtime* rt) {
    if (!v) return nullptr;
    try {
        return dataframe_handle_wrap(
            runtime_of(rt).submit(v->v.collect_frame()).get());
    } catch (const std::exception&) {
        return nullptr;
    }
}

dftu_lazyframe* dftu_view_lazy(const dftu_view* v) {
    if (!v) return nullptr;
    try {
        return lazyframe_handle_wrap(
            LazyFrame::scan(std::make_shared<ViewSource>(v->v)));
    } catch (const std::exception&) {
        return nullptr;
    }
}

}  // extern "C"
