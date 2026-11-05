// SPDX-License-Identifier: MIT
// Calls one thing from each library a static consumer links: duql (regex
// match), dataframe (a column), trace and store and io (index a plain trace),
// utilities (the host ops registration).
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/abi.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/trace/trace_config.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" void dftu_register_host_ops(void);

using namespace dftracer::utils;

namespace {

std::uint32_t crc32(const std::string& data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void put_le(std::string& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i)
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

// One gzip member holding the text in a single stored deflate block.
std::string gzip_stored(const std::string& text) {
    std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    out.push_back('\x01');
    put_le(out, static_cast<std::uint32_t>(text.size()), 2);
    put_le(out, ~static_cast<std::uint32_t>(text.size()) & 0xFFFFu, 2);
    out += text;
    put_le(out, crc32(text), 4);
    put_le(out, static_cast<std::uint32_t>(text.size()), 4);
    return out;
}

int check_index(const std::filesystem::path& dir) {
    const std::string trace = (dir / "trace.pfw.gz").string();
    {
        const std::string text =
            R"({"id":1,"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":5,"ph":"X","args":{}})"
            "\n"
            R"({"id":2,"name":"write","cat":"POSIX","pid":1,"tid":1,"ts":9,"dur":7,"ph":"X","args":{}})"
            "\n";
        std::ofstream out(trace, std::ios::binary);
        out << gzip_stored(text);
    }
    dftu_indexer_options options;
    dftu_indexer_options_init(&options);
    const char* paths[] = {trace.c_str()};
    auto opened = dftu_indexer_open(paths, 1, &options);
    if (!DFTU_RESULT_OK(opened)) return 10;
    auto built = dftu_indexer_build(DFTU_RESULT_VALUE(opened));
    const int rc = DFTU_RESULT_OK(built) &&
                           DFTU_RESULT_VALUE(built).total == 1 &&
                           DFTU_RESULT_VALUE(built).needs_work == 0
                       ? 0
                       : 11;
    dftu_indexer_free(DFTU_RESULT_VALUE(opened));
    return rc;
}

}  // namespace

int main() {
    dftu_register_host_ops();

    auto query = duql::Query::from_string(R"(name ~ "^re.*")");
    if (!query) return 1;
    json::Document doc;
    auto hit = doc.parse(R"({"name":"read"})");
    if (!hit || !query->matches(*hit)) return 2;
    auto miss = doc.parse(R"({"name":"write"})");
    if (!miss || query->matches(*miss)) return 3;

    const std::int64_t values[] = {1, 2, 3};
    auto column =
        dataframe::Series::from_borrowed(dataframe::TypeId::Int64, values, 3);
    if (column.length() != 3) return 4;

    auto config = trace::parse_end_event(
        R"({"name":"end","cat":"dftracer","type":1,"ph":1,"pid":7,"ts":1,"args":{"num_events":3}})");
    if (!config || config->pid != 7) return 5;

    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec) /
               ("dftu_pkgconfig_static_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir, ec);
    const int rc = check_index(dir);
    std::filesystem::remove_all(dir, ec);
    return rc;
}
