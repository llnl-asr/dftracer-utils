// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/versions.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/layout.h>

#include <algorithm>

namespace dftracer::utils::index::store::layout {

dstd::StringView ext_name(Ext ext) {
    switch (ext) {
        case Ext::Members:
            return "core.members";
        case Ext::Zonemap:
            return "zonemap";
        case Ext::Bloom:
            return "bloom";
        case Ext::Counts:
            return "counts";
        case Ext::Postings:
            return "postings";
        case Ext::Stats:
            return "dft.stats";
        case Ext::Catalog:
            return "core.catalog";
        case Ext::Profile:
            return "core.profile";
        case Ext::Metadata:
            return "dft.metadata";
        case Ext::Plugin:
            return "plugin";
        case Ext::Agg:
            return "dftracer.agg";
        case Ext::Rowset:
            return "core.rowset";
    }
    return "";
}

// The index format version of VERSION, so a bump leaves every stored copy in a
// namespace this build does not read and each file is indexed again.
std::uint32_t ext_version(Ext /*ext*/) {
    return DFTRACER_UTILS_INDEX_FORMAT_VERSION;
}

namespace {
template <typename T>
T read_be_checked(dstd::StringView bytes) {
    if (bytes.size() < sizeof(T))
        throw common::DFTUtilsException(common::ErrorCode::Parse,
                                        "index layout: truncated integer");
    return dstd::read_be<T>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()));
}
}  // namespace

std::uint16_t read_u16(dstd::StringView bytes) {
    return read_be_checked<std::uint16_t>(bytes);
}
std::uint32_t read_u32(dstd::StringView bytes) {
    return read_be_checked<std::uint32_t>(bytes);
}
std::uint64_t read_u64(dstd::StringView bytes) {
    return read_be_checked<std::uint64_t>(bytes);
}

dstd::String prefix_end(dstd::StringView prefix) {
    dstd::String end(prefix);
    while (!end.empty()) {
        auto& last = end.back();
        if (static_cast<unsigned char>(last) != 0xFFU) {
            last = static_cast<char>(static_cast<unsigned char>(last) + 1);
            return end;
        }
        end.pop_back();
    }
    return dstd::String(prefix) + dstd::String(8, '\xFF');
}

dstd::String encode_path_stat(const PathStat& stat) {
    dstd::String out;
    out.push_back(static_cast<char>(stat.type));
    out.push_back(static_cast<char>(stat.seen));
    dstd::append_u64_be(out, stat.count);
    return out;
}

dstd::Optional<PathStat> decode_path_stat(dstd::StringView body) {
    if (body.size() != 10) return dstd::nullopt;
    const auto type = static_cast<std::uint8_t>(body[0]);
    if (type > static_cast<std::uint8_t>(PathType::Array)) return dstd::nullopt;
    return PathStat{static_cast<PathType>(type),
                    static_cast<std::uint8_t>(body[1]),
                    read_u64(body.substr(2))};
}

namespace {

// A list as {count u32, then per value {length u32, bytes}}; an over-cap
// (nullopt) list is the count OVER_CAP.
constexpr std::uint32_t OVER_CAP = 0xFFFFFFFFU;

void append_list(dstd::String& out,
                 const dstd::Optional<dstd::Vector<dstd::String>>& list) {
    if (!list) {
        dstd::append_u32_be(out, OVER_CAP);
        return;
    }
    dstd::append_u32_be(out, static_cast<std::uint32_t>(list->size()));
    for (const auto& v : *list) {
        dstd::append_u32_be(out, static_cast<std::uint32_t>(v.size()));
        out.append(v);
    }
}

bool read_list(dstd::StringView& in,
               dstd::Optional<dstd::Vector<dstd::String>>& list) {
    if (in.size() < 4) return false;
    const std::uint32_t n = read_u32(in);
    in.remove_prefix(4);
    if (n == OVER_CAP) {
        list.reset();
        return true;
    }
    list.emplace();
    for (std::uint32_t i = 0; i < n; ++i) {
        if (in.size() < 4) return false;
        const std::uint32_t len = read_u32(in);
        in.remove_prefix(4);
        if (in.size() < len) return false;
        list->emplace_back(in.substr(0, len));
        in.remove_prefix(len);
    }
    return true;
}

}  // namespace

dstd::String encode_chunk_metadata(const ChunkMetadata& metadata) {
    dstd::String body;
    dstd::append_u64_be(body, metadata.records);
    append_list(body, metadata.names);
    append_list(body, metadata.paths);
    return body;
}

dstd::Optional<ChunkMetadata> decode_chunk_metadata(dstd::StringView body) {
    if (body.size() < 8) return dstd::nullopt;
    ChunkMetadata m;
    m.records = read_u64(body);
    body.remove_prefix(8);
    if (!read_list(body, m.names) || !read_list(body, m.paths) || !body.empty())
        return dstd::nullopt;
    return m;
}

}  // namespace dftracer::utils::index::store::layout

namespace dftracer::utils::index::store {

dstd::StringView extension_name(IndexExtension ext) {
    return layout::ext_name(ext);
}

dstd::Optional<IndexExtension> parse_extension(dstd::StringView name) {
    for (auto ext : ALL_EXTENSIONS)
        if (layout::ext_name(ext) == name) return ext;
    return dstd::nullopt;
}

void add_metadata_value(dstd::Optional<dstd::Vector<dstd::String>>& list,
                        dstd::StringView value) {
    if (!list) return;
    auto it = std::lower_bound(list->begin(), list->end(), value);
    if (it != list->end() && *it == value) return;
    if (list->size() == METADATA_VALUES_CAP) {
        list.reset();
        return;
    }
    list->emplace(it, value);
}

}  // namespace dftracer::utils::index::store
