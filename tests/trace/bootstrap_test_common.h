// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_TRACE_BOOTSTRAP_COMMON_H
#define DFTRACER_UTILS_TESTS_TRACE_BOOTSTRAP_COMMON_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <trace/views/test_view_common.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bootstrap_test {

inline std::map<std::string, std::int64_t> counts(
    const dftracer::utils::dataframe::DataFrame& df) {
    std::map<std::string, std::int64_t> out;
    for (std::int64_t i = 0; i < df.num_rows(); ++i)
        out[test_view_common::bstr(df, i, "name")] =
            static_cast<std::int64_t>(test_view_common::bnum(df, i, "n"));
    return out;
}

inline dftracer::utils::dstd::Vector<dftracer::utils::trace::views::ViewFile>
view_files_of(const std::vector<std::string>& files,
              const std::string& index_dir) {
    dftracer::utils::dstd::Vector<dftracer::utils::trace::views::ViewFile>
        view_files;
    for (const auto& f : files) {
        dftracer::utils::trace::views::ViewFile vf;
        vf.file_path = f;
        vf.index_path = dftracer::utils::trace::internal::determine_index_path(
            f, index_dir);
        view_files.push_back(std::move(vf));
    }
    return view_files;
}

inline dftracer::utils::dataframe::DataFrame count_by_name(
    const std::vector<std::string>& files, const std::string& index_dir) {
    const auto view =
        dftracer::utils::trace::views::View::from_files(
            view_files_of(files, index_dir))
            .group_by({dftracer::utils::trace::views::GroupKey::name()})
            .agg({{dftracer::utils::trace::views::AggOp::Count, "", "n"}});
    dftracer::utils::dataframe::DataFrame out;
    dftracer::utils::default_runtime().block_on(dftracer::utils::coro::named(
        "bootstrap-parallelism",
        dftracer::utils::coro::with_scope(
            [&](dftracer::utils::coro::Scope&)
                -> dftracer::utils::coro::CoroTask<void> {
                out = co_await view.collect();
            })));
    return out;
}

}  // namespace bootstrap_test

#endif  // DFTRACER_UTILS_TESTS_TRACE_BOOTSTRAP_COMMON_H
