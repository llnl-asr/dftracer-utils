# Newest-Linux toolchain (Ubuntu 24.04 + current GCC) for validating
# dftracer-utils against an up-to-date compiler/stdlib. Build tree lives in a
# bind-mounted build/build-docker-latest so rebuilds are incremental.
#
#   docker build -t dftracer-latest -f docker/gcc-latest.Dockerfile .
#   (or: scripts/docker-test.sh latest)
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
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

# The container runs as root; let OpenMPI's mpirun proceed.
ENV OMPI_ALLOW_RUN_AS_ROOT=1 OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1
WORKDIR /work
