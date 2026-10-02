#include <ankerl/unordered_dense.h>
#include <dftracer/utils/dataframe/internal/string_reader.h>
#include <dftracer/utils/dataframe/internal/value_ids.h>

#include <cstring>
#include <string_view>

namespace dftracer::utils::dataframe {

namespace {

constexpr char BOOL_BYTES[2] = {0, 1};

struct Builder {
    ankerl::unordered_dense::map<std::string_view, std::int32_t> map;
    ValueIds& out;
    bool is_string;
    bool is_bool;
    std::size_t width;

    std::int32_t intern(std::string_view s, std::int64_t row) {
        auto [it, fresh] =
            map.try_emplace(s, static_cast<std::int32_t>(out.first.size()));
        if (fresh) out.first.push_back(row);
        return it->second;
    }

    std::int32_t null(std::int64_t row) {
        if (out.null_id < 0) {
            out.null_id = static_cast<std::int32_t>(out.first.size());
            out.first.push_back(row);
        }
        return out.null_id;
    }

    bool key_at(const dftu_series* c, std::int64_t i, std::string_view& s) {
        if (is_string) return str_at(c, i, s);
        for (;;) {
            if (c->validity && !((c->validity->data()[i >> 3] >> (i & 7)) & 1))
                return false;
            switch (c->encoding) {
                case Encoding::Dictionary:
                    i = reinterpret_cast<const std::int32_t*>(
                        c->data->data())[i];
                    c = c->child().get();
                    break;
                case Encoding::Selection: {
                    const std::int64_t idx =
                        reinterpret_cast<const std::int64_t*>(
                            c->data->data())[i];
                    if (idx < 0) return false;
                    i = idx;
                    c = c->child().get();
                    break;
                }
                case Encoding::Chunked: {
                    std::int64_t local = 0;
                    c = &c->chunk_at(i, local);
                    i = local;
                    break;
                }
                default: {
                    const auto* d = c->data->data();
                    if (is_bool)
                        s = std::string_view(
                            BOOL_BYTES + ((d[i >> 3] >> (i & 7)) & 1), 1);
                    else
                        s = std::string_view(
                            reinterpret_cast<const char*>(d) +
                                static_cast<std::size_t>(i) * width,
                            width);
                    return true;
                }
            }
        }
    }

    void fill(const dftu_series* c, std::int64_t base) {
        std::int32_t* ids = out.ids.data() + base;
        std::string_view s;
        if (c->is_chunked()) {
            const auto* starts =
                reinterpret_cast<const std::int64_t*>(c->data->data());
            for (std::size_t k = 0; k < c->nested.size(); ++k)
                fill(c->nested[k].series.get(), base + starts[k]);
            return;
        }
        if (c->encoding == Encoding::Dictionary && c->child()) {
            const dftu_series* dict = c->child().get();
            const auto k = static_cast<std::uint32_t>(dict->length);
            const auto* codes =
                reinterpret_cast<const std::int32_t*>(c->data->data());
            std::vector<std::int32_t> entry(k, -2);
            for (std::int64_t i = 0; i < c->length; ++i) {
                const std::int64_t row = base + i;
                if (c->validity &&
                    !((c->validity->data()[i >> 3] >> (i & 7)) & 1)) {
                    ids[i] = null(row);
                    continue;
                }
                const auto code = static_cast<std::uint32_t>(codes[i]);
                if (code >= k) {
                    ids[i] = null(row);
                    continue;
                }
                std::int32_t& e = entry[code];
                if (e == -2)
                    e = key_at(dict, code, s) ? intern(s, row) : null(row);
                ids[i] = e;
            }
            return;
        }
        for (std::int64_t i = 0; i < c->length; ++i)
            ids[i] = key_at(c, i, s) ? intern(s, base + i) : null(base + i);
    }
};

}  // namespace

bool value_ids(const dftu_series& v, ValueIds& out) {
    if (value_domain(v.type) == ValueDomain::None) return false;
    out.ids.assign(static_cast<std::size_t>(v.length), 0);
    out.first.clear();
    out.null_id = -1;
    const TypeId narrow = narrow_varwidth_type(v.type);
    Builder b{{},
              out,
              narrow == TypeId::String || narrow == TypeId::Binary,
              v.type == TypeId::Bool,
              0};
    if (v.type == TypeId::FixedSizeBinary)
        b.width = static_cast<std::size_t>(v.fixed_size());
    else
        b.width = byte_width(v.type).value_or(0);
    b.map.reserve(64);
    b.fill(&v, 0);
    return true;
}

}  // namespace dftracer::utils::dataframe
