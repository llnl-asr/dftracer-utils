#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/dataframe/agg_spill.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/spill.h>  // spill::Dir

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {
namespace {

/// Groups merged before a chunk is finalized and released. Bounds the drain
/// peak to one chunk of partial state plus the finalized rows so far.
constexpr std::int64_t AGG_SPILL_CHUNK_GROUPS = 4096;

/// Ops whose finalized column is List/Struct. concat_columns cannot rejoin a
/// nested column, so a state holding one of these finalizes in a single call
/// instead of in chunks.
bool op_is_nested(AggOp op) {
    return op == AggOp::Hist || op == AggOp::Sample ||
           op == AggOp::ListSorted || op == AggOp::TopK ||
           op == AggOp::BottomK || op == AggOp::ApproxTopK;
}

// A key-ordered stream of single-group states, so the drain merge is a k-way
// merge over readers however the groups are stored.
class GroupSource {
   public:
    virtual ~GroupSource() = default;
    virtual bool valid() const = 0;
    virtual const AggState& state() const = 0;
    virtual void advance() = 0;
};

class RunSource : public GroupSource {
   public:
    explicit RunSource(const std::string& path) : reader_(path) {}
    bool valid() const override { return reader_.valid(); }
    const AggState& state() const override { return reader_.state(); }
    void advance() override { reader_.advance(); }

   private:
    spill::AggRunReader reader_;
};

// The still-live state, read in key order without mutating (and so without
// copying) it: the groups are visited through a sorted index.
class LiveSource : public GroupSource {
   public:
    explicit LiveSource(const AggState& live) : live_(&live) {
        order_.resize(static_cast<std::size_t>(agg_num_groups(live)));
        std::iota(order_.begin(), order_.end(), std::int64_t{0});
        std::sort(order_.begin(), order_.end(),
                  [&](std::int64_t a, std::int64_t b) {
                      return agg_key_cmp(live, a, live, b) < 0;
                  });
        advance();
    }
    bool valid() const override { return valid_; }
    const AggState& state() const override { return *cur_; }
    void advance() override {
        if (at_ >= order_.size()) {
            valid_ = false;
            return;
        }
        cur_ = agg_extract_group(*live_, order_[at_++]);
        valid_ = true;
    }

   private:
    const AggState* live_;
    std::vector<std::int64_t> order_;
    std::size_t at_ = 0;
    AggStatePtr cur_;
    bool valid_ = false;
};

}  // namespace

struct AggSpiller::Impl {
    std::uint64_t budget = 0;
    std::vector<std::shared_ptr<spill::Dir>> dirs;
    std::vector<std::string> runs;
    int next_id = 0;

    const std::string& new_run_path() {
        if (dirs.empty()) dirs.push_back(std::make_shared<spill::Dir>());
        runs.push_back(dirs.back()->run_path(next_id++));
        return runs.back();
    }
};

AggSpiller::AggSpiller(std::uint64_t budget) : impl_(std::make_unique<Impl>()) {
    impl_->budget = resolve_spill_budget(budget);
}
AggSpiller::~AggSpiller() = default;
AggSpiller::AggSpiller(AggSpiller&&) noexcept = default;
AggSpiller& AggSpiller::operator=(AggSpiller&&) noexcept = default;

std::size_t AggSpiller::runs() const { return impl_->runs.size(); }

bool AggSpiller::maybe_spill(AggStatePtr& state) {
    if (!state || impl_->budget == NO_SPILL_BUDGET) return false;
    if (agg_approx_bytes(*state) <= impl_->budget) return false;
    std::vector<AggSpec> specs = agg_specs(*state);
    spill::write_agg_run(*state, impl_->new_run_path());
    DFTRACER_UTILS_LOG_DEBUG(
        "Aggregation spilled run %d (%lld groups) past the %llu byte budget",
        impl_->next_id - 1, static_cast<long long>(agg_num_groups(*state)),
        static_cast<unsigned long long>(impl_->budget));
    state = agg_new(std::move(specs));
    return true;
}

void AggSpiller::adopt(AggSpiller& other) {
    for (auto& d : other.impl_->dirs) impl_->dirs.push_back(std::move(d));
    for (auto& r : other.impl_->runs) impl_->runs.push_back(std::move(r));
    other.impl_->dirs.clear();
    other.impl_->runs.clear();
    // A future run of ours must not reuse an adopted id in the shared dir.
    impl_->next_id = static_cast<int>(impl_->runs.size());
}

DataFrame AggSpiller::drain(const AggState& live,
                            const std::vector<std::string>& key_names) const {
    if (impl_->runs.empty()) return agg_finalize(live, key_names);

    const std::vector<AggSpec> specs = agg_specs(live);
    const bool nested =
        std::any_of(specs.begin(), specs.end(),
                    [](const AggSpec& s) { return op_is_nested(s.op); });
    const std::int64_t chunk = nested ? std::numeric_limits<std::int64_t>::max()
                                      : AGG_SPILL_CHUNK_GROUPS;

    std::vector<std::unique_ptr<GroupSource>> sources;
    sources.reserve(impl_->runs.size() + 1);
    for (const std::string& path : impl_->runs)
        sources.push_back(std::make_unique<RunSource>(path));
    sources.push_back(std::make_unique<LiveSource>(live));

    std::vector<DataFrame> parts;
    AggStatePtr merged = agg_new(specs);
    std::int64_t in_chunk = 0;
    for (;;) {
        int best = -1;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            if (!sources[i]->valid()) continue;
            if (best < 0 ||
                agg_key_cmp(sources[i]->state(), 0,
                            sources[static_cast<std::size_t>(best)]->state(),
                            0) < 0)
                best = static_cast<int>(i);
        }
        if (best < 0) break;
        // Snapshot the winning key: advancing its own source mid-loop would
        // otherwise mutate the state the remaining comparisons read.
        const AggStatePtr win = agg_extract_group(
            sources[static_cast<std::size_t>(best)]->state(), 0);
        for (auto& src : sources) {
            if (!src->valid()) continue;
            if (agg_key_cmp(src->state(), 0, *win, 0) != 0) continue;
            agg_merge(*merged, src->state());
            src->advance();
        }
        if (++in_chunk >= chunk) {
            parts.push_back(agg_finalize(*merged, key_names));
            merged = agg_new(specs);
            in_chunk = 0;
        }
    }
    if (in_chunk > 0 || parts.empty())
        parts.push_back(agg_finalize(*merged, key_names));
    if (parts.size() == 1) return std::move(parts.front());

    std::vector<const DataFrame*> refs;
    refs.reserve(parts.size());
    for (const DataFrame& p : parts) refs.push_back(&p);
    return concat(refs);
}

}  // namespace dftracer::utils::dataframe
