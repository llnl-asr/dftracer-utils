#include <dftracer/utils/python/indexer_checkpoint.h>
#include <structmember.h>

PyObject *IndexerCheckpoint_new(PyTypeObject *type, PyObject *args,
                                PyObject *kwds) {
    IndexerCheckpointObject *self;
    self = (IndexerCheckpointObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        memset(&self->checkpoint, 0, sizeof(dft_indexer_checkpoint_t));
    }
    return (PyObject *)self;
}

static void IndexerCheckpoint_dealloc(IndexerCheckpointObject *self) {
    free(self->checkpoint.dict_compressed);
    self->checkpoint.dict_compressed = NULL;
    Py_TYPE(self)->tp_free((PyObject *)self);
}

#if PY_VERSION_HEX >= 0x030C0000  // >= 3.12

static PyMemberDef IndexerCheckpoint_members[] = {
    {"checkpoint_idx", Py_T_ULONGLONG,
     offsetof(IndexerCheckpointObject, checkpoint.checkpoint_idx), 0,
     "Checkpoint index"},
    {"uc_offset", Py_T_ULONGLONG,
     offsetof(IndexerCheckpointObject, checkpoint.uc_offset), 0,
     "Uncompressed offset"},
    {"uc_size", Py_T_ULONGLONG,
     offsetof(IndexerCheckpointObject, checkpoint.uc_size), 0,
     "Uncompressed size"},
    {"c_offset", Py_T_ULONGLONG,
     offsetof(IndexerCheckpointObject, checkpoint.c_offset), 0,
     "Compressed offset"},
    {"c_size", Py_T_ULONGLONG,
     offsetof(IndexerCheckpointObject, checkpoint.c_size), 0,
     "Compressed size"},
    {"bits", Py_T_UINT, offsetof(IndexerCheckpointObject, checkpoint.bits), 0,
     "Bit position"},
    {"num_lines", Py_T_ULONGLONG,
     offsetof(IndexerCheckpointObject, checkpoint.num_lines), 0,
     "Number of lines in this chunk"},
    {NULL} /* Sentinel */
};

#else

static PyMemberDef IndexerCheckpoint_members[] = {
    {"checkpoint_idx", T_ULONGLONG,
     offsetof(IndexerCheckpointObject, checkpoint.checkpoint_idx), 0,
     "Checkpoint index"},
    {"uc_offset", T_ULONGLONG,
     offsetof(IndexerCheckpointObject, checkpoint.uc_offset), 0,
     "Uncompressed offset"},
    {"uc_size", T_ULONGLONG,
     offsetof(IndexerCheckpointObject, checkpoint.uc_size), 0,
     "Uncompressed size"},
    {"c_offset", T_ULONGLONG,
     offsetof(IndexerCheckpointObject, checkpoint.c_offset), 0,
     "Compressed offset"},
    {"c_size", T_ULONGLONG,
     offsetof(IndexerCheckpointObject, checkpoint.c_size), 0,
     "Compressed size"},
    {"bits", T_UINT, offsetof(IndexerCheckpointObject, checkpoint.bits), 0,
     "Bit position"},
    {"num_lines", T_ULONGLONG,
     offsetof(IndexerCheckpointObject, checkpoint.num_lines), 0,
     "Number of lines in this chunk"},
    {NULL} /* Sentinel */
};

#endif

PyTypeObject IndexerCheckpointType = {
    PyVarObject_HEAD_INIT(NULL, 0) "indexer.IndexerCheckpoint", /* tp_name */
    sizeof(IndexerCheckpointObject),       /* tp_basicsize */
    0,                                     /* tp_itemsize */
    (destructor)IndexerCheckpoint_dealloc, /* tp_dealloc */
    0,                                     /* tp_vectorcall_offset */
    0,                                     /* tp_getattr */
    0,                                     /* tp_setattr */
    0,                                     /* tp_as_async */
    0,                                     /* tp_repr */
    0,                                     /* tp_as_number */
    0,                                     /* tp_as_sequence */
    0,                                     /* tp_as_mapping */
    0,                                     /* tp_hash */
    0,                                     /* tp_call */
    0,                                     /* tp_str */
    0,                                     /* tp_getattro */
    0,                                     /* tp_setattro */
    0,                                     /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                    /* tp_flags */
    "IndexerCheckpoint objects",           /* tp_doc */
    0,                                     /* tp_traverse */
    0,                                     /* tp_clear */
    0,                                     /* tp_richcompare */
    0,                                     /* tp_weaklistoffset */
    0,                                     /* tp_iter */
    0,                                     /* tp_iternext */
    0,                                     /* tp_methods */
    IndexerCheckpoint_members,             /* tp_members */
    0,                                     /* tp_getset */
    0,                                     /* tp_base */
    0,                                     /* tp_dict */
    0,                                     /* tp_descr_get */
    0,                                     /* tp_descr_set */
    0,                                     /* tp_dictoffset */
    0,                                     /* tp_init */
    0,                                     /* tp_alloc */
    IndexerCheckpoint_new,                 /* tp_new */
};

int init_indexer_checkpoint(PyObject *m) {
    if (PyType_Ready(&IndexerCheckpointType) < 0) return -1;

    Py_INCREF(&IndexerCheckpointType);
    if (PyModule_AddObject(m, "IndexerCheckpoint",
                           (PyObject *)&IndexerCheckpointType) < 0) {
        Py_DECREF(&IndexerCheckpointType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
