// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_TYPED_ROWS_H
#define DFTRACER_UTILS_TRACE_VIEWS_TYPED_ROWS_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <dftracer/utils/json/value_path.h>
#include <dftracer/utils/trace/index/schema_class.h>
#include <dftracer/utils/trace/views/view.h>

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

namespace dftracer::utils::trace::views {

namespace detail::typed_rows {

template <typename V>
bool convert(json::Value& e, V& out) {
    if constexpr (std::same_as<V, bool>) {
        const auto b = e.as_bool();
        if (!b) return false;
        out = *b;
        return true;
    } else if constexpr (std::integral<V>) {
        const json::Value::Type type = e.type();
        if (type == json::Value::Type::Int) {
            const auto i = e.as_int();
            if (!i || !std::in_range<V>(*i)) return false;
            out = static_cast<V>(*i);
            return true;
        }
        if (type != json::Value::Type::Double) return false;
        const dstd::StringView token = e.raw();
        if (!token.empty() && token.front() != '-' &&
            token.find_first_of(".eE") == dstd::StringView::npos) {
            const dstd::Optional<std::uint64_t> u =
                dstd::from_chars<std::uint64_t>(token);
            if (u) {
                if (!std::in_range<V>(*u)) return false;
                out = static_cast<V>(*u);
                return true;
            }
        }
        const auto d = e.as_double();
        if (!d) return false;
        const auto whole = trace::index::whole_int64(*d);
        if (!whole || !std::in_range<V>(*whole)) return false;
        out = static_cast<V>(*whole);
        return true;
    } else if constexpr (std::floating_point<V>) {
        const auto d = e.as_double();
        if (!d) return false;
        out = static_cast<V>(*d);
        return true;
    } else if constexpr (std::same_as<V, dstd::String>) {
        const auto s = e.as_string();
        if (!s) return false;
        out.assign(*s);
        return true;
    } else {
        static_assert(std::same_as<V, trace::index::Json>);
        out.text = json::canonical_json_text(e.raw());
        return true;
    }
}

template <typename F>
struct FieldRead;
template <typename T, trace::index::FixedString PATH, typename... Options>
struct FieldRead<trace::index::Field<T, PATH, Options...>> {
    static void read(json::Value& root, T& out) {
        auto fill = [&out](json::Value& e) {
            if constexpr (trace::index::detail::Unwrap<T>::OPTIONAL) {
                typename trace::index::detail::Unwrap<T>::type v{};
                if (!convert(e, v)) return false;
                out = std::move(v);
            } else {
                T v{};
                if (!convert(e, v)) return false;
                out = std::move(v);
            }
            return true;
        };
        json::at_path(root, PATH.view(), fill);
    }
};

// Converts to the member it initializes, reading that member's field.
struct MemberFill {
    json::Value& root;
    template <typename U>
    operator U() const {
        static_assert(trace::index::detail::IsField<U>::value,
                      "schema class members must be trace::index::Field<...>");
        U f{};
        FieldRead<U>::read(root, f.value);
        return f;
    }
};

template <std::size_t>
MemberFill fill(json::Value& root) {
    return {root};
}

template <typename T, std::size_t... I>
T decode(json::Value& root, std::index_sequence<I...>) {
    return T{fill<I>(root)...};
}

}  // namespace detail::typed_rows

/// Decodes one record into schema class `T` (see trace::index::schema_spec). A
/// missing value, or one that does not convert to the member's type, leaves
/// the member at its default (empty for an optional member). Reads `record`
/// on demand; when a read hits a syntax error the record is marked broken
/// and the members read so far are not meaningful.
template <typename T>
T decode_row(json::Value& record) {
    return detail::typed_rows::decode<T>(
        record,
        std::make_index_sequence<trace::index::detail::member_count<T>()>{});
}

/// The records `view` selects (its filters, pruning, time window and phase,
/// as collect() reads them), each decoded into schema class `T` by
/// decode_row. Rows are decoded on the scan workers; their order across
/// workers is unspecified.
template <typename T>
dataframe::LazyResult<dstd::Vector<T>> rows(const View& view, Lazy) {
    return view.branch<dstd::Vector<T>>(
        dstd::Function<Deferred<dstd::Vector<T>>(ViewSession&)>(
            [](ViewSession& s) {
                return s.fold<dstd::Vector<T>>(
                    [](dstd::Vector<T>& out, json::Value& jv,
                       dstd::StringView) {
                        T row = decode_row<T>(jv);
                        if (!json::detail::ValueAccess::broken(jv))
                            out.push_back(std::move(row));
                    },
                    [](dstd::Vector<T>&& a, dstd::Vector<T>&& b) {
                        if (a.empty()) return std::move(b);
                        a.insert(a.end(), std::make_move_iterator(b.begin()),
                                 std::make_move_iterator(b.end()));
                        return std::move(a);
                    });
            }));
}

template <typename T>
coro::CoroTask<dstd::Result<dstd::Vector<T>>> rows(const View& view) {
    auto lazy = rows<T>(view, LAZY);
    auto collecting = lazy.collect();
    co_return co_await std::move(collecting);
}

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_TYPED_ROWS_H
