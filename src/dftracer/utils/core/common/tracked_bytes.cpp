#include <dftracer/utils/core/common/tracked_bytes.h>

namespace dftracer::utils::tracked_bytes {

namespace {
std::atomic<std::uint64_t> lazy_resident_{0};
std::atomic<std::uint64_t> pool_used_{0};
}  // namespace

std::atomic<std::uint64_t>& lazy_resident() noexcept { return lazy_resident_; }

std::atomic<std::uint64_t>& pool_used() noexcept { return pool_used_; }

}  // namespace dftracer::utils::tracked_bytes
