#!/bin/sh
# Produce a copy of libstdc++.a that can be linked beside another C++ runtime.
#
# Skia is compiled by the host clang++ and therefore needs libstdc++ for
# `std::__cxx11::basic_string` and friends. An embedder that already links a
# libc++ — V8 bundles one — also brings its own copy of the *ABI-stable* half
# of the standard library: `std::exception`, `std::logic_error`,
# `std::type_info`, `__cxxabiv1::*`. Those deliberately live outside libc++'s
# `std::__1` inline namespace so the two implementations interoperate, which
# is exactly why they collide at link time instead of coexisting.
#
# Rather than guess at which object files to drop — they cross-reference each
# other, so removing one strands the rest — demote exactly the symbols both
# archives define to local. Every such symbol still has one global definition
# (the provider's) plus a local copy for libstdc++'s internal use, and symbols
# unique to libstdc++ are left untouched.
#
# usage: cxx-runtime.sh <libstdc++.a> <output.a> <provider.a>...

set -e

source_archive="$1"
dest="$2"
shift 2

if [ -z "$source_archive" ] || [ -z "$dest" ] || [ "$#" -eq 0 ]; then
    echo "usage: cxx-runtime.sh <libstdc++.a> <output.a> <provider.a>..." >&2
    exit 1
fi

dest_dir=$(dirname "$dest")
mkdir -p "$dest_dir"
cp -f "$source_archive" "$dest"
chmod u+w "$dest"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Only *strong* definitions are considered. Weak ones (vtables, inline
# functions, template instantiations) already deduplicate at link time and
# demoting them breaks the COMDAT groups they belong to.
nm -g --defined-only "$dest" 2>/dev/null |
    awk '$2 ~ /^[TDBRG]$/ { print $3 }' | sort -u > "$work/ours"

for provider in "$@"; do
    nm -g --defined-only "$provider" 2>/dev/null |
        awk '$2 ~ /^[TWVDBRG]$/ { print $3 }'
done | sort -u > "$work/theirs"

# The unwinder's personality routine and the DWARF references to it are
# addressed through COMDAT groups keyed on the symbol name; localizing them
# discards the group while relocations still point at it.
comm -12 "$work/ours" "$work/theirs" |
    grep -vE '^(DW\.ref\.|__gxx_personality|_Unwind_)' > "$work/shared" || true

count=$(wc -l < "$work/shared" | tr -d ' ')
echo "cxx-runtime: localizing $count symbol(s) also provided by the embedder" >&2

if [ "$count" -gt 0 ]; then
    objcopy --localize-symbols="$work/shared" "$dest"
fi

ranlib "$dest"
