#ifndef DFTRACER_UTILS_PYTHON_INDEX_DATABASE_H
#define DFTRACER_UTILS_PYTHON_INDEX_DATABASE_H

#include <Python.h>

#include <memory>

namespace dftracer::utils::index::store {
class IndexDatabase;
class SstArtifactRegistry;
}  // namespace dftracer::utils::index::store

typedef struct {
    PyObject_HEAD std::shared_ptr<dftracer::utils::index::store::IndexDatabase>
        db;
} IndexDatabaseObject;

extern PyTypeObject IndexDatabaseType;

namespace dftracer::utils::python {

int init_index_database(PyObject *m);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_INDEX_DATABASE_H
