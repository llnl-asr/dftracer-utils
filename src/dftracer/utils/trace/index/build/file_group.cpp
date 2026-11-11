// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/build/file_group.h>
#include <dftracer/utils/trace/index/tables.h>

#include <utility>

namespace dftracer::utils::trace::index::build {

coro::CoroTask<dstd::String> FileGroup::add(FileBuild build, std::size_t tag,
                                            dstd::String path) {
    bool repeated = false;
    for (const Entry& entry : entries_)
        repeated = repeated || entry.path == path;
    if (repeated) co_await publish();
    auto held = co_await claim(build);
    auto staged = co_await held->stage();
    held.reset();
    if (!staged) co_return dstd::String(staged.error().message());
    builds_.push_back(std::move(build));
    entries_.push_back(Entry{tag, std::move(path)});
    co_return dstd::String();
}

coro::CoroTask<void> FileGroup::publish() {
    if (builds_.empty()) co_return;
    dstd::Vector<FileBuild*> group;
    group.reserve(builds_.size());
    for (FileBuild& build : builds_) group.push_back(&build);
    ++result_.publishes;
    auto errors = co_await trace::index::commit_group(
        dstd::Span<FileBuild* const>(group.data(), group.size()));
    for (std::size_t i = 0; i < errors.size(); ++i) {
        if (!errors[i]) {
            ++result_.published;
            continue;
        }
        result_.failures.push_back(
            GroupFailure{entries_[i].tag, std::move(entries_[i].path),
                         dstd::String(errors[i]->message())});
    }
    builds_.clear();
    entries_.clear();
}

coro::CoroTask<GroupResult> FileGroup::commit() {
    co_await publish();
    GroupResult out = std::move(result_);
    result_ = GroupResult{};
    co_return out;
}

}  // namespace dftracer::utils::trace::index::build
