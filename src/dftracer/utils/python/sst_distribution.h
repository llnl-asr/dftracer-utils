#ifndef DFTRACER_UTILS_PYTHON_SST_DISTRIBUTION_H
#define DFTRACER_UTILS_PYTHON_SST_DISTRIBUTION_H

#include <Python.h>

namespace dftracer::utils::utilities::indexer {
class SstArtifactRegistry;
}

/// Extract the owned C++ SstArtifactRegistry from a Python
/// SstArtifactRegistry instance. Returns NULL (without setting an error)
/// if `obj` is not an SstArtifactRegistry.
dftracer::utils::utilities::indexer::SstArtifactRegistry *
sst_artifact_registry_get(PyObject *obj);

int init_sst_distribution(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_SST_DISTRIBUTION_H
