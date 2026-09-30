#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/kernels/order.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <string>

namespace dftracer::utils::dataframe {

namespace {

std::optional<ColumnView::Kind> kind_of(TypeId t) {
    switch (physical_type(t)) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
            return ColumnView::Kind::Signed;
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            return ColumnView::Kind::Unsigned;
        case TypeId::Float32:
        case TypeId::Float64:
            return ColumnView::Kind::Float;
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
            return ColumnView::Kind::Bytes;
        default:
            return std::nullopt;
    }
}

template <class T>
int three_way(T a, T b) {
    return a < b ? -1 : (a > b ? 1 : 0);
}

int compare_doubles(double a, double b) {
    const bool na = std::isnan(a);
    const bool nb = std::isnan(b);
    if (na || nb) return na == nb ? 0 : (na ? 1 : -1);
    return three_way(a, b);
}

}  // namespace

bool ColumnView::supports(TypeId t) noexcept { return kind_of(t).has_value(); }

ColumnView::ColumnView(const Series& column)
    : column_(column.encoding() == Encoding::Flat ? column.share()
                                                  : column.materialize()) {
    type_ = column_.type();
    const auto kind = kind_of(type_);
    if (!kind)
        throw std::invalid_argument(std::string("column type '") +
                                    type_name(type_) + "' has no order");
    kind_ = *kind;
    physical_ = physical_type(type_);
    const dftu_series& h = *column_.handle();
    length_ = h.length;
    data_ = h.data ? h.data->data() : nullptr;
    validity_ = h.validity ? h.validity->data() : nullptr;
    if (kind_ == Kind::Bytes) {
        if (h.offsets64)
            off64_ = reinterpret_cast<const std::int64_t*>(h.offsets64->data());
        else
            off32_ = reinterpret_cast<const std::int32_t*>(h.offsets->data());
    }
}

ColumnView::ColumnView(const ColumnView& other)
    : column_(other.column_.share()),
      kind_(other.kind_),
      type_(other.type_),
      physical_(other.physical_),
      length_(other.length_),
      data_(other.data_),
      validity_(other.validity_),
      off32_(other.off32_),
      off64_(other.off64_) {}

ColumnView& ColumnView::operator=(const ColumnView& other) {
    if (this != &other) *this = ColumnView(other);
    return *this;
}

std::int64_t ColumnView::get_int(std::int64_t i) const noexcept {
    const auto at = static_cast<std::size_t>(i);
    switch (physical_) {
        case TypeId::Bool:
            return (data_[at >> 3] >> (at & 7)) & 1;
        case TypeId::Int8:
            return reinterpret_cast<const std::int8_t*>(data_)[at];
        case TypeId::Int16:
            return reinterpret_cast<const std::int16_t*>(data_)[at];
        case TypeId::Int32:
            return reinterpret_cast<const std::int32_t*>(data_)[at];
        default:
            return reinterpret_cast<const std::int64_t*>(data_)[at];
    }
}

std::uint64_t ColumnView::get_uint(std::int64_t i) const noexcept {
    const auto at = static_cast<std::size_t>(i);
    switch (physical_) {
        case TypeId::Uint8:
            return reinterpret_cast<const std::uint8_t*>(data_)[at];
        case TypeId::Uint16:
            return reinterpret_cast<const std::uint16_t*>(data_)[at];
        case TypeId::Uint32:
            return reinterpret_cast<const std::uint32_t*>(data_)[at];
        default:
            return reinterpret_cast<const std::uint64_t*>(data_)[at];
    }
}

double ColumnView::get_double(std::int64_t i) const noexcept {
    switch (kind_) {
        case Kind::Signed:
            return static_cast<double>(get_int(i));
        case Kind::Unsigned:
            return static_cast<double>(get_uint(i));
        default:
            if (physical_ == TypeId::Float32)
                return static_cast<double>(reinterpret_cast<const float*>(
                    data_)[static_cast<std::size_t>(i)]);
            return reinterpret_cast<const double*>(
                data_)[static_cast<std::size_t>(i)];
    }
}

std::string_view ColumnView::get_bytes(std::int64_t i) const noexcept {
    const auto at = static_cast<std::size_t>(i);
    const char* d = reinterpret_cast<const char*>(data_);
    if (off64_ != nullptr)
        return std::string_view(
            d + off64_[at],
            static_cast<std::size_t>(off64_[at + 1] - off64_[at]));
    return std::string_view(
        d + off32_[at], static_cast<std::size_t>(off32_[at + 1] - off32_[at]));
}

int ColumnView::compare(std::int64_t a, const ColumnView& other,
                        std::int64_t b) const noexcept {
    switch (kind_) {
        case Kind::Signed:
            return three_way(get_int(a), other.get_int(b));
        case Kind::Unsigned:
            return three_way(get_uint(a), other.get_uint(b));
        case Kind::Float:
            return compare_doubles(get_double(a), other.get_double(b));
        default: {
            const std::string_view x = get_bytes(a);
            const std::string_view y = other.get_bytes(b);
            const int c = x.compare(y);
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
    }
}

std::vector<std::int64_t> order_rows(const std::vector<ColumnView>& keys,
                                     std::int64_t n) {
    std::vector<std::int64_t> order(static_cast<std::size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    if (keys.empty()) return order;
    std::sort(order.begin(), order.end(), [&](std::int64_t a, std::int64_t b) {
        for (const ColumnView& k : keys) {
            const bool na = k.is_null(a);
            const bool nb = k.is_null(b);
            if (na || nb) {
                if (na && nb) continue;
                return nb;
            }
            const int c = k.compare(a, b);
            if (c != 0) return c < 0;
        }
        return a < b;
    });
    return order;
}

bool same_keys(const std::vector<ColumnView>& keys, std::int64_t a,
               std::int64_t b) {
    for (const ColumnView& k : keys) {
        const bool na = k.is_null(a);
        const bool nb = k.is_null(b);
        if (na != nb) return false;
        if (!na && k.compare(a, b) != 0) return false;
    }
    return true;
}

std::int64_t run_end(const std::vector<ColumnView>& keys,
                     const std::vector<std::int64_t>& order,
                     std::int64_t begin) {
    const auto n = static_cast<std::int64_t>(order.size());
    const std::int64_t rep = order[static_cast<std::size_t>(begin)];
    std::int64_t end = begin + 1;
    while (end < n &&
           same_keys(keys, rep, order[static_cast<std::size_t>(end)]))
        ++end;
    return end;
}

}  // namespace dftracer::utils::dataframe
