#!/bin/bash
# SPDX-License-Identifier: MIT
# Sphinx docs; output in public/ is published by `distribute`. Doxygen first:
# conf.py wires breathe to docs/doxygen/xml, and without it every C++ API page
# resolves to nothing. .readthedocs.yaml does the same in its pre_build.
set -eo pipefail
cd "$CI_PROJECT_DIR"
export DEBIAN_FRONTEND=noninteractive
apt-get -o APT::Sandbox::User=root update -qq
apt-get -o APT::Sandbox::User=root install -y -qq doxygen
pip install --quiet --upgrade pip
pip install --quiet -r docs/requirements.txt
# `make html` runs doxygen and the class diagrams before sphinx; it writes to
# docs/build/html, while the artifact is published from public/.
make -C docs html
rm -rf public
cp -r docs/build/html public
