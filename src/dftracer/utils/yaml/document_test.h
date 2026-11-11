// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_DOCUMENT_TEST_H
#define DFTRACER_UTILS_YAML_DOCUMENT_TEST_H

#include <dftracer/utils/yaml/document.h>

namespace dftracer::utils::yaml {

namespace detail {

/// Makes every parse use the tape reader, as a test of the line index needs.
void set_force_tape_for_test(bool force);
bool is_force_tape();

}  // namespace detail

struct DocumentTestAccess {
    /// True when the document was read by the line index.
    static bool is_line_backed(const Document& doc);
};

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_DOCUMENT_TEST_H
