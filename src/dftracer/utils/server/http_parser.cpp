// HTTP/1.1 request parser.
//
// Inspired by picohttpparser by Kazuho Oku, Tokuhiro Matsuno,
// Daisuke Murase, Shigeo Mitsunari (MIT license).
// https://github.com/h2o/picohttpparser
//
// Simplified C++ rewrite: only parses HTTP/1.x requests (not
// responses or chunked encoding). Zero-copy — all string_views
// point into the caller's buffer.

#include <dftracer/utils/server/http_parser.h>

#include <cctype>
#include <cstddef>

namespace dftracer::utils::server::parser {

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
static bool read_token(Cursor& c, const char*& start, std::size_t& len,
                       char delim) {
    start = c.buf;
    while (!c.eof()) {
        if (c.peek() == delim) {
            len = static_cast<std::size_t>(c.buf - start);
            return len > 0;
        }
        if (!is_token_char(static_cast<unsigned char>(c.peek()))) return false;
        c.advance();
    }
    return false;  // EOF before delimiter
}

// Read the request-target (path) up to a space.
static bool read_request_target(Cursor& c, const char*& start,
                                std::size_t& len) {
    start = c.buf;
    while (!c.eof()) {
        if (c.peek() == ' ') {
            len = static_cast<std::size_t>(c.buf - start);
            return len > 0;
        }
        auto ch = static_cast<unsigned char>(c.peek());
        if (ch < 0x21 || ch == 0x7F) return false;
        c.advance();
    }
    return false;
}

// Read a header value up to CRLF or LF.
// Returns false on EOF, sets start/len.
static bool read_header_value(Cursor& c, const char*& start, std::size_t& len) {
    start = c.buf;
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
            return true;
        }
        if (ch == '\n') {
            len = static_cast<std::size_t>(c.buf - start);
            c.advance();
            while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
                --len;
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
static bool has_complete_request(const char* buf, std::size_t len) {
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

int parse_request(const char* buf, std::size_t len, ParsedRequest& out) {
    // Quick completeness check to avoid partial parsing work.
    if (!has_complete_request(buf, len)) return -2;

    Cursor c{buf, buf + len};

    // Skip optional leading CRLF (some clients send it after POST body).
    if (!c.eof() && c.peek() == '\r') {
        c.advance();
        if (c.eof() || c.peek() != '\n') return -1;
        c.advance();
    } else if (!c.eof() && c.peek() == '\n') {
        c.advance();
    }

    // Method
    const char* method_start = nullptr;
    std::size_t method_len = 0;
    if (!read_token(c, method_start, method_len, ' ')) return -1;
    c.advance();  // consume space
    c.skip_spaces();

    // Request-target (path)
    const char* path_start = nullptr;
    std::size_t path_len = 0;
    if (!read_request_target(c, path_start, path_len)) return -1;
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
    out.method = std::string_view(method_start, method_len);
    out.path = std::string_view(path_start, path_len);
    out.minor_version = minor_ver;
    out.headers.clear();

    constexpr std::size_t kMaxHeaders = 64;

    while (!c.eof()) {
        // Empty line = end of headers
        if (c.peek() == '\r' || c.peek() == '\n') {
            if (!consume_eol(c)) return -1;
            break;
        }

        if (out.headers.size() >= kMaxHeaders) return -1;

        // Header name
        const char* name_start = nullptr;
        std::size_t name_len = 0;
        if (!read_token(c, name_start, name_len, ':')) return -1;
        c.advance();  // consume ':'

        // Skip OWS after colon
        while (!c.eof() && (c.peek() == ' ' || c.peek() == '\t')) c.advance();

        // Header value
        const char* val_start = nullptr;
        std::size_t val_len = 0;
        if (!read_header_value(c, val_start, val_len)) return -1;

        out.headers.push_back({std::string_view(name_start, name_len),
                               std::string_view(val_start, val_len)});
    }

    return static_cast<int>(c.buf - buf);
}

}  // namespace dftracer::utils::server::parser
