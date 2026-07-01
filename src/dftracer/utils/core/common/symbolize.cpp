#include <cxxabi.h>
#include <dftracer/utils/core/common/symbolize.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace dftracer::utils {

namespace {

// Extract the bare function name: drop the parameter list (first '('), the
// return type (last space), then the namespace (last "::").
std::string extract_name(std::string_view fn) {
    const std::size_t paren = fn.find('(');
    if (paren != std::string_view::npos) fn = fn.substr(0, paren);
    const std::size_t space = fn.rfind(' ');
    if (space != std::string_view::npos) fn = fn.substr(space + 1);
    const std::size_t scope = fn.rfind("::");
    if (scope != std::string_view::npos) fn = fn.substr(scope + 2);
    return std::string(fn);
}

// Erase everything between matching '<' '>' so template arguments cannot
// interfere with name extraction.
std::string strip_templates(std::string_view s) {
    std::string out;
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
// resolve to a CoroScope::spawn<...> instantiation whose useful name lives
// inside the template args, so the plain pass (which keeps "::" inside the
// args) recovers it. Direct coroutines with function-type template params
// (e.g. std::function<T ()>) collapse the plain pass to empty; retry with the
// template arguments removed.
std::string short_function(std::string_view full) {
    // Demangled anonymous-namespace members read as
    // "ns::(anonymous namespace)::fn(...)"; the parenthesis there defeats name
    // extraction, so drop the marker first.
    std::string buf;
    if (full.find("(anonymous namespace)::") != std::string_view::npos) {
        buf.assign(full);
        const std::string marker = "(anonymous namespace)::";
        for (std::size_t p; (p = buf.find(marker)) != std::string::npos;) {
            buf.erase(p, marker.size());
        }
        full = buf;
    }
    std::string r = extract_name(full);
    if (r.empty()) r = extract_name(strip_templates(full));
    if (!r.empty()) return r;
    // Extraction failed (e.g. a lambda / operator() with no clean enclosing
    // name); show the raw symbol, truncated, rather than an opaque "?".
    constexpr std::size_t MAX_LABEL_LEN = 80;
    std::string f(
        full.size() > MAX_LABEL_LEN ? full.substr(0, MAX_LABEL_LEN - 3) : full);
    if (full.size() > MAX_LABEL_LEN) f += "...";
    return f.empty() ? std::string("?") : f;
}

}  // namespace

std::string symbolize_function(const void* fn) {
    if (fn == nullptr) return "?";
    Dl_info info;
    if (dladdr(fn, &info) != 0 && info.dli_sname != nullptr) {
        int status = 0;
        char* dem =
            abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        std::string out = (status == 0 && dem != nullptr)
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
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s+0x%llx", mod,
                      static_cast<unsigned long long>(off));
        return buf;
    }
    return "?";
}

}  // namespace dftracer::utils
