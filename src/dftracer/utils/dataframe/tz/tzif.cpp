// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/internal/tz.h>
#include <dftracer/utils/dataframe/tz/footer.h>
#include <dftracer/utils/dstd/big_endian.h>

#include <cstring>

// The TZif format of RFC 8536: a header and a 32-bit data block, and in
// version 2 and later a second header, a 64-bit block and a POSIX TZ footer.
// The reader takes the 64-bit block when there is one. Leap seconds are
// skipped, as Python's zoneinfo does.

namespace dftracer::utils::dataframe::tz {

namespace {

// Past this an offset breaks the window that Zone::resolve_local searches.
constexpr std::int32_t MAX_UTC_OFFSET = 90'000;
constexpr std::size_t HEADER_SIZE = 44;

struct Counts {
    std::uint32_t is_ut = 0;
    std::uint32_t is_std = 0;
    std::uint32_t leap = 0;
    std::uint32_t time = 0;
    std::uint32_t type = 0;
    std::uint32_t chars = 0;
};

struct Header {
    char version = 0;
    Counts counts;
};

dstd::Result<Header> read_header(dstd::BeReader& in, dstd::StringView name) {
    const std::uint8_t* raw = in.take(HEADER_SIZE);
    if (raw == nullptr || std::memcmp(raw, "TZif", 4) != 0)
        return common::make_error<"time zone '{}': not a TZif file">(
            common::ErrorCode::Parse, name);
    Header h;
    h.version = static_cast<char>(raw[4]);
    if (h.version != 0 && (h.version < '2' || h.version > '4'))
        return common::make_error<"time zone '{}': unknown TZif version">(
            common::ErrorCode::Parse, name);
    dstd::BeReader counts(raw + 20, HEADER_SIZE - 20);
    counts.u32(h.counts.is_ut);
    counts.u32(h.counts.is_std);
    counts.u32(h.counts.leap);
    counts.u32(h.counts.time);
    counts.u32(h.counts.type);
    counts.u32(h.counts.chars);
    return h;
}

std::size_t block_size(const Counts& c, std::size_t time_size) {
    return static_cast<std::size_t>(c.time) * (time_size + 1) +
           static_cast<std::size_t>(c.type) * 6 + c.chars +
           static_cast<std::size_t>(c.leap) * (time_size + 4) + c.is_std +
           c.is_ut;
}

dstd::Result<void> read_block(dstd::BeReader& in, const Counts& c,
                              std::size_t time_size, dstd::StringView name,
                              dstd::Vector<std::int64_t>& trans,
                              dstd::Vector<std::uint8_t>& trans_type,
                              dstd::Vector<LocalType>& types) {
    const auto bad = [&](dstd::StringView why) {
        return common::make_error<"time zone '{}': bad TZif data: {}">(
            common::ErrorCode::Parse, name, why);
    };
    if (c.type == 0 || c.chars == 0) return bad("no local time type");
    if (in.remaining() < block_size(c, time_size)) return bad("truncated");

    trans.reserve(c.time);
    for (std::uint32_t i = 0; i < c.time; ++i) {
        std::int64_t t = 0;
        if (time_size == 4) {
            std::uint32_t v = 0;
            in.u32(v);
            t = static_cast<std::int32_t>(v);
        } else {
            std::uint64_t v = 0;
            in.u64(v);
            t = static_cast<std::int64_t>(v);
        }
        if (!trans.empty() && t < trans.back())
            return bad("transitions out of order");
        trans.push_back(t);
    }
    trans_type.reserve(c.time);
    for (std::uint32_t i = 0; i < c.time; ++i) {
        std::uint8_t idx = 0;
        in.u8(idx);
        if (idx >= c.type) return bad("transition type out of range");
        trans_type.push_back(idx);
    }
    struct Raw {
        std::int32_t offset;
        bool is_dst;
        std::uint8_t abbr;
    };
    dstd::Vector<Raw> raw;
    raw.reserve(c.type);
    for (std::uint32_t i = 0; i < c.type; ++i) {
        std::uint32_t off = 0;
        std::uint8_t dst = 0, abbr = 0;
        in.u32(off);
        in.u8(dst);
        in.u8(abbr);
        const auto offset = static_cast<std::int32_t>(off);
        if (offset > MAX_UTC_OFFSET || offset < -MAX_UTC_OFFSET)
            return bad("offset out of range");
        if (dst > 1 || abbr >= c.chars) return bad("bad local time type");
        raw.push_back({offset, dst == 1, abbr});
    }
    const std::uint8_t* chars = in.take(c.chars);
    types.reserve(c.type + 2);
    for (const Raw& r : raw) {
        const std::uint8_t* from = chars + r.abbr;
        const std::size_t room = c.chars - r.abbr;
        const void* end = std::memchr(from, 0, room);
        if (end == nullptr) return bad("unterminated abbreviation");
        LocalType t;
        t.offset = r.offset;
        t.is_dst = r.is_dst;
        t.abbreviation.assign(
            reinterpret_cast<const char*>(from),
            static_cast<std::size_t>(static_cast<const std::uint8_t*>(end) -
                                     from));
        types.push_back(std::move(t));
    }
    in.take(static_cast<std::size_t>(c.leap) * (time_size + 4) + c.is_std +
            c.is_ut);
    return {};
}

}  // namespace

dstd::Result<Zone> parse_tzif(dstd::StringView name,
                              dstd::Span<const std::uint8_t> bytes) {
    dstd::BeReader in(bytes.data(), bytes.size());
    auto head = read_header(in, name);
    if (!head) return dstd::Unexpected<dstd::Error>(head.error());
    Header header = *head;
    std::size_t time_size = 4;
    if (header.version != 0) {
        if (in.take(block_size(header.counts, 4)) == nullptr)
            return common::make_error<"time zone '{}': bad TZif data: {}">(
                common::ErrorCode::Parse, name, "truncated");
        auto second = read_header(in, name);
        if (!second) return dstd::Unexpected<dstd::Error>(second.error());
        header = *second;
        time_size = 8;
    }

    Zone zone;
    zone.name_.assign(name);
    auto read = read_block(in, header.counts, time_size, name, zone.trans_,
                           zone.trans_type_, zone.types_);
    if (!read) return dstd::Unexpected<dstd::Error>(read.error());

    if (header.version != 0) {
        std::uint8_t nl = 0;
        if (!in.u8(nl))
            return common::make_error<"time zone '{}': bad TZif footer">(
                common::ErrorCode::Parse, name);
        const std::uint8_t* rest = in.position();
        const std::size_t room = in.remaining();
        const void* stop = std::memchr(rest, '\n', room);
        if (nl != '\n' || stop == nullptr)
            return common::make_error<"time zone '{}': bad TZif footer">(
                common::ErrorCode::Parse, name);
        const dstd::StringView text(
            reinterpret_cast<const char*>(rest),
            static_cast<std::size_t>(static_cast<const std::uint8_t*>(stop) -
                                     rest));
        if (!text.empty()) {
            auto parsed = parse_posix_tz(text);
            if (!parsed) return dstd::Unexpected<dstd::Error>(parsed.error());
            zone.footer_ = dstd::make_unique<Footer>(parsed->footer);
            zone.footer_->std_type =
                static_cast<std::uint32_t>(zone.types_.size());
            zone.types_.push_back(
                {parsed->footer.std_offset, false, parsed->std_name});
            if (parsed->footer.has_dst) {
                zone.footer_->dst_type =
                    static_cast<std::uint32_t>(zone.types_.size());
                zone.types_.push_back(
                    {parsed->footer.dst_offset, true, parsed->dst_name});
            }
        }
    }
    return zone;
}

dstd::Result<Zone> zone_from_posix(dstd::StringView name,
                                   dstd::StringView rule) {
    auto parsed = parse_posix_tz(rule);
    if (!parsed) return dstd::Unexpected<dstd::Error>(parsed.error());
    Zone zone;
    zone.name_.assign(name);
    zone.footer_ = dstd::make_unique<Footer>(parsed->footer);
    zone.types_.push_back({parsed->footer.std_offset, false, parsed->std_name});
    if (parsed->footer.has_dst) {
        zone.footer_->dst_type = 1;
        zone.types_.push_back(
            {parsed->footer.dst_offset, true, parsed->dst_name});
    }
    return zone;
}

Zone fixed_zone(dstd::StringView name, std::int32_t offset,
                dstd::StringView abbreviation) {
    Zone zone;
    zone.name_.assign(name);
    zone.types_.push_back({offset, false, dstd::String(abbreviation)});
    return zone;
}

}  // namespace dftracer::utils::dataframe::tz
