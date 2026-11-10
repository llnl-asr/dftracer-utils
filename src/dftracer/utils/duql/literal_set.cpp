// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/literal_set.h>

#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
#include <hs.h>
#endif

#include <limits>

namespace dftracer::utils::duql {

#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
namespace {

struct ThreadState {
    hs_scratch_t* scratch = nullptr;
    dstd::Vector<std::uint64_t> bits;
    ~ThreadState() { hs_free_scratch(scratch); }
};

struct Hits {
    std::uint64_t* bits;
    std::size_t remaining;
};

int on_hit(unsigned id, unsigned long long, unsigned long long, unsigned,
           void* ctx) {
    auto* h = static_cast<Hits*>(ctx);
    h->bits[id >> 6] |= std::uint64_t{1} << (id & 63);
    return --h->remaining == 0 ? 1 : 0;
}

// hs_alloc_scratch allocates and checks on every call, so scan first and
// grow the scratch only when the scan reports it missing or too small.
hs_error_t scan_grow(const hs_database_t* db, dstd::StringView s, Hits& hits,
                     ThreadState& ts) {
    const auto n = static_cast<unsigned>(s.size());
    hs_error_t rc = hs_scan(db, s.data(), n, 0, ts.scratch, on_hit, &hits);
    if (rc != HS_INVALID || hs_alloc_scratch(db, &ts.scratch) != HS_SUCCESS)
        return rc;
    return hs_scan(db, s.data(), n, 0, ts.scratch, on_hit, &hits);
}

}  // namespace
#endif

LiteralSet LiteralSet::make(dstd::Span<const dstd::StringView> literals) {
    LiteralSet set;
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    if (literals.empty()) return set;
    dstd::Vector<const char*> exprs;
    dstd::Vector<std::size_t> lens;
    dstd::Vector<unsigned> flags(literals.size(), HS_FLAG_SINGLEMATCH);
    dstd::Vector<unsigned> idv(literals.size());
    exprs.reserve(literals.size());
    lens.reserve(literals.size());
    for (std::size_t i = 0; i < literals.size(); ++i) {
        exprs.push_back(literals[i].data());
        lens.push_back(literals[i].size());
        idv[i] = static_cast<unsigned>(i);
    }
    hs_database_t* db = nullptr;
    hs_compile_error_t* err = nullptr;
    if (hs_compile_lit_multi(exprs.data(), flags.data(), idv.data(),
                             lens.data(), static_cast<unsigned>(exprs.size()),
                             HS_MODE_BLOCK, nullptr, &db, &err) != HS_SUCCESS) {
        hs_free_compile_error(err);
        return set;
    }
    set.db_ = dstd::adopt_shared_ptr(db, hs_free_database);
    set.size_ = literals.size();
#else
    (void)literals;
#endif
    return set;
}

const std::uint64_t* LiteralSet::scan(dstd::StringView line) const {
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    if (!db_ || line.size() > std::numeric_limits<unsigned>::max())
        return nullptr;
    thread_local ThreadState ts;
    ts.bits.assign((size_ + 63) >> 6, 0);
    if (line.empty()) return ts.bits.data();
    Hits hits{ts.bits.data(), size_};
    const hs_error_t rc = scan_grow(db_.get(), line, hits, ts);
    return rc == HS_SUCCESS || rc == HS_SCAN_TERMINATED ? ts.bits.data()
                                                        : nullptr;
#else
    (void)line;
    return nullptr;
#endif
}

}  // namespace dftracer::utils::duql
