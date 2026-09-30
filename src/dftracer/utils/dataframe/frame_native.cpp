#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dataframe/internal/spill.h>

#include <cstring>
#include <exception>

namespace dftracer::utils::dataframe {

namespace {

constexpr char MAGIC[4] = {'D', 'F', 'T', 'N'};
constexpr std::uint32_t VERSION = 1;

template <class T>
void put(std::string& out, T v) {
    out.append(reinterpret_cast<const char*>(&v), sizeof v);
}

template <class T>
bool take(const std::uint8_t*& p, const std::uint8_t* end, T& v) {
    if (static_cast<std::size_t>(end - p) < sizeof v) return false;
    std::memcpy(&v, p, sizeof v);
    p += sizeof v;
    return true;
}

}  // namespace

std::string frame_to_native(const DataFrame& f) {
    std::string out(MAGIC, sizeof MAGIC);
    put<std::uint32_t>(out, VERSION);
    put<std::int64_t>(out, f.num_rows());
    put<std::uint32_t>(out, static_cast<std::uint32_t>(f.columns.size()));
    for (std::size_t c = 0; c < f.columns.size(); ++c) {
        const std::string& name = f.names[c];
        put<std::uint32_t>(out, static_cast<std::uint32_t>(name.size()));
        out += name;
        spill::put_series(out, f.columns[c]);
    }
    return out;
}

std::optional<DataFrame> frame_from_native(std::string_view bytes) noexcept {
    try {
        if (bytes.size() < sizeof MAGIC ||
            std::memcmp(bytes.data(), MAGIC, sizeof MAGIC) != 0)
            return std::nullopt;
        const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
        const auto* end = p + bytes.size();
        p += sizeof MAGIC;
        std::uint32_t version = 0;
        std::int64_t rows = 0;
        std::uint32_t columns = 0;
        if (!take(p, end, version) || version != VERSION ||
            !take(p, end, rows) || !take(p, end, columns))
            return std::nullopt;
        DataFrame f;
        for (std::uint32_t c = 0; c < columns; ++c) {
            std::uint32_t name_bytes = 0;
            if (!take(p, end, name_bytes) ||
                static_cast<std::size_t>(end - p) < name_bytes)
                return std::nullopt;
            f.names.emplace_back(reinterpret_cast<const char*>(p), name_bytes);
            p += name_bytes;
            f.columns.push_back(spill::get_series(p, end));
            if (f.columns.back().length() != rows) return std::nullopt;
        }
        if (p != end) return std::nullopt;
        return f;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace dftracer::utils::dataframe
