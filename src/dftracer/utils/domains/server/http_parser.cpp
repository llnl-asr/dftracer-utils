// SPDX-License-Identifier: MIT
// HTTP/1.1 request parser.
//
// Inspired by picohttpparser by Kazuho Oku, Tokuhiro Matsuno,
// Daisuke Murase, Shigeo Mitsunari (dual Perl/MIT, used under MIT).
// https://github.com/h2o/picohttpparser
//
// Simplified C++ rewrite: only parses HTTP/1.x requests (not
// responses or chunked encoding). It copies nothing, since every string_view
// points into the caller's buffer.

#include <dftracer/utils/domains/server/http_parser.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>

namespace dftracer::utils::domains::server::parser {

namespace {

// Characters valid in an HTTP token (RFC 9110 Section 5.6.2).
// tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" /
//         "." / "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
constexpr bool is_token_char(unsigned char c) {
    // clang-format off
    constexpr bool table[256] = {
        // 0-31: control
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        // 32-47: SP ! " # $ % & ' ( ) * + , - . /
        0,1,0,1,1,1,1,1, 0,0,1,1,0,1,1,0,
        // 48-63: 0-9 : ; < = > ?
        1,1,1,1,1,1,1,1, 1,1,0,0,0,0,0,0,
        // 64-79: @ A-O
        0,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
        // 80-95: P-Z [ \ ] ^ _
        1,1,1,1,1,1,1,1, 1,1,1,0,0,0,1,1,
        // 96-111: ` a-o
        1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
        // 112-127: p-z { | } ~ DEL
        1,1,1,1,1,1,1,1, 1,1,1,0,1,0,1,0,
        // 128-255: high bytes
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    };
    // clang-format on
    return table[c];
}

constexpr bool is_printable(unsigned char c) { return c >= 0x20 && c != 0x7F; }

struct Cursor {
    const char* buf;
    const char* end;

    bool eof() const { return buf >= end; }
    char peek() const { return *buf; }
    void advance() { ++buf; }

    // Advance past a run of spaces.
    void skip_spaces() {
        while (!eof() && peek() == ' ') advance();
    }
};

// Read a token (method, header name) up to a delimiter.
// Returns false on EOF or bad character.
static bool read_token(Cursor& c, dstd::StringView& token, char delim) {
    const char* start = c.buf;
    while (!c.eof()) {
        if (c.peek() == delim) {
            token = dstd::StringView(start,
                                     static_cast<std::size_t>(c.buf - start));
            return !token.empty();
        }
        if (!is_token_char(static_cast<unsigned char>(c.peek()))) return false;
        c.advance();
    }
    return false;  // EOF before delimiter
}

// Read the request-target (path) up to a space.
static bool read_request_target(Cursor& c, dstd::StringView& target) {
    const char* start = c.buf;
    while (!c.eof()) {
        if (c.peek() == ' ') {
            target = dstd::StringView(start,
                                      static_cast<std::size_t>(c.buf - start));
            return !target.empty();
        }
        auto ch = static_cast<unsigned char>(c.peek());
        if (ch < 0x21 || ch == 0x7F) return false;
        c.advance();
    }
    return false;
}

// Read a header value up to CRLF or LF.
// Returns false on EOF, sets value.
static bool read_header_value(Cursor& c, dstd::StringView& value) {
    const char* start = c.buf;
    std::size_t len = 0;
    while (!c.eof()) {
        auto ch = static_cast<unsigned char>(c.peek());
        if (ch == '\r') {
            len = static_cast<std::size_t>(c.buf - start);
            c.advance();
            if (c.eof() || c.peek() != '\n') return false;
            c.advance();
            // Trim trailing whitespace
            while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
                --len;
            value = dstd::StringView(start, len);
            return true;
        }
        if (ch == '\n') {
            len = static_cast<std::size_t>(c.buf - start);
            c.advance();
            while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
                --len;
            value = dstd::StringView(start, len);
            return true;
        }
        // Allow printable ASCII + HT (0x09)
        if (ch != '\t' && !is_printable(ch)) return false;
        c.advance();
    }
    return false;
}

// Consume CRLF or bare LF.  Returns false if neither found.
static bool consume_eol(Cursor& c) {
    if (c.eof()) return false;
    if (c.peek() == '\r') {
        c.advance();
        if (c.eof() || c.peek() != '\n') return false;
        c.advance();
        return true;
    }
    if (c.peek() == '\n') {
        c.advance();
        return true;
    }
    return false;
}

// Check whether the buffer contains a complete request (double CRLF).
static bool has_complete_request(dstd::StringView buf) {
    const std::size_t len = buf.size();
    if (len < 4) return false;
    // Scan for \r\n\r\n or \n\n
    for (std::size_t i = 0; i + 1 < len; ++i) {
        if (buf[i] == '\n') {
            if (buf[i + 1] == '\n') return true;
            if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n')
                return true;
        }
    }
    return false;
}

}  // namespace

int parse_request(dstd::StringView buf, ParsedRequest& out) {
    // Quick completeness check to avoid partial parsing work.
    if (!has_complete_request(buf)) return -2;

    Cursor c{buf.data(), buf.data() + buf.size()};

    // Skip optional leading CRLF (some clients send it after POST body).
    if (!c.eof() && c.peek() == '\r') {
        c.advance();
        if (c.eof() || c.peek() != '\n') return -1;
        c.advance();
    } else if (!c.eof() && c.peek() == '\n') {
        c.advance();
    }

    // Method
    dstd::StringView method;
    if (!read_token(c, method, ' ')) return -1;
    c.advance();  // consume space
    c.skip_spaces();

    // Request-target (path)
    dstd::StringView path;
    if (!read_request_target(c, path)) return -1;
    c.advance();  // consume space
    c.skip_spaces();

    // HTTP version: "HTTP/1.X"
    if (c.end - c.buf < 8) return -1;
    if (c.buf[0] != 'H' || c.buf[1] != 'T' || c.buf[2] != 'T' ||
        c.buf[3] != 'P' || c.buf[4] != '/' || c.buf[5] != '1' ||
        c.buf[6] != '.')
        return -1;
    char ver_digit = c.buf[7];
    if (ver_digit < '0' || ver_digit > '9') return -1;
    int minor_ver = ver_digit - '0';
    c.buf += 8;

    if (!consume_eol(c)) return -1;

    // Headers
    out.method = method;
    out.path = path;
    out.minor_version = minor_ver;
    out.headers.clear();

    constexpr std::size_t MAX_HEADERS = 64;

    while (!c.eof()) {
        // Empty line = end of headers
        if (c.peek() == '\r' || c.peek() == '\n') {
            if (!consume_eol(c)) return -1;
            break;
        }

        if (out.headers.size() >= MAX_HEADERS) return -1;

        // Header name
        dstd::StringView name;
        if (!read_token(c, name, ':')) return -1;
        c.advance();  // consume ':'

        // Skip OWS after colon
        while (!c.eof() && (c.peek() == ' ' || c.peek() == '\t')) c.advance();

        // Header value
        dstd::StringView value;
        if (!read_header_value(c, value)) return -1;

        out.headers.push_back({name, value});
    }

    return static_cast<int>(c.buf - buf.data());
}

}  // namespace dftracer::utils::domains::server::parser
