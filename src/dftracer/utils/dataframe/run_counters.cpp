// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/internal/run_counters.h>

#include <algorithm>

namespace dftracer::utils::dataframe {

void RunCounters::add(dstd::StringView owner, dstd::StringView name,
                      std::int64_t delta) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (CounterTotal& e : entries_)
        if (dstd::StringView(e.owner) == owner &&
            dstd::StringView(e.name) == name) {
            e.value += delta;
            return;
        }
    entries_.push_back(
        CounterTotal{dstd::String(owner), dstd::String(name), delta});
}

dstd::Vector<CounterTotal> RunCounters::totals() const {
    dstd::Vector<CounterTotal> out;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        out = entries_;
    }
    std::sort(out.begin(), out.end(),
              [](const CounterTotal& a, const CounterTotal& b) {
                  if (a.owner != b.owner) return a.owner < b.owner;
                  return a.name < b.name;
              });
    return out;
}

}  // namespace dftracer::utils::dataframe
