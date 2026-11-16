// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/bootstrap.h>
#include <dftracer/utils/comm/mpi/mpi_backend.h>
#include <dftracer/utils/coro/timer_service.h>

namespace dftracer::utils::comm::mpi {

namespace {

class MpiBootstrap : public backend::Bootstrap {
   public:
    explicit MpiBootstrap(dstd::SharedPtr<Context> context)
        : context_(std::move(context)) {}

    std::uint32_t ranks() const noexcept override { return context_->size(); }

    std::uint32_t own_rank() const noexcept override {
        return context_->rank();
    }

    dstd::Vector<std::uint32_t> node_members(
        std::uint32_t rank) const override {
        const dstd::Vector<std::uint32_t>& leaders = context_->node_leaders();
        dstd::Vector<std::uint32_t> members;
        if (rank >= leaders.size()) return members;
        for (std::uint32_t other = 0; other < leaders.size(); ++other)
            if (leaders[other] == leaders[rank]) members.push_back(other);
        return members;
    }

    coro::TimerService* timers() const noexcept override {
        return &coro::process_timers();
    }

   private:
    dstd::SharedPtr<Context> context_;
};

}  // namespace

dstd::SharedPtr<backend::Bootstrap> make_bootstrap(
    dstd::SharedPtr<Context> context) {
    return dstd::make_shared<MpiBootstrap>(std::move(context));
}

}  // namespace dftracer::utils::comm::mpi
