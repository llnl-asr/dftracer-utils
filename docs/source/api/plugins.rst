:description: Python API for loading compiled plugins into the process: load_plugin and PluginHandle.

Plugins
=======

A compiled plugin adds file formats, operations and other parts to every reader in the process.
``DFTRACER_UTILS_PLUGINS`` loads plugins for the whole process (see :doc:`../environment`); :func:`dftracer.utils.plugins.load_plugin` loads one for a block of code.

.. autofunction:: dftracer.utils.plugins.load_plugin

.. autoclass:: dftracer.utils.plugins.PluginHandle
   :members:
