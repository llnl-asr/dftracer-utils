// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>

namespace dftracer::utils::coro {
namespace {

void retire_process_timers() { dstd::Singleton<TimerService>::get().retire(); }

TimerService& start_process_timers() {
    TimerService& timers = dstd::Singleton<TimerService>::get();
    if (dstd::shutdown_started()) {
        timers.retire();
        return timers;
    }
    // The hook goes in once, after the runtime stops and before the exit
    // checks, so the timer thread is joined while its owners are gone.
    dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches,
                            &retire_process_timers);
    timers.start();
    return timers;
}

}  // namespace

TimerService& process_timers() {
    static TimerService& timers = start_process_timers();
    return timers;
}

}  // namespace dftracer::utils::coro
