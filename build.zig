const std = @import("std");

const Build = std.Build;
const LazyPath = Build.LazyPath;

/// Pinned to the same revision @napi-rs/canvas vendors, so the C shim below
/// is written against a Skia tree that combination is known to compile.
/// Milestone 151.
const skia_rev = "7219df0fb0ff64f26adad448f94e8c001b964e6a";
const skia_repo = "https://github.com/google/skia.git";

/// Files copied into the Skia checkout during bootstrap. `src` is relative to
/// this repo, `dest` is relative to the checkout root. The shim lives inside
/// the checkout because GN can only see targets under its source root.
const StagedFile = struct { src: []const u8, dest: []const u8 };
const staged_files = [_]StagedFile{
    .{ .src = "src/binding.h", .dest = "zig/binding.h" },
    .{ .src = "src/binding.cpp", .dest = "zig/binding.cpp" },
    .{ .src = "build-tools/BUILD.gn", .dest = "zig/BUILD.gn" },
    .{ .src = "build-tools/skia.gn", .dest = "zig/.gn" },
};

/// A CPU-only Skia carrying exactly the codecs and font machinery a browser
/// 2D canvas needs.
///
/// Two non-obvious entries:
///   - Every `skia_use_system_*` is pinned off. `is_official_build=true`
///     otherwise defaults them on, which breaks hermetic builds and, worse,
///     would make text rendering depend on whatever freetype the host has.
///   - The font managers are the custom (directory/embedded) ones and
///     fontconfig is off, so the font set is whatever we ship rather than
///     whatever the host has installed.
const gn_args = [_][]const u8{
    // No GPU: there is no window, no GL/Vulkan context, and canvas readback
    // has to be deterministic.
    "skia_enable_ganesh=false",
    "skia_enable_graphite=false",
    "skia_use_gl=false",
    "skia_use_vulkan=false",
    "skia_use_metal=false",
    "skia_use_dawn=false",
    "skia_use_webgl=false",
    "skia_use_webgpu=false",
    "skia_use_x11=false",
    "skia_use_egl=false",
    "skia_use_angle=false",

    // Text stack: FreeType rasterizes, HarfBuzz shapes, ICU handles bidi.
    "skia_use_freetype=true",
    "skia_use_harfbuzz=true",
    "skia_use_icu=true",
    "skia_enable_skunicode=true",
    "skia_use_fontations=false",

    // Fonts come from a directory we control, never from the host.
    "skia_enable_fontmgr_custom_directory=true",
    "skia_enable_fontmgr_custom_embedded=true",
    "skia_enable_fontmgr_custom_empty=true",
    "skia_enable_fontmgr_fontconfig=false",
    "skia_use_fontconfig=false",

    // The three formats toDataURL/convertToBlob accept, both directions
    // (decode is needed for drawImage of an HTMLImageElement).
    "skia_use_libpng_decode=true",
    "skia_use_libpng_encode=true",
    "skia_use_libjpeg_turbo_decode=true",
    "skia_use_libjpeg_turbo_encode=true",
    "skia_use_libwebp_decode=true",
    "skia_use_libwebp_encode=true",
    "skia_use_zlib=true",

    // Everything vendored.
    "skia_use_system_freetype2=false",
    "skia_use_system_harfbuzz=false",
    "skia_use_system_icu=false",
    "skia_use_system_libjpeg_turbo=false",
    "skia_use_system_libpng=false",
    "skia_use_system_libwebp=false",
    "skia_use_system_zlib=false",

    // Skia bundles PartitionAlloc by default, and so does V8. Linking both
    // archives into one binary then fails on ~70 duplicate
    // `partition_alloc::*` symbols, so Skia is left on plain malloc and V8
    // keeps its own allocator.
    "skia_use_partition_alloc=false",

    // Unused subsystems, kept out of the archive.
    "skia_use_expat=false",
    "skia_use_dng_sdk=false",
    "skia_use_piex=false",
    "skia_use_libavif=false",
    "skia_use_libjxl_decode=false",
    "skia_enable_pdf=false",
    "skia_enable_svg=false",
    "skia_enable_skottie=false",
    "skia_enable_tools=false",

    "cc=\"clang\"",
    "cxx=\"clang++\"",
};

const BuiltSkia = struct {
    step: *Build.Step,
    lib_path: LazyPath,
};

/// Resolves a toolchain library by name, using the same search the compiler
/// that built Skia uses, so the runtime always matches the binary.
fn hostLibrary(b: *Build, name: []const u8) []const u8 {
    const printed = b.run(&.{ "clang++", b.fmt("-print-file-name={s}", .{name}) });
    return b.dupe(std.mem.trim(u8, printed, " \t\r\n"));
}

pub fn build(b: *Build) !void {
    const io = b.graph.io;
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const cache_root = b.option(
        []const u8,
        "cache_root",
        "Directory holding the Skia checkout",
    ) orelse b.pathFromRoot(".skia-cache");

    const prebuilt_skia_path = b.option(
        []const u8,
        "prebuilt_skia_path",
        "Path to a prebuilt libc_skia.a, skipping the Skia build entirely",
    );

    std.Io.Dir.cwd().access(io, cache_root, .{}) catch {
        try std.Io.Dir.cwd().createDirPath(io, cache_root);
    };

    const skia_dir = b.fmt("{s}/skia", .{cache_root});
    const out_dir = if (optimize == .Debug) "out/canvas-debug" else "out/canvas";

    const built = if (prebuilt_skia_path) |path| BuiltSkia{
        .step = b.step("prebuilt_skia", "Use a prebuilt libc_skia.a"),
        .lib_path = .{ .cwd_relative = path },
    } else try buildSkia(b, io, skia_dir, out_dir, optimize);

    const build_step = b.step("build-skia", "Build libc_skia.a");
    build_step.dependOn(built.step);
    b.getInstallStep().dependOn(build_step);

    const binding = b.addTranslateC(.{
        .root_source_file = b.path("src/binding.h"),
        .target = target,
        .optimize = optimize,
    });
    // translate-c must not run before the archive exists, otherwise a fresh
    // checkout races the bootstrap.
    binding.step.dependOn(built.step);

    // Skia is compiled by the host clang++, which on Linux means libstdc++,
    // while Zig's `link_libcpp` would supply libc++. Statically folding in the
    // libstdc++ Skia was actually built against keeps the two from being mixed
    // by accident, and lets an embedder keep using libc++ for its own C++
    // dependencies: libstdc++ and libc++ mangle differently (`std::__cxx11`
    // vs `std::__1`), and Skia is built -fno-exceptions/-fno-rtti behind a
    // pure C header, so no C++ type ever crosses the boundary. libgcc_eh
    // supplies the _Unwind_* entry points libstdc++'s personality routine
    // references; nothing here throws, the symbols just have to resolve.
    const libgcc_eh = hostLibrary(b, "libgcc_eh.a");
    const host_libstdcxx = hostLibrary(b, "libstdc++.a");

    // An embedder that already links a C++ ABI runtime (V8 bundles a libc++)
    // provides `std::exception`, `std::type_info`, `__cxxabiv1::*` and the
    // standard exception types itself. Those are ABI-stable and live outside
    // libc++'s `std::__1` namespace on purpose, so linking a whole libstdc++
    // beside them fails on duplicate symbols rather than deduplicating.
    const cxx_abi_provider = b.option(
        []const u8,
        "cxx_abi_provider",
        "Archive that already provides the C++ ABI runtime (e.g. V8's, which bundles libc++); " ++
            "symbols it shares with libstdc++ are demoted to local instead of colliding",
    );

    const libstdcxx: LazyPath = if (cxx_abi_provider) |provider| blk: {
        const trim = b.addSystemCommand(&.{"sh"});
        trim.addFileArg(b.path("build-tools/cxx-runtime.sh"));
        trim.addArg(host_libstdcxx);
        const out = trim.addOutputFileArg("libstdc++_embeddable.a");
        trim.addArg(provider);
        break :blk out;
    } else .{ .cwd_relative = host_libstdcxx };

    const skia_module = b.addModule("skia", .{
        .root_source_file = b.path("src/skia.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    skia_module.addImport("binding", binding.createModule());
    skia_module.addObjectFile(built.lib_path);
    skia_module.addObjectFile(libstdcxx);
    skia_module.addObjectFile(.{ .cwd_relative = libgcc_eh });

    {
        const demo_module = b.createModule(.{
            .root_source_file = b.path("src/demo.zig"),
            .target = target,
            .optimize = optimize,
        });
        demo_module.addImport("skia", skia_module);

        const demo = b.addExecutable(.{ .name = "demo", .root_module = demo_module });
        const run_demo = b.addRunArtifact(demo);
        if (b.args) |args| run_demo.addArgs(args);
        b.step("demo", "Render the canvas fingerprint probe to a PNG").dependOn(&run_demo.step);
    }

    {
        const test_module = b.createModule(.{
            .root_source_file = b.path("src/skia.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        });
        test_module.addImport("binding", binding.createModule());
        test_module.addObjectFile(built.lib_path);
        test_module.addObjectFile(libstdcxx);
        test_module.addObjectFile(.{ .cwd_relative = libgcc_eh });

        const tests = b.addTest(.{ .root_module = test_module });
        const run_tests = b.addRunArtifact(tests);
        b.step("test", "Run tests").dependOn(&run_tests.step);
    }
}

fn buildSkia(
    b: *Build,
    io: std.Io,
    skia_dir: []const u8,
    out_dir: []const u8,
    optimize: std.builtin.OptimizeMode,
) !BuiltSkia {
    const bootstrap = try bootstrapSkia(b, io, skia_dir);

    // Staging is cheap and must rerun whenever the shim changes, so it is not
    // guarded by the bootstrap marker.
    var stage_step = bootstrap;
    for (staged_files) |file| {
        const copy = b.addSystemCommand(&.{ "cp", "-f" });
        copy.addFileArg(b.path(file.src));
        copy.addArg(b.fmt("{s}/{s}", .{ skia_dir, file.dest }));
        copy.step.dependOn(stage_step);
        stage_step = &copy.step;
    }

    var args: std.ArrayList(u8) = .empty;
    for (gn_args) |arg| {
        try args.appendSlice(b.allocator, arg);
        try args.append(b.allocator, ' ');
    }
    // Skia asserts these two are never both set.
    try args.appendSlice(b.allocator, if (optimize == .Debug)
        "is_debug=true"
    else
        "is_official_build=true");

    // --root-target=//zig is what gets the staged BUILD.gn parsed at all;
    // see build-tools/skia.gn.
    const gn_gen = b.addSystemCommand(&.{
        "bin/gn",
        "--root=.",
        "--root-target=//zig",
        "--dotfile=zig/.gn",
        "gen",
        out_dir,
        b.fmt("--args={s}", .{args.items}),
    });
    gn_gen.setCwd(.{ .cwd_relative = skia_dir });
    gn_gen.step.dependOn(stage_step);

    const ninja = b.addSystemCommand(&.{
        "third_party/ninja/ninja", "-C", out_dir, "zig:c_skia",
    });
    ninja.setCwd(.{ .cwd_relative = skia_dir });
    ninja.step.dependOn(&gn_gen.step);

    // bundle.sh globs the output directory, so archives left behind by an
    // earlier configuration would silently be merged back in — which is how
    // a disabled PartitionAlloc still ended up colliding with V8's copy.
    const clean_dead = b.addSystemCommand(&.{
        "third_party/ninja/ninja", "-C", out_dir, "-t", "cleandead",
    });
    clean_dead.setCwd(.{ .cwd_relative = skia_dir });
    clean_dead.step.dependOn(&ninja.step);

    const bundle_path = b.fmt("{s}/bundle/libskia_bundle.a", .{out_dir});
    const bundle = b.addSystemCommand(&.{"sh"});
    bundle.addFileArg(b.path("build-tools/bundle.sh"));
    bundle.addArgs(&.{ out_dir, bundle_path });
    bundle.setCwd(.{ .cwd_relative = skia_dir });
    bundle.step.dependOn(&clean_dead.step);

    return .{
        .step = &bundle.step,
        .lib_path = .{ .cwd_relative = b.fmt("{s}/{s}", .{ skia_dir, bundle_path }) },
    };
}

/// Fetches the pinned Skia revision and its dependencies. Guarded by a marker
/// file: the checkout plus third_party/externals is several gigabytes and
/// takes minutes, so it must not rerun on every `zig build`.
fn bootstrapSkia(b: *Build, io: std.Io, skia_dir: []const u8) !*Build.Step {
    const marker = b.fmt("{s}/.zig-skia-bootstrapped-{s}", .{ skia_dir, skia_rev[0..12] });

    if (std.Io.Dir.cwd().access(io, marker, .{})) |_| {
        const noop = b.addSystemCommand(&.{"true"});
        return &noop.step;
    } else |_| {}

    std.debug.print(
        "Fetching Skia {s} into {s} (several GB, this will take a while)...\n",
        .{ skia_rev[0..12], skia_dir },
    );

    const script = b.fmt(
        \\set -e
        \\mkdir -p '{[dir]s}'
        \\cd '{[dir]s}'
        \\if [ ! -d .git ]; then git init -q; fi
        \\git remote remove origin 2>/dev/null || true
        \\git remote add origin '{[repo]s}'
        \\git fetch --depth 1 -q origin '{[rev]s}'
        \\git checkout -q FETCH_HEAD
        \\python3 tools/git-sync-deps
        \\python3 bin/fetch-gn
        \\python3 bin/fetch-ninja
        \\touch '{[marker]s}'
    , .{ .dir = skia_dir, .repo = skia_repo, .rev = skia_rev, .marker = marker });

    const run = b.addSystemCommand(&.{ "sh", "-c", script });
    return &run.step;
}
