#!/bin/sh
# Merge every archive Skia produced into one self-contained static library.
#
# Skia's GN toolchain does not implement `complete_static_lib`, so the shim
# archive holds only binding.o and the ~20 dependency archives sit beside it.
# Consumers should link exactly one file, so fold them together with an ar
# MRI script.
#
# usage: bundle.sh <ninja-out-dir> <output.a>

set -e

out_dir="$1"
dest="$2"

if [ -z "$out_dir" ] || [ -z "$dest" ]; then
    echo "usage: bundle.sh <ninja-out-dir> <output.a>" >&2
    exit 1
fi

dest_dir=$(dirname "$dest")
mkdir -p "$dest_dir"
rm -f "$dest"

# `ar -M` refuses to overwrite, hence the rm above. Members keep their
# per-target name prefixes (skia.SkCanvas.o, libc_skia.binding.o, ...) so
# folding the archives together does not collide.
{
    echo "create $dest"
    for archive in "$out_dir"/*.a; do
        [ -e "$archive" ] || continue
        case "$archive" in
            *"$(basename "$dest")") continue ;;
        esac
        echo "addlib $archive"
    done
    echo "save"
    echo "end"
} | ar -M

ranlib "$dest"
