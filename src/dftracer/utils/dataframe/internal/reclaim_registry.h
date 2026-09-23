#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_RECLAIM_REGISTRY_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_RECLAIM_REGISTRY_H

#include <dftracer/utils/dataframe/lazyframe.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace dftracer::utils::dataframe {

// The live stages of one plan, for the driver's memory pass. A stage joins
// through Cursor::attach when the plan lowering builds it and leaves from
// ~Cursor, so the set is exact whoever owns the stage at the time (a plugin
// node takes ownership of its input and may drop it early). The mutex is for
// a stage destroyed off the driving thread; the driver reads a snapshot.
class ReclaimRegistry {
   public:
    void add(Cursor* c) {
        std::lock_guard<std::mutex> lock(m_);
        stages_.push_back(c);
    }
    void remove(Cursor* c) {
        std::lock_guard<std::mutex> lock(m_);
        stages_.erase(std::remove(stages_.begin(), stages_.end(), c),
                      stages_.end());
    }
    std::vector<Cursor*> snapshot() const {
        std::lock_guard<std::mutex> lock(m_);
        return stages_;
    }

   private:
    mutable std::mutex m_;
    std::vector<Cursor*> stages_;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_RECLAIM_REGISTRY_H
