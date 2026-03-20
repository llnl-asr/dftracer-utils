Indexer Module
==============

The indexer module provides functionality for indexing and searching gzip trace files.

Indexer Class
-------------

.. autoclass:: dftracer.utils.Indexer(gz_path: str, idx_path: str | None = None, checkpoint_size: int = 1048576, force_rebuild: bool = False, build_bloom: bool = False, build_manifest: bool = False, index_threshold: int = 8388608, runtime: Runtime | None = None)
   :members:
   :undoc-members:
   :show-inheritance:
   :special-members: __enter__, __exit__

IndexerCheckpoint Class
-----------------------

.. autoclass:: dftracer.utils.IndexerCheckpoint
   :members:
   :undoc-members:
   :show-inheritance:
