# zig-skia

Skia as a Zig package: builds a pinned Skia into one self-contained static
library and exposes it through a pure-C shim, so a Zig project can render a
browser 2D canvas with the same rasterizer Chrome uses.

Modelled on the `zig-v8-fork` layout, minus the depot_tools bootstrap — Skia
syncs its own dependencies.

```
build.zig               bootstrap → gn gen → ninja → bundle
build.zig.zon
build-tools/
  BUILD.gn              the c_skia target, staged into <skia>/zig/
  skia.gn               GN dotfile, staged into <skia>/zig/.gn
  bundle.sh             merges the ~20 output archives into one .a
src/
  binding.h             pure-C API (translate-c reads this)
  binding.cpp           C++ implementation over Skia
  skia.zig              Zig wrapper, exported as the `skia` module
  demo.zig              renders the canvas fingerprint probe to a PNG
```

## Pinned version

Skia `7219df0fb0ff64f26adad448f94e8c001b964e6a` (milestone 151) — the same
revision `@napi-rs/canvas` vendors, so its `skia-c` shim is a usable reference
for extending `binding.cpp`.

## Requirements

`git`, `python3`, `clang`/`clang++`. Skia fetches its own `gn` and `ninja`.
The checkout plus `third_party/externals` is about 8.7 GB; a full build takes
around 40 s on 16 cores, and incremental shim edits about 2 s.

## Usage

```bash
zig build test                 # unit tests
zig build build-skia           # just produce libskia_bundle.a
```

The demo needs a font directory:

```bash
SKIA_FONT_DIR=/path/to/fonts SKIA_DEMO_OUT=probe.png zig build demo
```

As a dependency:

```zig
const skia = b.dependency("skia", .{
    .target = target,
    .optimize = optimize,
    .cache_root = b.pathFromRoot(".lp-cache"),
});
mod.addImport("skia", skia.module("skia"));
```

`-Dprebuilt_skia_path=/path/to/libskia_bundle.a` skips the Skia build, for CI
and for packaging.

## Notable build decisions

**No GPU.** Ganesh and Graphite are off: there is no window or GL context, and
canvas readback has to be deterministic. Everything is CPU raster.

**Nothing from the host.** Every `skia_use_system_*` is pinned off.
`is_official_build=true` otherwise defaults them on, which would make text
rendering depend on whichever freetype the host happens to ship.

**Fonts come from a directory, not fontconfig.** The font managers built in are
the custom directory/embedded ones. A browser that reports one platform while
serving another platform's font set contradicts itself, and fontconfig would
also make the same binary render differently on different hosts.

**Shaping goes through HarfBuzz, fallback through the font manager.**
`skia_use_harfbuzz`, `skia_use_icu` and `skia_enable_skunicode` are on, and
`//modules/skshaper` + `//modules/skunicode` are linked into the shim.
`SkShaper::MakeFontMgrRunIterator` resolves a codepoint the requested typeface
lacks against the font manager, so the fallback chain is the same font set the
directory manager serves and not whatever the host has installed.

**Canvas text metrics.** `makeCanvasFont` disables hinting and enables linear
metrics, because grid-fitting snaps advances to whole pixels while
`measureText()` must report the unrounded advance.

**Archives are merged by hand.** Skia ships a minimal GN toolchain whose
`alink` is a plain `ar rcs`, so `complete_static_lib` does not fold dependency
archives in — it lists them as order-only deps and the shim archive ends up
holding only `binding.o`. `build-tools/bundle.sh` merges them afterwards.

**libstdc++, not libc++.** Skia is compiled by the host `clang++`, which on
Linux means libstdc++, and `libstdc++.a` + `libgcc_eh.a` are linked in
statically. Building Skia with `zig c++` instead would align it with Zig's
libc++, but Skia's SIMD dispatch translation units pass `-march=x86-64-v3/v4`,
which `zig cc` rejects; disabling those would change rasterizer output.

**No PartitionAlloc.** Skia bundles it by default and so does V8; linking both
archives into one binary fails on ~70 duplicate `partition_alloc::*` symbols.
Skia is left on plain malloc so the embedder's allocator wins.

## Embedding beside another C++ runtime

Most of libstdc++ and libc++ coexist happily — they mangle differently
(`std::__cxx11` vs `std::__1`) and Skia is built `-fno-exceptions -fno-rtti`
behind a pure C header, so no C++ type crosses the boundary. The exception is
the ABI-stable half of the standard library: `std::exception`,
`std::logic_error`, `std::type_info`, `__cxxabiv1::*`. libc++ deliberately
keeps those *out* of its inline namespace so the two implementations
interoperate, which is exactly why they collide instead of coexisting.

Pass `-Dcxx_abi_provider=/path/to/their.a` and `build-tools/cxx-runtime.sh`
demotes every symbol both archives define strongly to a local one, leaving the
provider as the single global definition. Two details that took a wrong turn
first:

- Deleting the offending libstdc++ members instead does not work — they
  cross-reference each other, so dropping `cow-stdexcept.o` strands
  `functexcept.o`, which Skia needs for `std::__throw_length_error`.
- Only *strong* definitions may be demoted. Weak ones (vtables, inline
  functions, template instantiations) already deduplicate at link time, and
  localizing them discards the COMDAT groups they belong to, leaving
  relocations pointing at `DW.ref.__gxx_personality_v0`.

## Size

The archive is ~81 MB, but that is the whole of Skia. A binary that actually
uses the canvas path — raster surface, glyph rasterization, PNG encoding —
pulls in about **8 MB** after `--gc-sections`.

## Not done yet

`binding.h` covers surfaces, paints, paths, transforms, clip paths, linear and
radial gradients, blend modes, colour-matrix filters and shadows, HarfBuzz
shaping with per-character fallback, font matching, pixel transfer and
PNG/JPEG/WebP encoding. What a 2D canvas still needs beyond that:

- **Patterns.** The only shaders are the two gradients, so `createPattern` has
  nothing to bind to — an image shader with the four tiling modes is missing.
- **Image decode.** Nothing decodes an encoded image into a surface. The
  embedder decodes and hands over unpremultiplied RGBA through
  `skiac_surface_write_pixels`; only the encode direction is covered.
- **Layers.** No `saveLayer`, so anything that needs its own backdrop before
  being composited — a filter or a non-`source-over` operator applied to a
  group rather than a single draw — has to be staged by the caller into a
  second surface.
- **Conic gradients and dashed strokes.** `createConicGradient` and
  `setLineDash` have no counterpart on the shader or the paint.

## Font directory

`SkFontMgr_New_Custom_Directory` groups files into families by their internal
name, and `matchFamilyStyle` picks a face by CSS weight/width/slant. Shipping
only the regular weight of a family is a visible tell: a bold request then
answers with the 400 face and the renderer synthesizes the difference, which
has neither the metrics nor the pixels of the real bold. `zig build demo`
prints a style-coverage table that flags any style resolving to a synthesized
face.
