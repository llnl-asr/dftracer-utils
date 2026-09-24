#ifndef DFTRACER_UTILS_TRACE_VIEWS_TYPED_ROWS_H
#define DFTRACER_UTILS_TRACE_VIEWS_TYPED_ROWS_H

#include <dftracer/utils/index/schema_class.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/trace/views/view.h>
#include <simdjson.h>

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::views {

namespace detail::typed_rows {

// Walks a dotted path; a numeric segment also indexes an array.
inline bool find_path(simdjson::dom::element root, std::string_view path,
                      simdjson::dom::element& out) {
    simdjson::dom::element cur = root;
    while (true) {
        const std::size_t dot = path.find('.');
        const std::string_view seg = path.substr(0, dot);
        simdjson::dom::object obj;
        simdjson::dom::array arr;
        if (cur.get(obj) == simdjson::SUCCESS) {
            if (obj.at_key(seg).get(cur) != simdjson::SUCCESS) return false;
        } else if (cur.get(arr) == simdjson::SUCCESS) {
            std::size_t i = 0;
            auto [end, ec] =
                std::from_chars(seg.data(), seg.data() + seg.size(), i);
            if (ec != std::errc{} || end != seg.data() + seg.size() ||
                arr.at(i).get(cur) != simdjson::SUCCESS)
                return false;
        } else {
            return false;
        }
        if (dot == std::string_view::npos) break;
        path.remove_prefix(dot + 1);
    }
    out = cur;
    return true;
}

template <typename V>
bool convert(simdjson::dom::element e, V& out) {
    if constexpr (std::same_as<V, bool>) {
        return e.get(out) == simdjson::SUCCESS;
    } else if constexpr (std::integral<V>) {
        std::int64_t i;
        std::uint64_t u;
        double d;
        if (e.get(i) == simdjson::SUCCESS) {
            if (!std::in_range<V>(i)) return false;
            out = static_cast<V>(i);
            return true;
        }
        if (e.get(u) == simdjson::SUCCESS) {
            if (!std::in_range<V>(u)) return false;
            out = static_cast<V>(u);
            return true;
        }
        if (e.get(d) != simdjson::SUCCESS) return false;
        const auto whole = index::whole_int64(d);
        if (!whole || !std::in_range<V>(*whole)) return false;
        out = static_cast<V>(*whole);
        return true;
    } else if constexpr (std::floating_point<V>) {
        double d;
        if (e.get(d) != simdjson::SUCCESS) return false;
        out = static_cast<V>(d);
        return true;
    } else if constexpr (std::same_as<V, std::string>) {
        std::string_view s;
        if (e.get(s) != simdjson::SUCCESS) return false;
        out.assign(s);
        return true;
    } else {
        static_assert(std::same_as<V, index::Json>);
        out.text.clear();
        json::append_canonical_json(out.text, e);
        return true;
    }
}

template <typename F>
struct FieldRead;
template <typename T, index::FixedString PATH, typename... Options>
struct FieldRead<index::Field<T, PATH, Options...>> {
    static void read(simdjson::dom::element root, T& out) {
        simdjson::dom::element e;
        if (!find_path(root, PATH.view(), e)) return;
        if constexpr (index::detail::Unwrap<T>::OPTIONAL) {
            typename index::detail::Unwrap<T>::type v{};
            if (convert(e, v)) out = std::move(v);
        } else {
            T v{};
            if (convert(e, v)) out = std::move(v);
        }
    }
};

// Converts to the member it initializes, reading that member's field.
struct MemberFill {
    simdjson::dom::element root;
    template <typename U>
    operator U() const {
        static_assert(index::detail::IsField<U>::value,
                      "schema class members must be index::Field<...>");
        U f{};
        FieldRead<U>::read(root, f.value);
        return f;
    }
};

template <std::size_t>
MemberFill fill(simdjson::dom::element root) {
    return {root};
}

template <typename T, std::size_t... I>
T decode(simdjson::dom::element root, std::index_sequence<I...>) {
    return T{fill<I>(root)...};
}

}  // namespace detail::typed_rows

/// Decodes one record into schema class `T` (see index::schema_spec). A
/// missing value, or one that does not convert to the member's type, leaves
/// the member at its default (empty for an optional member).
template <typename T>
T decode_row(simdjson::dom::element record) {
    return detail::typed_rows::decode<T>(
        record, std::make_index_sequence<index::detail::member_count<T>()>{});
}

/// The records `view` selects (its filters, pruning, time window and phase,
/// as collect() reads them), each decoded into schema class `T` by
/// decode_row. Rows are decoded on the scan workers; their order across
/// workers is unspecified.
template <typename T>
dataframe::LazyResult<std::vector<T>> rows(const View& view, Lazy) {
    return view.branch<std::vector<T>>(
        std::function<Deferred<std::vector<T>>(ViewSession&)>(
            [](ViewSession& s) {
                return s.fold<std::vector<T>>(
                    [](std::vector<T>& out, const json::JsonValue& jv,
                       std::string_view) {
                        out.push_back(decode_row<T>(jv.element()));
                    },
                    [](std::vector<T>&& a, std::vector<T>&& b) {
                        if (a.empty()) return std::move(b);
                        a.insert(a.end(), std::make_move_iterator(b.begin()),
                                 std::make_move_iterator(b.end()));
                        return std::move(a);
                    });
            }));
}

template <typename T>
coro::CoroTask<std::vector<T>> rows(const View& view) {
    co_return co_await rows<T>(view, LAZY).collect();
}

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_TYPED_ROWS_H
