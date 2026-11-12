// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/runtime.h>

#include <utility>

namespace dftracer::utils::runtime::fs {

namespace {

using Found = dstd::Expected<dstd::Vector<FoundFile>, dstd::Error>;

coro::CoroTask<Found> find_in_scope(dstd::Path root, FindOptions options) {
    auto body = [&](coro::Scope& scope) {
        return find_all(scope, root.view(), std::move(options));
    };
    auto scoped = coro::with_scope(body);
    co_return co_await std::move(scoped);
}

}  // namespace

Found find_all_on_pool(std::size_t threads, std::size_t io_threads,
                       dstd::PathView root, FindOptions options) {
    RuntimeSettings settings;
    settings.threads = threads;
    settings.io_threads = io_threads;
    return configure_default_runtime(settings).block_on(
        coro::named("scan trace files",
                    find_in_scope(dstd::Path(root.str()), std::move(options))));
}

}  // namespace dftracer::utils::runtime::fs
