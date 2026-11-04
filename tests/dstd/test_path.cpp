// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <filesystem>

using namespace dftracer::utils;

namespace {

/// Inputs whose filename, parent, stem and extension are the same here as in
/// std::filesystem. A trailing separator and a leading "//" are not, and the
/// cases below CHECK them on their own.
const dstd::Vector<dstd::String> SAME_AS_STD = {
    "",         "/",        "/a",      "a",      "a/b", "/a/b",
    "a.tar.gz", ".bashrc",  "foo.txt", ".",      "..",  "a/.",
    "a/..",     "/..",      "../a",    "a..b",   "..a", "...",
    "a/b/c",    "/a/b/./c", "x.txt",   "a b.txt"};

}  // namespace

static_assert(dstd::path_detail::filename("/a/b.txt", dstd::PathStyle::Posix) ==
              "b.txt");
static_assert(dstd::path_detail::parent("/a/b.txt", dstd::PathStyle::Posix) ==
              "/a");
static_assert(dstd::path_detail::extension("/a/b.txt",
                                           dstd::PathStyle::Posix) == ".txt");

TEST_CASE("PathView queries match std::filesystem without a trailing slash") {
    for (const dstd::String& input : SAME_AS_STD) {
        CAPTURE(input);
        const dstd::PathView path(input);
        const dstd::Path reference(input);
        CHECK(path.filename() == reference.view().filename());
        CHECK(path.parent().str() == reference.view().parent().str());
        CHECK(path.stem() == reference.view().stem());
        CHECK(path.extension() == reference.view().extension());
        CHECK(path.is_absolute() == reference.view().is_absolute());
        CHECK(path.str() == input);
    }
}

TEST_CASE("a trailing separator is dropped before the last component") {
    CHECK(dstd::PathView("a/").filename() == "a");
    CHECK(dstd::PathView("a//").filename() == "a");
    CHECK(dstd::PathView("a/").parent().str() == "");
    CHECK(dstd::PathView("a/b/").filename() == "b");
    CHECK(dstd::PathView("a/b/").parent().str() == "a");
    CHECK(dstd::PathView("/a/b/").filename() == "b");
    CHECK(dstd::PathView("/a/b/").parent().str() == "/a");
    CHECK(dstd::PathView("/a/b/").stem() == "b");
    CHECK(dstd::PathView("/a/b/").extension() == "");
    CHECK(dstd::PathView("a.tar.gz/").extension() == ".gz");
}

TEST_CASE("the root keeps its own parent") {
    CHECK(dstd::PathView("/").root() == "/");
    CHECK(dstd::PathView("/").filename() == "");
    CHECK(dstd::PathView("/").parent().str() == "/");
    CHECK(dstd::PathView("/a").root() == "/");
    CHECK(dstd::PathView("/a").parent().str() == "/");
    CHECK(dstd::PathView("a").root() == "");
    CHECK(dstd::PathView("a").parent().str() == "");
    CHECK(dstd::PathView("").parent().str() == "");
    CHECK(dstd::PathView("").filename() == "");
    CHECK(dstd::PathView("").root() == "");
    CHECK(dstd::PathView("/").is_absolute());
    CHECK(dstd::PathView("/a/b").is_absolute());
    CHECK_FALSE(dstd::PathView("a/b").is_absolute());
    CHECK_FALSE(dstd::PathView("").is_absolute());
}

TEST_CASE("a doubled leading separator collapses to one root") {
    CHECK(dstd::PathView("//").root() == "/");
    CHECK(dstd::PathView("//").filename() == "");
    CHECK(dstd::PathView("//").parent().str() == "/");
    CHECK(dstd::PathView("//a").root() == "/");
    CHECK(dstd::PathView("//a").filename() == "a");
    CHECK(dstd::PathView("//a").parent().str() == "/");
}

TEST_CASE("a dotfile and a multi-dot name") {
    CHECK(dstd::PathView(".bashrc").extension() == "");
    CHECK(dstd::PathView(".bashrc").stem() == ".bashrc");
    CHECK(dstd::PathView("a.tar.gz").extension() == ".gz");
    CHECK(dstd::PathView("a.tar.gz").stem() == "a.tar");
    CHECK(dstd::PathView("dir/.bashrc").parent().str() == "dir");
    CHECK(dstd::PathView("a..b").extension() == ".b");
    CHECK(dstd::PathView("a..b").stem() == "a.");
}

TEST_CASE("Path joins with the host separator") {
    dstd::Path joined("/a");
    joined /= "b";
    CHECK(joined.str() == "/a/b");
    joined /= "";
    CHECK(joined.str() == "/a/b");
    joined /= "/c";
    CHECK(joined.str() == "/c");
    CHECK(joined.view().filename() == "c");
    CHECK(joined.c_str()[joined.str().size()] == '\0');

    dstd::Path from_empty;
    from_empty /= "b";
    CHECK(from_empty.str() == "b");

    dstd::Path trailing("a/");
    trailing /= "b";
    CHECK(trailing.str() == "a/b");
    CHECK((dstd::PathView("/a") / "b").str() == "/a/b");
    CHECK((dstd::PathView("/a") / "/b").str() == "/b");
    CHECK((dstd::PathView("") / "b").str() == "b");
}

TEST_CASE("Path converts to CStringView") {
    const dstd::Path path("/a/b.txt");
    const dstd::CStringView view = path;
    CHECK(view.c_str()[view.size()] == '\0');
    CHECK(view.size() == 8);
    CHECK(view.view() == "/a/b.txt");
}

TEST_CASE("replace_extension") {
    dstd::Path path("a/b.tar.gz");
    path.replace_extension(".x");
    CHECK(path.str() == "a/b.tar.x");

    dstd::Path no_extension("a/b");
    no_extension.replace_extension("x");
    CHECK(no_extension.str() == "a/b.x");

    dstd::Path dropped("a/b.tar.gz");
    dropped.replace_extension("");
    CHECK(dropped.str() == "a/b.tar");

    dstd::Path dotfile(".bashrc");
    dotfile.replace_extension(".x");
    CHECK(dotfile.str() == ".bashrc.x");

    dstd::Path empty;
    empty.replace_extension(".x");
    CHECK(empty.str() == ".x");

    dstd::Path rooted("/a/b");
    rooted.replace_extension(".x");
    CHECK(rooted.str() == "/a/b.x");

    dstd::Path trailing("a/b/");
    trailing.replace_extension(".x");
    CHECK(trailing.str() == "a/b.x");
}

TEST_CASE("normalize resolves dots and keeps what has nothing to cancel") {
    CHECK(dstd::normalize(dstd::PathView("a/./b/../c")).str() == "a/c");
    CHECK(dstd::normalize(dstd::PathView("../a")).str() == "../a");
    CHECK(dstd::normalize(dstd::PathView("/..")).str() == "/");
    CHECK(dstd::normalize(dstd::PathView("/a/../..")).str() == "/");
    CHECK(dstd::normalize(dstd::PathView("a/../../b")).str() == "../b");
    CHECK(dstd::normalize(dstd::PathView(".")).str() == ".");
    CHECK(dstd::normalize(dstd::PathView("a/..")).str() == ".");
    CHECK(dstd::normalize(dstd::PathView("/")).str() == "/");
    CHECK(dstd::normalize(dstd::PathView("//a")).str() == "/a");
    CHECK(dstd::normalize(dstd::PathView("")).str() == "");
    CHECK(dstd::normalize(dstd::PathView("a//b//c")).str() == "a/b/c");
}

TEST_CASE("relative is lexical") {
    CHECK(
        dstd::relative(dstd::PathView("/a/b/c"), dstd::PathView("/a")).str() ==
        "b/c");
    CHECK(
        dstd::relative(dstd::PathView("/a/b"), dstd::PathView("/a/b")).str() ==
        ".");
    CHECK(dstd::relative(dstd::PathView("/a"), dstd::PathView("/a/b")).str() ==
          "..");
    CHECK(dstd::relative(dstd::PathView("a/b"), dstd::PathView("c")).str() ==
          "../a/b");
    CHECK(dstd::relative(dstd::PathView("a/b"), dstd::PathView("a")).str() ==
          "b");
    CHECK(dstd::relative(dstd::PathView("/a/b"), dstd::PathView("a/b")).str() ==
          "");
}

TEST_CASE("the Windows style rules run on any platform") {
    CHECK(dstd::path_detail::root_size("C:\\a\\b.txt",
                                       dstd::PathStyle::Windows) == 3);
    CHECK(dstd::path_detail::root("C:\\a\\b.txt", dstd::PathStyle::Windows) ==
          "C:\\");
    CHECK(dstd::path_detail::root("C:", dstd::PathStyle::Windows) == "C:");
    CHECK(dstd::path_detail::parent("C:\\a\\b.txt", dstd::PathStyle::Windows) ==
          "C:\\a");
    CHECK(dstd::path_detail::filename("C:\\a\\b.txt",
                                      dstd::PathStyle::Windows) == "b.txt");
    CHECK(dstd::path_detail::stem("C:\\a\\b.txt", dstd::PathStyle::Windows) ==
          "b");
    CHECK(dstd::path_detail::extension("C:\\a\\b.txt",
                                       dstd::PathStyle::Windows) == ".txt");
    CHECK(dstd::path_detail::parent("C:/a/b.txt", dstd::PathStyle::Windows) ==
          "C:/a");
    CHECK(dstd::path_detail::filename("C:/a/b.txt", dstd::PathStyle::Windows) ==
          "b.txt");

    CHECK(dstd::path_detail::root("\\\\srv\\share\\x",
                                  dstd::PathStyle::Windows) ==
          "\\\\srv\\share");
    CHECK(dstd::path_detail::filename("\\\\srv\\share\\x",
                                      dstd::PathStyle::Windows) == "x");
    CHECK(dstd::path_detail::parent("\\\\srv\\share\\x",
                                    dstd::PathStyle::Windows) ==
          "\\\\srv\\share");

    CHECK(dstd::path_detail::is_absolute("C:\\a", dstd::PathStyle::Windows));
    CHECK(dstd::path_detail::is_absolute("C:/a", dstd::PathStyle::Windows));
    CHECK(dstd::path_detail::is_absolute("\\\\srv\\share",
                                         dstd::PathStyle::Windows));
    CHECK(dstd::path_detail::is_absolute("\\a", dstd::PathStyle::Windows));
    CHECK_FALSE(
        dstd::path_detail::is_absolute("C:a", dstd::PathStyle::Windows));
    CHECK_FALSE(dstd::path_detail::is_absolute("a", dstd::PathStyle::Windows));
    CHECK_FALSE(
        dstd::path_detail::is_absolute("C:\\a", dstd::PathStyle::Posix));

    CHECK(dstd::PathView("C:\\a\\b.txt").filename(dstd::PathStyle::Windows) ==
          "b.txt");
    CHECK(
        dstd::PathView("C:\\a\\b.txt").parent(dstd::PathStyle::Windows).str() ==
        "C:\\a");
    CHECK(dstd::PathView("\\\\srv\\share\\x").root(dstd::PathStyle::Windows) ==
          "\\\\srv\\share");

    CHECK(dstd::normalize(dstd::PathView("C:\\a\\..\\b"),
                          dstd::PathStyle::Windows)
              .str() == "C:\\b");
    CHECK(dstd::relative(dstd::PathView("C:\\a\\b"), dstd::PathView("C:\\a"),
                         dstd::PathStyle::Windows)
              .str() == "b");
    CHECK(dstd::relative(dstd::PathView("C:\\a"), dstd::PathView("D:\\a"),
                         dstd::PathStyle::Windows)
              .str() == "");
}

TEST_CASE("the host style is the one the platform uses") {
#if defined(_WIN32)
    CHECK(dstd::HOST_PATH_STYLE == dstd::PathStyle::Windows);
    CHECK(dstd::PATH_SEPARATOR == '\\');
#else
    CHECK(dstd::HOST_PATH_STYLE == dstd::PathStyle::Posix);
    CHECK(dstd::PATH_SEPARATOR == '/');
#endif
}
