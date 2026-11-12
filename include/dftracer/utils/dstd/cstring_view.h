// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_CSTRING_VIEW_H
#define DFTRACER_UTILS_DSTD_CSTRING_VIEW_H

#include <dftracer/utils/dstd/const_string.h>
#include <dftracer/utils/dstd/string.h>

#include <cassert>
#include <cstddef>
#include <string>

namespace dftracer::utils::dstd {

/// A string view whose byte after the end is always '\0', so c_str() can go
/// straight to a C API. It cannot be made from a StringView, which promises
/// no terminator. Take CStringView where a function calls C and StringView
/// elsewhere, never both overloads, since a literal then matches both.
class CStringView {
   public:
    constexpr CStringView() noexcept = default;
    constexpr CStringView(const char* s) noexcept
        : data_(s), size_(std::char_traits<char>::length(s)) {}
    template <class Alloc>
    CStringView(const std::basic_string<char, std::char_traits<char>, Alloc>&
                    s) noexcept
        : data_(s.c_str()), size_(s.size()) {}
    /// The @ref ConstString must leave room for its terminator (len < N).
    template <std::size_t N>
    constexpr CStringView(const ConstString<N>& s) noexcept
        : data_(s.data), size_(s.len) {
        assert(s.len < N);
    }

    /// For a caller that already knows p[n] is '\0'.
    static constexpr CStringView unchecked(const char* p,
                                           std::size_t n) noexcept {
        CStringView v;
        v.data_ = p;
        v.size_ = n;
        return v;
    }

    constexpr const char* c_str() const noexcept { return data_; }
    constexpr const char* data() const noexcept { return data_; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr StringView view() const noexcept { return {data_, size_}; }
    constexpr operator StringView() const noexcept { return {data_, size_}; }

    friend constexpr bool operator==(CStringView a, CStringView b) noexcept {
        return a.view() == b.view();
    }

   private:
    const char* data_ = "";
    std::size_t size_ = 0;
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_CSTRING_VIEW_H
