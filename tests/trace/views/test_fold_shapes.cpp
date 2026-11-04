// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <doctest/doctest.h>
#include <trace/views/fold_shapes.h>

using namespace dftracer::utils;
using namespace fold_shapes;

TEST_SUITE("FoldShapes") {
    TEST_CASE("the DOM reader gives the pinned event for each shape") {
        for (const Shape& shape : SHAPES) {
            CAPTURE(shape.name);
            StringIntern intern;
            FoldEvent ev;
            const String got =
                read_dom(shape.line, ev, intern, /*needs_args=*/true)
                    ? describe(ev, intern)
                    : String("rejected");
            CHECK(got == shape.expected);
        }
    }

    TEST_CASE("without args the DOM reader keeps the fixed fields and hashes") {
        for (const Shape& shape : SHAPES) {
            CAPTURE(shape.name);
            StringIntern intern;
            FoldEvent with_args;
            FoldEvent without_args;
            const bool read =
                read_dom(shape.line, with_args, intern, /*needs_args=*/true);
            REQUIRE(read_dom(shape.line, without_args, intern,
                             /*needs_args=*/false) == read);
            if (!read) continue;
            with_args.args.clear();
            with_args.specials.clear();
            CHECK(describe(without_args, intern) ==
                  describe(with_args, intern));
        }
    }
}
