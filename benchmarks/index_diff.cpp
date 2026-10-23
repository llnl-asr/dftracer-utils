// Compares two indexes of the same traces family by family, key by key and
// byte by byte. Usage: index_diff <trace-file> <index-dir-a> <index-dir-b>
// Exits 0 when every family matches, 1 when one differs and 2 on a usage
// error or a scan that failed.
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <cstdio>
#include <memory>
#include <string>

using namespace dftracer::utils;
namespace store = dftracer::utils::index::store;

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: index_diff <trace-file> <index-dir-a> "
                     "<index-dir-b>\n");
        return 2;
    }
    const std::string path_a =
        trace::internal::determine_index_path(argv[1], argv[2]);
    const std::string path_b =
        trace::internal::determine_index_path(argv[1], argv[3]);
    store::RocksDatabase a(path_a, store::RocksDatabase::OpenMode::ReadOnly);
    store::RocksDatabase b(path_b, store::RocksDatabase::OpenMode::ReadOnly);
    if (!a.is_open() || !b.is_open()) {
        std::fprintf(stderr, "cannot open %s\n",
                     a.is_open() ? path_b.c_str() : path_a.c_str());
        return 2;
    }
    int rc = 0;
    for (const auto cf : store::RocksDatabase::default_column_families()) {
        auto ia = a.new_iterator(cf);
        auto ib = b.new_iterator(cf);
        std::size_t rows_a = 0;
        std::size_t rows_b = 0;
        std::size_t differing = 0;
        ia->SeekToFirst();
        ib->SeekToFirst();
        while (ia->Valid() && ib->Valid()) {
            ++rows_a;
            ++rows_b;
            if (ia->key() != ib->key() || ia->value() != ib->value())
                ++differing;
            ia->Next();
            ib->Next();
        }
        for (; ia->Valid(); ia->Next()) ++rows_a;
        for (; ib->Valid(); ib->Next()) ++rows_b;
        if (!ia->status().ok() || !ib->status().ok()) {
            std::fprintf(stderr, "%s: scan failed: %s%s\n",
                         std::string(cf).c_str(),
                         ia->status().ToString().c_str(),
                         ib->status().ToString().c_str());
            return 2;
        }
        const bool same = rows_a == rows_b && differing == 0;
        std::printf("%-24s a=%zu b=%zu differing=%zu %s\n",
                    std::string(cf).c_str(), rows_a, rows_b, differing,
                    same ? "same" : "DIFFERENT");
        if (!same) rc = 1;
    }
    std::printf("%s\n", rc == 0 ? "indexes equal" : "indexes differ");
    return rc;
}
