#include <dftracer/utils/dataframe/series.h>

#include <cstdint>
#include <string_view>
#include <vector>

// Out-of-line Series methods over the dftu_series_* C ABI.
namespace dftracer::utils::dataframe {

namespace {
double scalar_as_double(dftu_scalar s) {
    switch (s.kind) {
        case DFTU_SCALAR_TAG_F64:
            return s.value.d;
        case DFTU_SCALAR_TAG_U64:
            return static_cast<double>(s.value.u);
        default:
            return static_cast<double>(s.value.i);
    }
}
}  // namespace

DataType Series::data_type() const {
    DataType dt;
    dt.id = type();
    dt.time_unit = static_cast<TimeUnit>(dftu_series_time_unit(handle_));
    const char* tz = dftu_series_timezone(handle_);
    dt.timezone = tz ? tz : "";
    dt.decimal_precision = dftu_series_decimal_precision(handle_);
    dt.decimal_scale = dftu_series_decimal_scale(handle_);
    dt.fixed_size = dftu_series_fixed_size(handle_);
    const std::int64_t n = num_children();
    for (std::int64_t i = 0; i < n; ++i) {
        Series c = child(i);
        const char* name =
            dftu_series_field_name(handle_, static_cast<int32_t>(i));
        bool default_named =
            dt.id == TypeId::List || dt.id == TypeId::LargeList ||
            dt.id == TypeId::FixedSizeList || dt.id == TypeId::Map;
        dt.fields.push_back(Field{name ? name : (default_named ? "item" : ""),
                                  c.data_type(), true});
    }
    return dt;
}

Series Series::add(const Series& o) const {
    return Series{dftu_series_add(handle_, o.handle_)};
}
Series Series::sub(const Series& o) const {
    return Series{dftu_series_sub(handle_, o.handle_)};
}
Series Series::mul(const Series& o) const {
    return Series{dftu_series_mul(handle_, o.handle_)};
}
Series Series::div(const Series& o) const {
    return Series{dftu_series_div(handle_, o.handle_)};
}

Scalar Series::sum() const {
    return dftu_series_reduce(handle_, DFTU_REDUCE_SUM);
}
Scalar Series::min() const {
    return dftu_series_reduce(handle_, DFTU_REDUCE_MIN);
}
Scalar Series::max() const {
    return dftu_series_reduce(handle_, DFTU_REDUCE_MAX);
}
Scalar Series::product() const { return dftu_series_product(handle_); }
Scalar Series::mode() const { return dftu_series_mode(handle_); }
bool Series::all() const { return dftu_series_all(handle_) != 0; }
bool Series::any() const { return dftu_series_any(handle_) != 0; }
std::int64_t Series::arg_min() const { return dftu_series_arg_min(handle_); }
std::int64_t Series::arg_max() const { return dftu_series_arg_max(handle_); }
std::int64_t Series::count() const { return dftu_series_count(handle_); }
double Series::mean() const {
    const std::int64_t n = count();
    return n == 0 ? 0.0 : scalar_as_double(sum()) / static_cast<double>(n);
}
double Series::variance(bool sample) const {
    return dftu_series_variance(handle_, sample ? 1 : 0);
}
double Series::stddev(bool sample) const {
    return dftu_series_stddev(handle_, sample ? 1 : 0);
}
double Series::skewness() const { return dftu_series_skewness(handle_); }
double Series::kurtosis() const { return dftu_series_kurtosis(handle_); }
double Series::quantile(double q) const {
    return dftu_series_quantile(handle_, q);
}
double Series::median() const { return dftu_series_quantile(handle_, 0.5); }
std::int64_t Series::nunique() const { return dftu_series_nunique(handle_); }

Series Series::rank(RankMethod method, bool descending) const {
    return Series{dftu_series_rank(
        handle_, static_cast<dftu_rank_method>(method), descending ? 1 : 0)};
}
Series Series::rolling(RollingOp op, std::int64_t window) const {
    return Series{
        dftu_series_rolling(handle_, window, static_cast<dftu_rolling_op>(op))};
}
Series Series::rolling_var(std::int64_t window) const {
    return Series{dftu_series_rolling_var(handle_, window)};
}
Series Series::rolling_std(std::int64_t window) const {
    return Series{dftu_series_rolling_std(handle_, window)};
}
Series Series::rolling_median(std::int64_t window) const {
    return Series{dftu_series_rolling_median(handle_, window)};
}
Series Series::rolling_quantile(std::int64_t window, double q) const {
    return Series{dftu_series_rolling_quantile(handle_, window, q)};
}
Series Series::ewm_mean(double alpha) const {
    return Series{dftu_series_ewm_mean(handle_, alpha)};
}
Series Series::ewm_std(double alpha) const {
    return Series{dftu_series_ewm_std(handle_, alpha)};
}
Series Series::cut(const Series& breaks) const {
    return Series{dftu_series_cut(handle_, breaks.handle_)};
}
Series Series::qcut(std::int32_t q) const {
    return Series{dftu_series_qcut(handle_, q)};
}
Series Series::search_sorted(const Series& values) const {
    return Series{dftu_series_search_sorted(handle_, values.handle_)};
}
Series Series::interpolate() const {
    return Series{dftu_series_interpolate(handle_)};
}
Scalar Series::dot(const Series& other) const {
    return dftu_series_dot(handle_, other.handle_);
}

Series Series::cast(TypeId target) const {
    return Series{dftu_series_cast(handle_, static_cast<dftu_dtype>(target))};
}
Series Series::prim(PrimOp op) const {
    return Series{dftu_series_prim(handle_, static_cast<dftu_prim_op>(op))};
}
Series Series::abs() const { return Series{dftu_series_abs(handle_)}; }
Series Series::clip(Scalar lo, Scalar hi) const {
    return Series{dftu_series_clip(handle_, lo, hi)};
}
Series Series::round() const { return Series{dftu_series_round(handle_)}; }
Series Series::fillna(Scalar fill) const {
    return Series{dftu_series_fillna(handle_, fill)};
}
Series Series::cumsum() const { return Series{dftu_series_cumsum(handle_)}; }
Series Series::cummax() const { return Series{dftu_series_cummax(handle_)}; }
Series Series::cummin() const { return Series{dftu_series_cummin(handle_)}; }
Series Series::cum_prod() const {
    return Series{dftu_series_cum_prod(handle_)};
}
Series Series::cum_count() const {
    return Series{dftu_series_cum_count(handle_)};
}
Series Series::ceil() const { return Series{dftu_series_ceil(handle_)}; }
Series Series::floor() const { return Series{dftu_series_floor(handle_)}; }
Series Series::trunc() const { return Series{dftu_series_trunc(handle_)}; }
Series Series::sign() const { return Series{dftu_series_sign(handle_)}; }
Series Series::negate() const { return Series{dftu_series_negate(handle_)}; }
Series Series::diff() const { return Series{dftu_series_diff(handle_)}; }
Series Series::pct_change() const {
    return Series{dftu_series_pct_change(handle_)};
}
Series Series::sqrt() const { return Series{dftu_series_sqrt(handle_)}; }
Series Series::exp() const { return Series{dftu_series_exp(handle_)}; }
Series Series::log() const { return Series{dftu_series_log(handle_)}; }
Series Series::unique() const { return Series{dftu_series_unique(handle_)}; }
Series Series::dictionary_encode() const {
    return Series{dftu_series_dictionary_encode(handle_)};
}
Series Series::null_mask() const {
    return Series{dftu_series_null_mask(handle_)};
}
Series Series::valid_mask() const {
    return Series{dftu_series_valid_mask(handle_)};
}
Series Series::is_nan() const { return Series{dftu_series_is_nan(handle_)}; }
Series Series::is_finite() const {
    return Series{dftu_series_is_finite(handle_)};
}
Series Series::is_infinite() const {
    return Series{dftu_series_is_infinite(handle_)};
}
Series Series::is_unique() const {
    return Series{dftu_series_is_unique(handle_)};
}
Series Series::is_duplicated() const {
    return Series{dftu_series_is_duplicated(handle_)};
}
bool Series::is_sorted(bool descending) const {
    return dftu_series_is_sorted(handle_, descending ? 1 : 0) != 0;
}
Series Series::drop_nulls() const {
    return Series{dftu_series_drop_nulls(handle_)};
}
Series Series::is_in(const Series& values) const {
    return Series{dftu_series_is_in(handle_, values.handle_)};
}
Series Series::sort(bool descending) const {
    return Series{dftu_series_sort(handle_, descending ? 1 : 0)};
}
Series Series::head(std::int64_t n) const {
    return Series{dftu_series_head(handle_, n)};
}
Series Series::tail(std::int64_t n) const {
    return Series{dftu_series_tail(handle_, n)};
}
Series Series::reverse() const { return Series{dftu_series_reverse(handle_)}; }
Series Series::shift(std::int64_t n) const {
    return Series{dftu_series_shift(handle_, n)};
}
Series Series::top_k(std::int64_t k) const {
    return Series{dftu_series_top_k(handle_, k)};
}
Series Series::bottom_k(std::int64_t k) const {
    return Series{dftu_series_bottom_k(handle_, k)};
}
Series Series::sample(std::int64_t n, std::uint64_t seed) const {
    return Series{dftu_series_sample(handle_, n, seed)};
}
Series Series::argsort(bool descending) const {
    return Series{dftu_series_argsort(handle_, descending ? 1 : 0)};
}
Series Series::take(const std::vector<std::int64_t>& indices) const {
    return take(std::span<const std::int64_t>(indices));
}
Series Series::take(std::span<const std::int64_t> indices) const {
    return Series{dftu_series_take(handle_, indices.data(),
                                   static_cast<std::int64_t>(indices.size()))};
}
Series Series::materialize() const {
    return Series{dftu_series_materialize(handle_)};
}
Series Series::where(const Series& mask, const Series& other) const {
    return Series{dftu_series_where(mask.handle_, handle_, other.handle_)};
}
Series Series::filter(const Series& mask) const {
    return Series{dftu_series_filter(handle_, mask.handle_)};
}

Series Series::str_eq(std::string_view rhs) const {
    return Series{dftu_series_str_eq(handle_, rhs.data(),
                                     static_cast<std::int32_t>(rhs.size()))};
}
Series Series::str_contains(std::string_view needle) const {
    return Series{dftu_series_str_contains(
        handle_, needle.data(), static_cast<std::int32_t>(needle.size()))};
}
Series Series::str_starts_with(std::string_view prefix) const {
    return Series{dftu_series_str_starts_with(
        handle_, prefix.data(), static_cast<std::int32_t>(prefix.size()))};
}
Series Series::str_ends_with(std::string_view suffix) const {
    return Series{dftu_series_str_ends_with(
        handle_, suffix.data(), static_cast<std::int32_t>(suffix.size()))};
}
Series Series::str_matches(std::string_view pattern) const {
    return Series{dftu_series_str_matches(
        handle_, pattern.data(), static_cast<std::int32_t>(pattern.size()))};
}
Series Series::str_search(std::string_view pattern) const {
    return Series{dftu_series_str_search(
        handle_, pattern.data(), static_cast<std::int32_t>(pattern.size()))};
}
Series Series::str_like(std::string_view pattern) const {
    return Series{dftu_series_str_like(
        handle_, pattern.data(), static_cast<std::int32_t>(pattern.size()))};
}
Series Series::str_len_bytes() const {
    return Series{dftu_series_str_len_bytes(handle_)};
}
Series Series::str_len_chars() const {
    return Series{dftu_series_str_len_chars(handle_)};
}
Series Series::str_find(std::string_view needle) const {
    return Series{dftu_series_str_find(
        handle_, needle.data(), static_cast<std::int32_t>(needle.size()))};
}
Series Series::fnv1a() const { return Series{dftu_series_fnv1a(handle_)}; }
Series Series::hex64_parse() const {
    return Series{dftu_series_hex64_parse(handle_)};
}
Series Series::hex64_format() const {
    return Series{dftu_series_hex64_format(handle_)};
}
Series Series::to_lowercase() const {
    return Series{dftu_series_to_lowercase(handle_)};
}
Series Series::to_uppercase() const {
    return Series{dftu_series_to_uppercase(handle_)};
}
Series Series::str_strip() const {
    return Series{dftu_series_str_strip(handle_)};
}
Series Series::str_lstrip() const {
    return Series{dftu_series_str_lstrip(handle_)};
}
Series Series::str_rstrip() const {
    return Series{dftu_series_str_rstrip(handle_)};
}
Series Series::str_replace(std::string_view pat, std::string_view repl) const {
    return Series{dftu_series_str_replace(
        handle_, pat.data(), static_cast<std::int32_t>(pat.size()), repl.data(),
        static_cast<std::int32_t>(repl.size()))};
}
Series Series::str_replace_all(std::string_view pat,
                               std::string_view repl) const {
    return Series{dftu_series_str_replace_all(
        handle_, pat.data(), static_cast<std::int32_t>(pat.size()), repl.data(),
        static_cast<std::int32_t>(repl.size()))};
}
Series Series::str_slice(std::int64_t start, std::int64_t length) const {
    return Series{dftu_series_str_slice(handle_, start, length)};
}
Series Series::str_pad_start(std::int64_t width, char fill) const {
    return Series{dftu_series_str_pad_start(handle_, width, fill)};
}
Series Series::str_pad_end(std::int64_t width, char fill) const {
    return Series{dftu_series_str_pad_end(handle_, width, fill)};
}
Series Series::str_zfill(std::int64_t width) const {
    return Series{dftu_series_str_zfill(handle_, width)};
}
Series Series::str_split(std::string_view sep) const {
    return Series{dftu_series_str_split(handle_, sep.data(),
                                        static_cast<std::int32_t>(sep.size()))};
}
Series Series::str_extract(std::string_view pattern, std::int64_t group) const {
    return Series{dftu_series_str_extract(
        handle_, pattern.data(), static_cast<std::int32_t>(pattern.size()),
        group)};
}
Series Series::list_len() const {
    return Series{dftu_series_list_len(handle_)};
}
Series Series::list_get(std::int64_t index) const {
    return Series{dftu_series_list_get(handle_, index)};
}
namespace {
std::int32_t sv_len(std::string_view s) {
    return static_cast<std::int32_t>(s.size());
}
}  // namespace
Series Series::list_join(std::string_view sep) const {
    return Series{dftu_series_list_join(handle_, sep.data(), sv_len(sep))};
}
Series Series::str_case(std::int32_t op) const {
    return Series{dftu_series_str_case(handle_, op)};
}
Series Series::str_is(std::int32_t cls) const {
    return Series{dftu_series_str_is(handle_, cls)};
}
Series Series::str_count(std::string_view pat) const {
    return Series{dftu_series_str_count(handle_, pat.data(), sv_len(pat))};
}
Series Series::str_rfind(std::string_view needle) const {
    return Series{
        dftu_series_str_rfind(handle_, needle.data(), sv_len(needle))};
}
Series Series::str_remove_prefix(std::string_view prefix) const {
    return Series{
        dftu_series_str_remove_prefix(handle_, prefix.data(), sv_len(prefix))};
}
Series Series::str_remove_suffix(std::string_view suffix) const {
    return Series{
        dftu_series_str_remove_suffix(handle_, suffix.data(), sv_len(suffix))};
}
Series Series::str_repeat(std::int64_t n) const {
    return Series{dftu_series_str_repeat(handle_, n)};
}
Series Series::str_center(std::int64_t width, char fill) const {
    return Series{dftu_series_str_center(handle_, width, fill)};
}
Series Series::str_cat(const Series& other) const {
    return Series{dftu_series_str_cat(handle_, other.handle_)};
}
Series Series::str_findall(std::string_view pattern) const {
    return Series{
        dftu_series_str_findall(handle_, pattern.data(), sv_len(pattern))};
}
Series Series::str_partition(std::string_view sep, bool from_right) const {
    return Series{dftu_series_str_partition(handle_, sep.data(), sv_len(sep),
                                            from_right ? 1 : 0)};
}
Series Series::dt_part(std::int32_t part, std::int32_t unit) const {
    return Series{dftu_series_dt_part(handle_, part, unit)};
}
Series Series::dt_round(std::int64_t every, std::int32_t mode) const {
    return Series{dftu_series_dt_round(handle_, every, mode)};
}
Series Series::with_timezone(std::string_view tz) const {
    return Series{dftu_series_with_timezone(
        handle_, tz.data(), static_cast<std::int32_t>(tz.size()))};
}
Series Series::floordiv(const Series& other) const {
    return Series{dftu_series_floordiv(handle_, other.handle_)};
}
Series Series::mod(const Series& other) const {
    return Series{dftu_series_mod(handle_, other.handle_)};
}
Series Series::pow(const Series& other) const {
    return Series{dftu_series_pow(handle_, other.handle_)};
}
Series Series::ffill() const { return Series{dftu_series_ffill(handle_)}; }
Series Series::bfill() const { return Series{dftu_series_bfill(handle_)}; }

Series Series::logical_and(const Series& o) const {
    return Series{dftu_series_logical(handle_, o.handle_, DFTU_LOGICAL_AND)};
}
Series Series::logical_or(const Series& o) const {
    return Series{dftu_series_logical(handle_, o.handle_, DFTU_LOGICAL_OR)};
}
Series Series::logical_not() const {
    return Series{dftu_series_logical_not(handle_)};
}

}  // namespace dftracer::utils::dataframe
