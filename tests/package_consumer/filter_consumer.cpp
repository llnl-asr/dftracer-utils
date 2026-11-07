// SPDX-License-Identifier: MIT
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/json/value.h>

using namespace dftracer::utils;

int main() {
    auto query = duql::Query::from_string("cat == \"POSIX\" and dur > 1000");
    if (!query) return 1;
    json::Document doc;
    auto hit = doc.parse(R"({"cat":"POSIX","dur":2000})");
    if (!hit || !query->matches(*hit)) return 2;
    auto miss = doc.parse(R"({"cat":"POSIX","dur":10})");
    if (!miss || query->matches(*miss)) return 3;
    return 0;
}
