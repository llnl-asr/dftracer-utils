#!/bin/sh
# Install a pinned sccache into a manylinux container; verifies the checksum.
set -eu
version=v0.18.0
name=sccache-$version-$(uname -m)-unknown-linux-musl
base=https://github.com/mozilla/sccache/releases/download/$version
cd "$(mktemp -d)"
curl -fsSL -O "$base/$name.tar.gz"
curl -fsSL -O "$base/$name.tar.gz.sha256"
echo "$(cut -d' ' -f1 "$name.tar.gz.sha256")  $name.tar.gz" | sha256sum -c -
tar xzf "$name.tar.gz"
install -m 0755 "$name/sccache" /usr/local/bin/sccache
sccache --version
