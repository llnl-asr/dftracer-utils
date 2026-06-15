# Valgrind test environment for dftracer-utils.
#
# Valgrind does not run on macOS (especially Apple Silicon), so this image
# provides a Linux toolchain + Valgrind to memory-check the C++ tests and the
# native Python binding.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        ccache \
        valgrind \
        git \
        pkg-config \
        zlib1g-dev \
        libsqlite3-dev \
        libopenmpi-dev \
        openmpi-bin \
        python3 \
        python3-dev \
        python3-venv \
        python3-pip \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Keep CPM/source downloads and ccache inside the bind-mounted workspace so
# they persist across container runs.
ENV CPM_SOURCE_CACHE=/work/.cpmsource \
    CCACHE_DIR=/work/.ccache \
    CCACHE_MAXSIZE=2G

WORKDIR /work

ENTRYPOINT ["/work/tests/valgrind/run.sh"]
CMD ["all"]
