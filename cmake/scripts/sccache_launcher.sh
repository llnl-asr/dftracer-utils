#!/bin/sh
# Vectorscan's build_wrapper.sh reruns the compile command with
# --print-file-name; sccache takes that for a compile and fails on it.
for arg in "$@"; do
    case $arg in
        --print-* | -print-*) exec "$@" ;;
    esac
done
exec sccache "$@"
