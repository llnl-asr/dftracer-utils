// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_FORMATS_VALUE_H
#define DFTRACER_UTILS_PYTHON_FORMATS_VALUE_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/formats/format.h>

namespace dftracer::utils::python::formats {

/// The source text of a record or of a whole document, which lies in what the
/// owner holds. `style` says how a YAML record is laid out in its file: 1 for
/// an element of a sequence, 2 for a document of a stream, 0 for neither.
struct RawSpan {
    const char *data = nullptr;
    std::size_t size = 0;
    int style = 0;
};

/// A Value of format F: a view of a parsed value that keeps what owns the
/// parsed text alive (`owner`, a document or a record batch). `elements` is the
/// table of the elements of an array, made on the first index.
template <class F>
struct ValueObject {
    PyObject_HEAD PyObject *owner;
    RawSpan raw;
    dstd::UniquePtr<dstd::Vector<typename F::Val>> elements;
    typename F::Val val;
};

/// The owner of a whole parsed document, behind a Value from `read` or `parse`.
template <class F>
struct DocumentObject {
    PyObject_HEAD dstd::UniquePtr<typename F::Doc> doc;
    dstd::String source;
};

template <class F>
PyTypeObject *value_type();

template <class F>
PyTypeObject *document_type();

/// A new Value of `val`, which `owner` keeps alive, with the source text `raw`
/// when it is the root of a record or a document; null with the Python error
/// set.
template <class F>
PyObject *make_value(PyObject *owner, const typename F::Val &val,
                     RawSpan raw = {});

/// A new owner of `doc` and of the `source` text it was parsed from, both
/// moved in.
template <class F>
PyObject *make_document(typename F::Doc &&doc, dstd::String &&source);

/// The source text of a Value of format F that is the root of a record or a
/// document, or null.
template <class F>
const RawSpan *raw_of(PyObject *o);

/// The parsed value behind a Value of format F, or null when `o` is not one.
template <class F>
const typename F::Val *value_of(PyObject *o);

/// A new Python object of the plain data under `v`: dict, list, str, int,
/// float, bool or None. Null with the Python error set.
template <class F>
PyObject *to_python(const typename F::Val &v, int depth);

/// The text a Value of `v` writes as, a new str.
template <class F>
PyObject *raw_text(const typename F::Val &v);

template <class F>
int init_value(PyObject *m);

}  // namespace dftracer::utils::python::formats

#endif  // DFTRACER_UTILS_PYTHON_FORMATS_VALUE_H
