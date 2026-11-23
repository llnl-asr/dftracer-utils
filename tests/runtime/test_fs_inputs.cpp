// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/fs/inputs.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <sys/stat.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <initializer_list>

using namespace dftracer::utils;
using namespace dftracer::utils::runtime::fs;

namespace {

using Names = dstd::Vector<dstd::String>;

ExpandedInputs expand(const Names& inputs) {
    Runtime rt;
    ExpandedInputs out;
    dftu_utils_test::ok(rt.block_on(coro::named(
        "inputs test", coro::with_scope([&](coro::Scope& scope) {
            return [](coro::Scope& s, const Names& in,
                      ExpandedInputs& result) -> coro::CoroTask<void> {
                auto expanding = expand_inputs(
                    s, dstd::Span<const dstd::String>(in.data(), in.size()));
                auto done = co_await std::move(expanding);
                REQUIRE(done.has_value());
                result = std::move(*done);
            }(scope, inputs, out);
        }))));
    return out;
}

void write_file_at(const dstd::Path& path) {
    REQUIRE(dstd::fs::create_directories(path.view().parent()).has_value());
    REQUIRE(dstd::fs::write_file(path.view(), "x").has_value());
}

// root/a.pfw.gz b.txt x1.pfw.gz x[1].pfw.gz sub/c.pfw.gz sub/deep/d.pfw.gz
// other/g.pfw.gz .hid/h.pfw.gz split/e.pfw.gz .dftindex/f.pfw.gz
// with links: link -> sub, alias.pfw.gz -> a.pfw.gz, hard.pfw.gz = a.pfw.gz
struct Tree {
    dftu_utils_test::ScopedTestDir dir;
    dstd::String root;

    explicit Tree(bool links = false) : dir("dftu_test_fs_inputs") {
        const dstd::Path base = dir.path() / "tree";
        for (const char* name :
             {"a.pfw.gz", "b.txt", "x1.pfw.gz", "x[1].pfw.gz", "sub/c.pfw.gz",
              "sub/deep/d.pfw.gz", "other/g.pfw.gz", ".hid/h.pfw.gz",
              "split/e.pfw.gz", ".dftindex/f.pfw.gz", ".out.tmp-1/s.pfw.gz",
              ".out.old-2/o.pfw.gz", ".z.pfw.gz.tmp-3"})
            write_file_at(base / name);
        if (links) {
            const dstd::String from((base / "a.pfw.gz").str());
            const dstd::String to((base / "hard.pfw.gz").str());
            REQUIRE(::link(from.c_str(), to.c_str()) == 0);
            REQUIRE(dstd::fs::create_symlink(dstd::PathView("sub"),
                                             (base / "link").view())
                        .has_value());
            REQUIRE(dstd::fs::create_symlink(dstd::PathView("a.pfw.gz"),
                                             (base / "alias.pfw.gz").view())
                        .has_value());
        }
        root = dstd::String(base.str());
    }

    dstd::String at(dstd::StringView rel) const {
        return dstd::String(root) + "/" + dstd::String(rel);
    }

    Names rel(const ExpandedInputs& result) const {
        Names out;
        for (const dstd::String& f : result.files) {
            dstd::StringView v(f);
            if (v.size() > root.size() && v.starts_with(dstd::StringView(root)))
                v.remove_prefix(root.size() + 1);
            out.push_back(dstd::String(v));
        }
        return out;
    }

    Names expand_rel(std::initializer_list<const char*> inputs) const {
        Names in;
        for (const char* i : inputs) in.push_back(at(i));
        return rel(expand(in));
    }
};

Names names(std::initializer_list<const char*> list) {
    Names out;
    for (const char* n : list) out.push_back(dstd::String(n));
    return out;
}

}  // namespace

TEST_CASE("a directory is scanned recursively, sorted, trace files only") {
    Tree t;
    Names in;
    in.push_back(t.root);
    CHECK(t.rel(expand(in)) ==
          names({".hid/h.pfw.gz", "a.pfw.gz", "other/g.pfw.gz", "sub/c.pfw.gz",
                 "sub/deep/d.pfw.gz", "x1.pfw.gz", "x[1].pfw.gz"}));
}

TEST_CASE("files keep the order of the inputs and any name is taken") {
    Tree t;
    CHECK(t.expand_rel({"sub/c.pfw.gz", "b.txt", "a.pfw.gz"}) ==
          names({"sub/c.pfw.gz", "b.txt", "a.pfw.gz"}));
}

TEST_CASE("a missing path with no wildcard stays and is reported") {
    Tree t;
    Names in;
    in.push_back(t.at("nope.pfw.gz"));
    in.push_back(t.at("a.pfw.gz"));
    in.push_back(t.at("nope.pfw.gz"));
    const auto out = expand(in);
    CHECK(t.rel(out) == names({"nope.pfw.gz", "a.pfw.gz"}));
    REQUIRE(out.missing.size() == 1);
    CHECK(out.missing[0] == t.at("nope.pfw.gz"));
}

TEST_CASE("a list of directories and a mixed list") {
    Tree t;
    CHECK(t.expand_rel({"other", "sub"}) ==
          names({"other/g.pfw.gz", "sub/c.pfw.gz", "sub/deep/d.pfw.gz"}));
    CHECK(t.expand_rel({"b.txt", "other", "sub/c.pfw.gz", "sub"}) ==
          names({"b.txt", "other/g.pfw.gz", "sub/c.pfw.gz",
                 "sub/deep/d.pfw.gz"}));
}

TEST_CASE("overlapping inputs and symlinks name a file once") {
    Tree t(true);
    CHECK(t.expand_rel({"sub", "sub/deep", "sub/c.pfw.gz", "sub"}) ==
          names({"sub/c.pfw.gz", "sub/deep/d.pfw.gz"}));
    // link is sub through a symlink: the same real files.
    CHECK(t.expand_rel({"sub", "link"}) ==
          names({"sub/c.pfw.gz", "sub/deep/d.pfw.gz"}));
    CHECK(t.expand_rel({"link", "sub/c.pfw.gz"}) ==
          names({"link/c.pfw.gz", "link/deep/d.pfw.gz"}));
    // A symlink to a file the user names is that file.
    CHECK(t.expand_rel({"a.pfw.gz", "alias.pfw.gz"}) == names({"a.pfw.gz"}));
    CHECK(t.expand_rel({"alias.pfw.gz", "a.pfw.gz"}) ==
          names({"alias.pfw.gz"}));
}

TEST_CASE("glob with * ? and classes") {
    Tree t;
    CHECK(t.expand_rel({"*.pfw.gz"}) ==
          names({"a.pfw.gz", "x1.pfw.gz", "x[1].pfw.gz"}));
    CHECK(t.expand_rel({"[ab]*"}) == names({"a.pfw.gz", "b.txt"}));
    CHECK(t.expand_rel({"[!a]*.txt"}) == names({"b.txt"}));
    CHECK(t.expand_rel({"x?.pfw.gz"}) == names({"x1.pfw.gz"}));
    CHECK(t.expand_rel({"?ub/c.pfw.gz"}) == names({"sub/c.pfw.gz"}));
    CHECK(t.expand_rel({"*/*.pfw.gz"}) ==
          names({".hid/h.pfw.gz", "other/g.pfw.gz", "split/e.pfw.gz",
                 "sub/c.pfw.gz"}));
}

TEST_CASE("a glob that matches a directory scans it") {
    Tree t;
    CHECK(t.expand_rel({"s*"}) ==
          names({"split/e.pfw.gz", "sub/c.pfw.gz", "sub/deep/d.pfw.gz"}));
}

TEST_CASE("a single input lists a link as often as it names it") {
    Tree t(true);
    Names in;
    in.push_back(t.root);
    const Names once = t.rel(expand(in));
    CHECK(std::count(once.begin(), once.end(), dstd::String("a.pfw.gz")) == 1);
    CHECK(std::count(once.begin(), once.end(), dstd::String("hard.pfw.gz")) ==
          1);
    CHECK(std::count(once.begin(), once.end(), dstd::String("alias.pfw.gz")) ==
          1);
}

TEST_CASE("several ** in one pattern") {
    Tree t;
    CHECK(t.expand_rel({"**/sub/**/*.pfw.gz"}) ==
          names({"sub/c.pfw.gz", "sub/deep/d.pfw.gz"}));
    CHECK(t.expand_rel({"**/**/d.pfw.gz"}) == names({"sub/deep/d.pfw.gz"}));
    CHECK(t.expand_rel({"**/sub/**/deep/**/d.pfw.gz"}) ==
          names({"sub/deep/d.pfw.gz"}));
    CHECK(t.expand_rel({"**/**"}) == t.expand_rel({"**"}));
    CHECK(t.expand_rel({"**/nothing/**/*.gz"}).empty());
}

namespace {

// Removes the permissions of a folder and gives them back on scope exit.
struct Locked {
    dstd::String path;
    explicit Locked(dstd::String p) : path(std::move(p)) {
        REQUIRE(::chmod(path.c_str(), 0) == 0);
    }
    ~Locked() { ::chmod(path.c_str(), 0755); }
};

}  // namespace

TEST_CASE("an unreadable folder is left out by a scan and by every glob form") {
    if (::geteuid() == 0) return;
    Tree t;
    write_file_at(dstd::Path(t.at("locked/z.pfw.gz")));
    Locked locked(t.at("locked"));
    const Names expected = t.expand_rel({"sub/**"});
    CHECK(!expected.empty());
    for (const char* spec : {"", "**/*.gz", "*/*.gz", "**", "lock*/*.gz",
                             "locked/**", "locked/*.gz"}) {
        Names in;
        in.push_back(*spec ? t.at(spec) : t.root);
        for (const dstd::String& f : t.rel(expand(in))) {
            INFO(spec);
            CHECK(f.find("locked/") == dstd::String::npos);
        }
    }
}

TEST_CASE("glob with ** at the start, middle and end") {
    Tree t;
    CHECK(t.expand_rel({"**/*.pfw.gz"}) ==
          names({".hid/h.pfw.gz", "a.pfw.gz", "other/g.pfw.gz", "sub/c.pfw.gz",
                 "sub/deep/d.pfw.gz", "x1.pfw.gz", "x[1].pfw.gz"}));
    CHECK(t.expand_rel({"**/d.pfw.gz"}) == names({"sub/deep/d.pfw.gz"}));
    CHECK(t.expand_rel({"sub/**/*.pfw.gz"}) ==
          names({"sub/c.pfw.gz", "sub/deep/d.pfw.gz"}));
    CHECK(t.expand_rel({"sub/**/d.pfw.gz"}) == names({"sub/deep/d.pfw.gz"}));
    // At the end it is every file, whatever its name, below the directory.
    CHECK(t.expand_rel({"**"}) ==
          names({".hid/h.pfw.gz", "a.pfw.gz", "b.txt", "other/g.pfw.gz",
                 "sub/c.pfw.gz", "sub/deep/d.pfw.gz", "x1.pfw.gz",
                 "x[1].pfw.gz"}));
    CHECK(t.expand_rel({"sub/**"}) ==
          names({"sub/c.pfw.gz", "sub/deep/d.pfw.gz"}));
}

TEST_CASE("** does not enter a skipped folder or a symlinked folder") {
    Tree t;
    for (const dstd::String& f : t.rel(expand(Names{t.at("**/*.pfw.gz")}))) {
        CHECK(f.find("split/") == dstd::String::npos);
        CHECK(f.find(".dftindex") == dstd::String::npos);
        CHECK(f.find("link/") == dstd::String::npos);
    }
}

TEST_CASE("glob results are mixed with other inputs in order, once") {
    Tree t;
    CHECK(t.expand_rel({"x1.pfw.gz", "*.pfw.gz", "sub"}) ==
          names({"x1.pfw.gz", "a.pfw.gz", "x[1].pfw.gz", "sub/c.pfw.gz",
                 "sub/deep/d.pfw.gz"}));
}

TEST_CASE("an existing file whose name has a wildcard is taken literally") {
    Tree t;
    CHECK(t.expand_rel({"x[1].pfw.gz"}) == names({"x[1].pfw.gz"}));
    CHECK(t.expand_rel({"x[1]*"}) == names({"x1.pfw.gz"}));
}

TEST_CASE("a glob that matches nothing adds nothing and is not missing") {
    Tree t;
    Names in;
    in.push_back(t.at("*.nothing"));
    in.push_back(t.at("nodir/**/*.gz"));
    in.push_back(t.at("b.txt/*"));
    const auto out = expand(in);
    CHECK(out.files.empty());
    CHECK(out.missing.empty());
}

TEST_CASE("a relative glob keeps relative paths") {
    Tree t;
    char cwd[4096];
    REQUIRE(::getcwd(cwd, sizeof cwd) != nullptr);
    REQUIRE(::chdir(t.root.c_str()) == 0);
    Names in;
    in.push_back(dstd::String("sub/*.pfw.gz"));
    in.push_back(dstd::String("**/d.pfw.gz"));
    const auto out = expand(in);
    REQUIRE(::chdir(cwd) == 0);
    CHECK(out.files == names({"sub/c.pfw.gz", "sub/deep/d.pfw.gz"}));
}

TEST_CASE("an empty directory and no inputs are no files") {
    Tree t;
    REQUIRE(dstd::fs::create_directories(dstd::PathView(t.at("empty")))
                .has_value());
    CHECK(t.expand_rel({"empty"}).empty());
    CHECK(expand(Names{}).files.empty());
}

TEST_CASE("a hard link and the file it links name one file") {
    Tree t(true);
    CHECK(t.expand_rel({"hard.pfw.gz", "a.pfw.gz"}) == names({"hard.pfw.gz"}));
    CHECK(t.expand_rel({"a.pfw.gz", "hard.pfw.gz", "alias.pfw.gz"}) ==
          names({"a.pfw.gz"}));
    // Scanned beside another input: a.pfw.gz sorts first, alias and hard
    // are the same inode.
    Names in;
    in.push_back(t.root);
    in.push_back(t.at("b.txt"));
    for (const dstd::String& f : t.rel(expand(in))) {
        CHECK(f != "hard.pfw.gz");
        CHECK(f != "alias.pfw.gz");
    }
}

TEST_CASE("staging names are skipped by scans and globs, not by name") {
    Tree t;
    Names in;
    in.push_back(t.root);
    for (const dstd::String& f : t.rel(expand(in))) {
        CHECK(f.find(".tmp-") == dstd::String::npos);
        CHECK(f.find(".old-") == dstd::String::npos);
    }
    for (const char* pattern : {"*/*.pfw.gz", "**/*.pfw.gz", "**", ".*"})
        for (const dstd::String& f : t.expand_rel({pattern})) {
            CHECK(f.find(".tmp-") == dstd::String::npos);
            CHECK(f.find(".old-") == dstd::String::npos);
        }
    // A user who names a staging file gets it.
    CHECK(t.expand_rel({".z.pfw.gz.tmp-3"}) == names({".z.pfw.gz.tmp-3"}));
}
