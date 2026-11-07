// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/stop_token.h>

#include <algorithm>

namespace dftracer::utils::coro {

namespace {

template <typename State>
void stop_tree(State& state) noexcept {
    state.stopped.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(state.mutex);
    for (const auto& weak : state.children) {
        if (auto child = weak.lock()) stop_tree(*child);
    }
}

}  // namespace

bool StopToken::stop_requested() const noexcept {
    return state_ && state_->stopped.load(std::memory_order_acquire);
}

StopSource::StopSource() : state_(dstd::make_shared<StopToken::State>()) {}

StopSource::StopSource(const StopToken& parent) : StopSource() {
    if (!parent.state_) return;
    std::lock_guard<std::mutex> lock(parent.state_->mutex);
    if (parent.state_->stopped.load(std::memory_order_acquire)) {
        state_->stopped.store(true, std::memory_order_release);
    } else {
        auto& children = parent.state_->children;
        // Before it would grow, so a long-lived parent keeps only live
        // children.
        if (children.size() == children.capacity()) {
            children.erase(
                std::remove_if(children.begin(), children.end(),
                               [](const auto& weak) { return weak.expired(); }),
                children.end());
        }
        children.push_back(state_);
    }
}

void StopSource::request_stop(Executor* executor) noexcept {
    stop_tree(*state_);
    Executor* target = executor != nullptr ? executor : Executor::current();
    if (target != nullptr) target->wake_cancelled();
}

}  // namespace dftracer::utils::coro
