// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_DUQL_FILTER_CORPUS_H
#define DFTRACER_UTILS_TESTS_DUQL_FILTER_CORPUS_H

// Events and filters that the raw-event evaluators are checked against.

inline constexpr const char* const EVENTS[] = {
    R"({"name":"read","cat":"POSIX","pid":1,"dur":2000,"args":{"size":4096}})",
    R"({"name":"write","cat":"POSIX","pid":2,"dur":10,"args":{"size":10}})",
    R"({"name":"MPI_Send","cat":"MPI","pid":3,"dur":1500})",
    R"({"name":"cudaMemcpyAsync","cat":"CUDA","dur":50,"args":{"retry":1}})",
    R"({"name":"open64","cat":"POSIX","args":{"fname":"/data/a.h5","fd":7}})",
    R"({"name":"H5Dwrite","cat":"STDIO","dur":null,"args":{"size":null}})",
    R"({"cat":"POSIX","dur":1001,"args":{"name":"from_args"}})",
    R"({"name":"lseek64","cat":"POSIX","dur":"1500","pid":2.5})",
    R"({"name":"fwrite","cat":"STDIO","dur":1000,"args":{"tags":["a","b"]}})",
    R"({"name":"","cat":"","dur":0,"args":{}})",
    R"({"args":{"cat":"POSIX","dur":5000}})",
    ("{\"name\":\"read\\u0041\",\"cat\":\"POSIX\",\"dur\":-5,\"tid\":"
     "18446744073709551615}"),
    R"({"name":"read","cat":"POSIX","dur":2000.5,"nested":{"k":{"j":1}}})",
    R"({})",
    R"([])",
    R"([{"cat":"POSIX"}])",
};

inline constexpr const char* const FILTERS[] = {
    R"(cat == "POSIX" and dur > 1000)",
    R"(cat == "POSIX" or pid == 3)",
    R"(not (dur > 1000))",
    R"(dur > 1000 and dur < 2001)",
    R"(name in ["read", "write", "MPI_Send"])",
    R"(name in [])",
    R"(pid in [1, 2, 3])",
    R"((dur ?? 0) >= 1000)",
    R"((retry ?? 5) == 1)",
    R"(exists(args.size))",
    R"(not exists(dur))",
    R"(args.size > 100)",
    R"(size > 100)",
    R"(name == "from_args")",
    R"(fname == "/data/a.h5")",
    R"(name like "MPI_%")",
    R"(name ilike "%MEMCPY%")",
    R"q(name ~ "^(read|write)")q",
    R"(name ~* "memcpy")",
    R"(name !~ "^MPI")",
    R"(lower(name) == "read")",
    R"(args.size * 2 > 1000)",
    R"(dur // 10 == 200)",
    R"(dur == 1500)",
    R"(dur == 1500.0)",
    R"(tid == 18446744073709551615)",
    R"(nested.k.j == 1)",
    R"(any(args.tags) == "b")",
    R"(cat == "POSIX" and name == "read" and dur >= 2000)",
    R"(cat == 5)",
    R"(dur == null)",
    "detailed",
};

#endif  // DFTRACER_UTILS_TESTS_DUQL_FILTER_CORPUS_H
