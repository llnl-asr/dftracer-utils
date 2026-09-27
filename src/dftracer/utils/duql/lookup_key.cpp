#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/duql/lookup.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace dftracer::utils::duql {

namespace {

constexpr char INT_TAG = 'i';
constexpr char DOUBLE_TAG = 'd';
constexpr char STRING_TAG = 's';
constexpr char BOOL_TAG = 'b';

void put_u64(std::string& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i)
        out += static_cast<char>((v >> (i * 8)) & 0xFF);
}

std::uint64_t get_u64(std::string_view s) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) |
            static_cast<unsigned char>(s[static_cast<std::size_t>(i)]);
    return v;
}

}  // namespace

void append_int_key(std::string& out, bool negative, std::uint64_t magnitude) {
    out += INT_TAG;
    out += negative && magnitude != 0 ? '-' : '+';
    put_u64(out, magnitude);
}

bool append_double_key(std::string& out, double v) {
    if (std::isnan(v)) return false;
    if (std::trunc(v) == v && std::abs(v) < 18446744073709551616.0) {
        const bool neg = v < 0;
        append_int_key(out, neg, static_cast<std::uint64_t>(neg ? -v : v));
        return true;
    }
    out += DOUBLE_TAG;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    put_u64(out, bits);
    return true;
}

void append_string_key(std::string& out, std::string_view v) {
    out += STRING_TAG;
    put_u64(out, v.size());
    out += v;
}

void append_bool_key(std::string& out, bool v) {
    out += BOOL_TAG;
    out += v ? '1' : '0';
}

std::string key_text(std::string_view key) {
    std::string out;
    std::size_t parts = 0;
    while (!key.empty()) {
        if (parts++ > 0) out += ", ";
        const char tag = key[0];
        key.remove_prefix(1);
        if (tag == INT_TAG) {
            if (key[0] == '-') out += '-';
            out += std::to_string(get_u64(key.substr(1)));
            key.remove_prefix(9);
        } else if (tag == DOUBLE_TAG) {
            const std::uint64_t bits = get_u64(key);
            double d = 0;
            std::memcpy(&d, &bits, sizeof d);
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.17g", d);
            out += buf;
            key.remove_prefix(8);
        } else if (tag == STRING_TAG) {
            const auto n = static_cast<std::size_t>(get_u64(key));
            out += '"';
            out += key.substr(8, n);
            out += '"';
            key.remove_prefix(8 + n);
        } else {
            out += key[0] == '1' ? "true" : "false";
            key.remove_prefix(1);
        }
    }
    return parts > 1 ? "(" + out + ")" : out;
}

const std::vector<std::int64_t>* LookupTable::find(std::string_view key) const {
    const auto it = rows.find(key);
    return it == rows.end() ? nullptr : &it->second;
}

void conflict(const LookupTable& table, std::string_view column,
              std::string_view key) {
    throw DFTUtilsException(
        ErrorCode::INVALID_ARGUMENT,
        "View::duql '" + table.name + "' has rows with different values of '" +
            std::string(column) + "' for key " + key_text(key) +
            "; pick one with a narrower let, or read every row with "
            "'lookup ... into'");
}

}  // namespace dftracer::utils::duql
