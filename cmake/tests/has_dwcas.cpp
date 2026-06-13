// Feature check: is a 16-byte atomic CAS (DWCAS) lock-free with the current
// flags? ObjectPool uses it for its fast ABA-safe path. The static_assert makes
// a lock-based libatomic fallback fail try_compile() so the build can skip it.

#include <atomic>
#include <cstdint>

struct alignas(16) Head {
  void* ptr;
  std::uint64_t tag;
};

static_assert(std::atomic<Head>::is_always_lock_free,
              "16-byte atomic is not lock-free with the current flags");

int main() {
  std::atomic<Head> head{Head{nullptr, 0}};
  Head expected = head.load(std::memory_order_acquire);
  Head desired{&head, expected.tag + 1};
  head.compare_exchange_weak(expected, desired, std::memory_order_release,
                             std::memory_order_relaxed);
  return 0;
}
