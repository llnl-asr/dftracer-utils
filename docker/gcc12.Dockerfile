# GCC 12 toolchain for validating dftracer-utils on the compiler where the
# coroutine frame-layout bug originates. Build the image once; the build tree
# lives in a bind-mounted build/build-docker-gcc12 so rebuilds are incremental.
#
#   docker build -t dftracer-gcc12 -f docker/gcc12.Dockerfile .
#   (or: scripts/docker-test.sh gcc12)
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-12 \
        g++-12 \
        cmake \
        ninja-build \
        git \
        pkg-config \
        zlib1g-dev \
        libopenmpi-dev \
        openmpi-bin \
        python3 \
        python3-dev \
        python3-venv \
        python3-pip \
        valgrind \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV CC=gcc-12 CXX=g++-12
# Build the MPI wrappers' sources with GCC 12 too.
ENV OMPI_CC=gcc-12 OMPI_CXX=g++-12
# The container runs as root; let OpenMPI's mpirun proceed.
ENV OMPI_ALLOW_RUN_AS_ROOT=1 OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1
WORKDIR /work
