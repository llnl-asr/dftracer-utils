// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/path.h>
#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::dstd::HOST_PATH_STYLE;
using dftracer::utils::dstd::Path;
using dftracer::utils::dstd::PathStyle;
using dftracer::utils::dstd::PathView;

namespace pd = dftracer::utils::dstd::path_detail;

namespace {

/// Inputs whose filename, parent, stem and extension are the same here as in
/// std::filesystem. A trailing separator and a leading "//" are not, and the
/// cases below CHECK them on their own.
const std::vector<std::string> SAME_AS_STD = {
    "",         "/",        "/a",      "a",      "a/b", "/a/b",
    "a.tar.gz", ".bashrc",  "foo.txt", ".",      "..",  "a/.",
    "a/..",     "/..",      "../a",    "a..b",   "..a", "...",
    "a/b/c",    "/a/b/./c", "x.txt",   "a b.txt"};

}  // namespace

static_assert(pd::filename("/a/b.txt", PathStyle::Posix) == "b.txt");
static_assert(pd::parent("/a/b.txt", PathStyle::Posix) == "/a");
static_assert(pd::extension("/a/b.txt", PathStyle::Posix) == ".txt");

TEST_CASE("PathView queries match std::filesystem without a trailing slash") {
    for (const std::string& input : SAME_AS_STD) {
        CAPTURE(input);
        const PathView path(input);
        const std::filesystem::path reference(input);
        CHECK(path.filename() == reference.filename().string());
        CHECK(path.parent().str() == reference.parent_path().string());
        CHECK(path.stem() == reference.stem().string());
        CHECK(path.extension() == reference.extension().string());
        CHECK(path.is_absolute() == reference.is_absolute());
        CHECK(path.str() == input);
    }
}

TEST_CASE("a trailing separator is dropped before the last component") {
    CHECK(PathView("a/").filename() == "a");
    CHECK(PathView("a//").filename() == "a");
    CHECK(PathView("a/").parent().str() == "");
    CHECK(PathView("a/b/").filename() == "b");
    CHECK(PathView("a/b/").parent().str() == "a");
    CHECK(PathView("/a/b/").filename() == "b");
    CHECK(PathView("/a/b/").parent().str() == "/a");
    CHECK(PathView("/a/b/").stem() == "b");
    CHECK(PathView("/a/b/").extension() == "");
    CHECK(PathView("a.tar.gz/").extension() == ".gz");
}

TEST_CASE("the root keeps its own parent") {
    CHECK(PathView("/").root() == "/");
    CHECK(PathView("/").filename() == "");
    CHECK(PathView("/").parent().str() == "/");
    CHECK(PathView("/a").root() == "/");
    CHECK(PathView("/a").parent().str() == "/");
    CHECK(PathView("a").root() == "");
    CHECK(PathView("a").parent().str() == "");
    CHECK(PathView("").parent().str() == "");
    CHECK(PathView("").filename() == "");
    CHECK(PathView("").root() == "");
    CHECK(PathView("/").is_absolute());
    CHECK(PathView("/a/b").is_absolute());
    CHECK_FALSE(PathView("a/b").is_absolute());
    CHECK_FALSE(PathView("").is_absolute());
}

TEST_CASE("a doubled leading separator collapses to one root") {
    CHECK(PathView("//").root() == "/");
    CHECK(PathView("//").filename() == "");
    CHECK(PathView("//").parent().str() == "/");
    CHECK(PathView("//a").root() == "/");
    CHECK(PathView("//a").filename() == "a");
    CHECK(PathView("//a").parent().str() == "/");
}

TEST_CASE("a dotfile and a multi-dot name") {
    CHECK(PathView(".bashrc").extension() == "");
    CHECK(PathView(".bashrc").stem() == ".bashrc");
    CHECK(PathView("a.tar.gz").extension() == ".gz");
    CHECK(PathView("a.tar.gz").stem() == "a.tar");
    CHECK(PathView("dir/.bashrc").parent().str() == "dir");
    CHECK(PathView("a..b").extension() == ".b");
    CHECK(PathView("a..b").stem() == "a.");
}

TEST_CASE("Path joins with the host separator") {
    Path joined("/a");
    joined /= "b";
    CHECK(joined.str() == "/a/b");
    joined /= "";
    CHECK(joined.str() == "/a/b");
    joined /= "/c";
    CHECK(joined.str() == "/c");
    CHECK(joined.view().filename() == "c");
    CHECK(joined.c_str()[joined.str().size()] == '\0');

    Path from_empty;
    from_empty /= "b";
    CHECK(from_empty.str() == "b");

    Path trailing("a/");
    trailing /= "b";
    CHECK(trailing.str() == "a/b");
    CHECK((PathView("/a") / "b").str() == "/a/b");
    CHECK((PathView("/a") / "/b").str() == "/b");
    CHECK((PathView("") / "b").str() == "b");
}

TEST_CASE("Path converts to CStringView") {
    const Path path("/a/b.txt");
    const dstd::CStringView view = path;
    CHECK(view.c_str()[view.size()] == '\0');
    CHECK(view.size() == 8);
    CHECK(view.view() == "/a/b.txt");
}

TEST_CASE("replace_extension") {
    Path path("a/b.tar.gz");
    path.replace_extension(".x");
    CHECK(path.str() == "a/b.tar.x");

    Path no_extension("a/b");
    no_extension.replace_extension("x");
    CHECK(no_extension.str() == "a/b.x");

    Path dropped("a/b.tar.gz");
    dropped.replace_extension("");
    CHECK(dropped.str() == "a/b.tar");

    Path dotfile(".bashrc");
    dotfile.replace_extension(".x");
    CHECK(dotfile.str() == ".bashrc.x");

    Path empty;
    empty.replace_extension(".x");
    CHECK(empty.str() == ".x");

    Path rooted("/a/b");
    rooted.replace_extension(".x");
    CHECK(rooted.str() == "/a/b.x");

    Path trailing("a/b/");
    trailing.replace_extension(".x");
    CHECK(trailing.str() == "a/b.x");
}

TEST_CASE("normalize resolves dots and keeps what has nothing to cancel") {
    CHECK(dftracer::utils::dstd::normalize(PathView("a/./b/../c")).str() ==
          "a/c");
    CHECK(dftracer::utils::dstd::normalize(PathView("../a")).str() == "../a");
    CHECK(dftracer::utils::dstd::normalize(PathView("/..")).str() == "/");
    CHECK(dftracer::utils::dstd::normalize(PathView("/a/../..")).str() == "/");
    CHECK(dftracer::utils::dstd::normalize(PathView("a/../../b")).str() ==
          "../b");
    CHECK(dftracer::utils::dstd::normalize(PathView(".")).str() == ".");
    CHECK(dftracer::utils::dstd::normalize(PathView("a/..")).str() == ".");
    CHECK(dftracer::utils::dstd::normalize(PathView("/")).str() == "/");
    CHECK(dftracer::utils::dstd::normalize(PathView("//a")).str() == "/a");
    CHECK(dftracer::utils::dstd::normalize(PathView("")).str() == "");
    CHECK(dftracer::utils::dstd::normalize(PathView("a//b//c")).str() ==
          "a/b/c");
}

TEST_CASE("relative is lexical") {
    using dftracer::utils::dstd::relative;
    CHECK(relative(PathView("/a/b/c"), PathView("/a")).str() == "b/c");
    CHECK(relative(PathView("/a/b"), PathView("/a/b")).str() == ".");
    CHECK(relative(PathView("/a"), PathView("/a/b")).str() == "..");
    CHECK(relative(PathView("a/b"), PathView("c")).str() == "../a/b");
    CHECK(relative(PathView("a/b"), PathView("a")).str() == "b");
    CHECK(relative(PathView("/a/b"), PathView("a/b")).str() == "");
}

TEST_CASE("the Windows style rules run on any platform") {
    CHECK(pd::root_length("C:\\a\\b.txt", PathStyle::Windows) == 3);
    CHECK(pd::root("C:\\a\\b.txt", PathStyle::Windows) == "C:\\");
    CHECK(pd::root("C:", PathStyle::Windows) == "C:");
    CHECK(pd::parent("C:\\a\\b.txt", PathStyle::Windows) == "C:\\a");
    CHECK(pd::filename("C:\\a\\b.txt", PathStyle::Windows) == "b.txt");
    CHECK(pd::stem("C:\\a\\b.txt", PathStyle::Windows) == "b");
    CHECK(pd::extension("C:\\a\\b.txt", PathStyle::Windows) == ".txt");
    CHECK(pd::parent("C:/a/b.txt", PathStyle::Windows) == "C:/a");
    CHECK(pd::filename("C:/a/b.txt", PathStyle::Windows) == "b.txt");

    CHECK(pd::root("\\\\srv\\share\\x", PathStyle::Windows) ==
          "\\\\srv\\share");
    CHECK(pd::filename("\\\\srv\\share\\x", PathStyle::Windows) == "x");
    CHECK(pd::parent("\\\\srv\\share\\x", PathStyle::Windows) ==
          "\\\\srv\\share");

    CHECK(pd::is_absolute("C:\\a", PathStyle::Windows));
    CHECK(pd::is_absolute("C:/a", PathStyle::Windows));
    CHECK(pd::is_absolute("\\\\srv\\share", PathStyle::Windows));
    CHECK(pd::is_absolute("\\a", PathStyle::Windows));
    CHECK_FALSE(pd::is_absolute("C:a", PathStyle::Windows));
    CHECK_FALSE(pd::is_absolute("a", PathStyle::Windows));
    CHECK_FALSE(pd::is_absolute("C:\\a", PathStyle::Posix));

    CHECK(PathView("C:\\a\\b.txt").filename(PathStyle::Windows) == "b.txt");
    CHECK(PathView("C:\\a\\b.txt").parent(PathStyle::Windows).str() == "C:\\a");
    CHECK(PathView("\\\\srv\\share\\x").root(PathStyle::Windows) ==
          "\\\\srv\\share");

    CHECK(dftracer::utils::dstd::normalize(PathView("C:\\a\\..\\b"),
                                           PathStyle::Windows)
              .str() == "C:\\b");
    CHECK(dftracer::utils::dstd::relative(PathView("C:\\a\\b"),
                                          PathView("C:\\a"), PathStyle::Windows)
              .str() == "b");
    CHECK(dftracer::utils::dstd::relative(PathView("C:\\a"), PathView("D:\\a"),
                                          PathStyle::Windows)
              .str() == "");
}

TEST_CASE("the host style is the one the platform uses") {
#if defined(_WIN32)
    CHECK(HOST_PATH_STYLE == PathStyle::Windows);
    CHECK(dstd::PATH_SEPARATOR == '\\');
#else
    CHECK(HOST_PATH_STYLE == PathStyle::Posix);
    CHECK(dstd::PATH_SEPARATOR == '/');
#endif
}
