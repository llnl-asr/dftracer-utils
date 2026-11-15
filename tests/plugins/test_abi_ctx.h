// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_PLUGIN_ABI_CTX_H
#define DFTRACER_UTILS_TESTS_PLUGIN_ABI_CTX_H
#include <dftracer/utils/common/c_api.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/plugin.h>

namespace test_plugin_common {

// A call context for driving the plugin ABI from outside a callback. It keeps
// the message a service or callback reports.
struct Ctx {
    dftracer::utils::plugins::AbiContext ctx;
    dftu_ctx* get() { return ctx.get(); }
    dftracer::utils::dstd::String message() const {
        return dftracer::utils::dstd::String(ctx.message());
    }
};

// The status a callback returns for the outcome of an SDK call.
inline dftu_status as_status(
    const dftracer::utils::dstd::Result<void>& result) {
    return result.has_value()
               ? DFTU_OK
               : dftracer::utils::plugins::status_of(result.error());
}

// Runs a task a plugin handed back to completion through the host task
// service's run, and gives its status; `message` gets the error text of a
// failed task.
inline dftu_status run_task(::dftu_task* task,
                            dftracer::utils::dstd::String* message = nullptr) {
    Ctx c;
    const dftu_status status = dftracer::utils::plugins::host::detail::task_run(
        c.get(), nullptr, task);
    if (message) *message = c.message();
    return status;
}

// Drives an SDK Task the way the on_finalize trampoline does: the Host the
// coroutine is built with reads its ctx through a proxy that the host's drive
// step points at the current step.
//
//   TaskDriver d(host);
//   auto status = d.run(my_task(d.host(), ...), &message);
class TaskDriver {
   public:
    explicit TaskDriver(dftu_plugin_host& host)
        : host_(&host),
          proxy_(dftracer::utils::dstd::make_unique<
                 dftracer::utils::plugins::detail::ProxyCtx>(ctx_.get())) {}

    dftracer::utils::plugins::Host host() {
        return dftracer::utils::plugins::detail::host_from_raw(host_,
                                                               &proxy_->base);
    }

    // The status the task ends with; `message` gets its error text.
    dftu_status run(dftracer::utils::plugins::Task&& task,
                    dftracer::utils::dstd::String* message = nullptr) {
        dftu_task* pending = nullptr;
        const dftu_status st = dftracer::utils::plugins::detail::drive_coro(
            ctx_.get(), host_, std::move(task), std::move(proxy_), &pending);
        if (st < 0) {
            if (message) *message = ctx_.message();
            return st;
        }
        return run_task(pending, message);
    }

   private:
    dftu_plugin_host* host_;
    Ctx ctx_;
    dftracer::utils::dstd::UniquePtr<dftracer::utils::plugins::detail::ProxyCtx>
        proxy_;
};

}  // namespace test_plugin_common

#endif  // DFTRACER_UTILS_TESTS_PLUGIN_ABI_CTX_H
