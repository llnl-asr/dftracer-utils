#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/utilities/common/arrow/array_view.h>
#include <dftracer/utils/utilities/common/arrow/attribution.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <dftracer/utils/utilities/common/arrow/join_internal.h>
#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::common::arrow {

namespace {

std::string render_cell(const ArrowArrayView* v, std::int64_t row, KeyKind k) {
    switch (k) {
        case KeyKind::SIGNED:
            return std::to_string(ArrowArrayViewGetIntUnsafe(v, row));
        case KeyKind::UNSIGNED:
            return std::to_string(ArrowArrayViewGetUIntUnsafe(v, row));
        case KeyKind::FLOAT: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g",
                          ArrowArrayViewGetDoubleUnsafe(v, row));
            return std::string(buf);
        }
        default: {
            ArrowStringView s = ArrowArrayViewGetStringUnsafe(v, row);
            return std::string(s.data, static_cast<std::size_t>(s.size_bytes));
        }
    }
}

struct OutRow {
    std::string dim;
    std::string value;
    std::int64_t sel;
    std::int64_t base;
};

}  // namespace

ArrowExportResult attribute(const ArrowSchema* s, const ArrowArray* a,
                            std::uint32_t select_col,
                            const std::uint32_t* dim_cols, std::uint32_t n_dim,
                            std::uint32_t top_k) {
    ArrowArrayView av;
    if (init_array_view(av, const_cast<ArrowSchema*>(s),
                        const_cast<ArrowArray*>(a)) != NANOARROW_OK) {
        throw DFTUtilsException(ErrorCode::INTERNAL,
                                "attribute: failed to view batch");
    }
    struct Guard {
        ArrowArrayView* v;
        ~Guard() { ArrowArrayViewReset(v); }
    } guard{&av};

    const std::int64_t ncols = av.n_children;
    if (select_col >= static_cast<std::uint32_t>(ncols)) {
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "attribute: select column index out of range");
    }
    const ArrowArrayView* sview = av.children[select_col];
    if (sview->storage_type != NANOARROW_TYPE_BOOL) {
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "attribute: select column must be boolean");
    }

    std::vector<const ArrowArrayView*> dviews(n_dim);
    std::vector<KeyKind> dkinds(n_dim);
    for (std::uint32_t d = 0; d < n_dim; ++d) {
        if (dim_cols[d] >= static_cast<std::uint32_t>(ncols)) {
            throw DFTUtilsException(
                ErrorCode::INVALID_ARGUMENT,
                "attribute: dimension column index out of range");
        }
        const ArrowArrayView* v = av.children[dim_cols[d]];
        auto kk = key_kind_from_storage(v->storage_type);
        if (!kk) {
            throw DFTUtilsException(
                ErrorCode::INVALID_ARGUMENT,
                "attribute: unsupported dimension column type");
        }
        dviews[d] = v;
        dkinds[d] = *kk;
    }

    std::vector<
        std::unordered_map<std::string, std::pair<std::int64_t, std::int64_t>>>
        counts(n_dim);
    std::int64_t total_sel = 0;
    std::int64_t total_base = 0;

    for (std::int64_t row = 0; row < av.length; ++row) {
        if (ArrowArrayViewIsNull(sview, row)) continue;
        const bool selected = ArrowArrayViewGetIntUnsafe(sview, row) != 0;
        if (selected)
            ++total_sel;
        else
            ++total_base;
        for (std::uint32_t d = 0; d < n_dim; ++d) {
            if (ArrowArrayViewIsNull(dviews[d], row)) continue;
            std::string key = render_cell(dviews[d], row, dkinds[d]);
            auto& c = counts[d][std::move(key)];
            if (selected)
                ++c.first;
            else
                ++c.second;
        }
    }

    std::vector<OutRow> rows;
    for (std::uint32_t d = 0; d < n_dim; ++d) {
        const char* dname = s->children[dim_cols[d]]->name;
        std::string dim = dname ? dname : "";
        for (auto& e : counts[d]) {
            rows.push_back({dim, e.first, e.second.first, e.second.second});
        }
    }

    const double denom_sel = total_sel > 0 ? static_cast<double>(total_sel) : 0;
    const double denom_base =
        total_base > 0 ? static_cast<double>(total_base) : 0;
    auto diff_of = [&](const OutRow& r) {
        double sf =
            denom_sel > 0 ? static_cast<double>(r.sel) / denom_sel : 0.0;
        double bf =
            denom_base > 0 ? static_cast<double>(r.base) / denom_base : 0.0;
        return sf - bf;
    };

    std::sort(rows.begin(), rows.end(), [&](const OutRow& x, const OutRow& y) {
        double dx = std::abs(diff_of(x));
        double dy = std::abs(diff_of(y));
        if (dx != dy) return dx > dy;
        if (x.dim != y.dim) return x.dim < y.dim;
        return x.value < y.value;
    });

    if (top_k > 0 && rows.size() > top_k) rows.resize(top_k);

    RecordBatchBuilder b;
    b.declare_schema({{"dimension", ColumnType::STRING},
                      {"value", ColumnType::STRING},
                      {"selected_count", ColumnType::INT64},
                      {"baseline_count", ColumnType::INT64},
                      {"selected_frac", ColumnType::DOUBLE},
                      {"baseline_frac", ColumnType::DOUBLE},
                      {"difference", ColumnType::DOUBLE}});
    for (const OutRow& r : rows) {
        double sf =
            denom_sel > 0 ? static_cast<double>(r.sel) / denom_sel : 0.0;
        double bf =
            denom_base > 0 ? static_cast<double>(r.base) / denom_base : 0.0;
        b.append_string(0, r.dim);
        b.append_string(1, r.value);
        b.append_int64(2, r.sel);
        b.append_int64(3, r.base);
        b.append_double(4, sf);
        b.append_double(5, bf);
        b.append_double(6, sf - bf);
        b.end_row();
    }
    return b.finish();
}

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
