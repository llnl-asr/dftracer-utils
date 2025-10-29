from __future__ import annotations

from setuptools import setup

from setuptools_scm import ScmVersion


def myversion_func(version: ScmVersion) -> str:
    from setuptools_scm.version import only_version

    if version.distance > 0:
        return version.format_next_version(only_version, fmt="{tag}.dev{distance}")
    else:
        return version.format_next_version(only_version, fmt="{tag}")


setup(use_scm_version={"version_scheme": myversion_func})
