#ifndef DFTRACER_UTILS_PYTHON_INDEX_DATABASE_H
#define DFTRACER_UTILS_PYTHON_INDEX_DATABASE_H

#include <Python.h>

#include <memory>

namespace dftracer::utils::utilities::indexer {
class IndexDatabase;
class SstArtifactRegistry;
}  // namespace dftracer::utils::utilities::indexer

typedef struct {
    PyObject_HEAD
        std::shared_ptr<dftracer::utils::utilities::indexer::IndexDatabase>
            db;
} IndexDatabaseObject;

extern PyTypeObject IndexDatabaseType;

int init_index_database(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_INDEX_DATABASE_H
