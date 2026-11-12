// SPDX-License-Identifier: MIT
#include <cxxabi.h>
#include <dftracer/utils/common/symbolize.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dlfcn.h>

#include <cstdlib>
#include <cstring>

namespace dftracer::utils::common {

namespace {

// Extract the bare function name: drop the parameter list (first '('), the
// return type (last space), then the namespace (last "::").
dstd::String extract_name(dstd::StringView fn) {
    const std::size_t paren = fn.find('(');
    if (paren != dstd::StringView::npos) fn = fn.substr(0, paren);
    const std::size_t space = fn.rfind(' ');
    if (space != dstd::StringView::npos) fn = fn.substr(space + 1);
    const std::size_t scope = fn.rfind("::");
    if (scope != dstd::StringView::npos) fn = fn.substr(scope + 2);
    return dstd::String(fn);
}

// Erase everything between matching '<' '>' so template arguments cannot
// interfere with name extraction.
dstd::String strip_templates(dstd::StringView s) {
    dstd::String out;
    int angle = 0;
    for (char c : s) {
        if (c == '<') {
            ++angle;
        } else if (c == '>') {
            if (angle > 0) --angle;
        } else if (angle == 0) {
            out += c;
        }
    }
    return out;
}

// Reduce a demangled coroutine symbol to a readable name. Spawned coroutines
// resolve to a coro::Scope::run_fn_child<...> instantiation whose useful name
// lives inside the template args, so the plain pass (which keeps "::" inside
// the args) recovers it. Direct coroutines with function-type template params
// (e.g. std::function<T ()>) collapse the plain pass to empty; retry with the
// template arguments removed.
dstd::String short_function(dstd::StringView full) {
    // Demangled anonymous-namespace members read as
    // "ns::(anonymous namespace)::fn(...)"; the parenthesis there defeats name
    // extraction, so drop the marker first.
    dstd::String buf;
    if (full.find("(anonymous namespace)::") != dstd::StringView::npos) {
        buf.assign(full);
        const dstd::String marker = "(anonymous namespace)::";
        for (std::size_t p; (p = buf.find(marker)) != dstd::String::npos;) {
            buf.erase(p, marker.size());
        }
        full = buf;
    }
    dstd::String r = extract_name(full);
    if (r.empty()) r = extract_name(strip_templates(full));
    if (!r.empty()) return r;
    // Extraction failed (e.g. a lambda / operator() with no clean enclosing
    // name); show the raw symbol, truncated, rather than an opaque "?".
    constexpr std::size_t MAX_LABEL_LEN = 80;
    dstd::String f(
        full.size() > MAX_LABEL_LEN ? full.substr(0, MAX_LABEL_LEN - 3) : full);
    if (full.size() > MAX_LABEL_LEN) f += "...";
    return f.empty() ? dstd::String("?") : f;
}

}  // namespace

dstd::String symbolize_function(const void* fn) {
    if (fn == nullptr) return "?";
    Dl_info info;
    if (dladdr(fn, &info) != 0 && info.dli_sname != nullptr) {
        int status = 0;
        char* dem =
            abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        dstd::String out = (status == 0 && dem != nullptr)
                               ? short_function(dem)
                               : short_function(info.dli_sname);
        std::free(dem);
        return out;
    }
    if (dladdr(fn, &info) != 0 && info.dli_fbase != nullptr) {
        const char* mod = info.dli_fname ? info.dli_fname : "?";
        if (const char* slash = std::strrchr(mod, '/')) mod = slash + 1;
        const auto off = reinterpret_cast<const char*>(fn) -
                         reinterpret_cast<const char*>(info.dli_fbase);
        return dstd::fmt<"{}+0x{:x}">(mod, static_cast<std::uint64_t>(off));
    }
    return "?";
}

}  // namespace dftracer::utils::common
