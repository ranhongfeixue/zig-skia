//! Renders the canvas-fingerprint pattern that scripts like FingerprintJS and
//! BrowserLeaks draw, and writes it to a PNG. Proves the whole path end to
//! end: raster surface, real glyph rasterization from a font directory,
//! alpha compositing, and PNG encoding.
//!
//! usage: SKIA_FONT_DIR=<dir> SKIA_DEMO_OUT=<out.png> demo

const std = @import("std");
const skia = @import("skia");

const probe_text = "Cwm fjordbank glyphs vext quiz,";
const probe_emoji = "\u{1F603}";

/// For each family, asks for the four CSS styles and reports what came back.
/// A request that answers with a different style means the family has no such
/// face and the renderer would synthesize it — faux bold and faux italic have
/// both different metrics and different pixels from the real faces Chrome uses.
fn reportStyleCoverage(fonts: skia.FontManager) !void {
    const families = [_][:0]const u8{
        "Arial",
        "Times New Roman",
        "Courier New",
        "Segoe UI",
    };
    const styles = [_]struct { label: []const u8, value: skia.FontStyle }{
        .{ .label = "regular", .value = skia.FontStyle.normal },
        .{ .label = "bold", .value = skia.FontStyle.bold },
        .{ .label = "italic", .value = skia.FontStyle.italic },
        .{ .label = "bold+italic", .value = skia.FontStyle.bold_italic },
    };

    std.debug.print("\nstyle coverage (16px, \"Hello\"):\n", .{});
    for (families) |family| {
        for (styles) |style| {
            const typeface = fonts.matchFamilyStyle(family, style.value) catch {
                std.debug.print("  {s:<16} {s:<12} MISSING\n", .{ family, style.label });
                continue;
            };
            defer typeface.deinit();

            const got = typeface.style();
            const synthesized = got.weight != style.value.weight or got.slant != style.value.slant;
            std.debug.print("  {s:<16} {s:<12} -> weight={d:<4} slant={s:<8} width={d:<10.4} {s}\n", .{
                family,
                style.label,
                got.weight,
                @tagName(got.slant),
                typeface.measureText("Hello", 16),
                if (synthesized) "SYNTHESIZED" else "real face",
            });
        }
    }
}

pub fn main() !void {
    const font_dir = std.mem.span(std.c.getenv("SKIA_FONT_DIR") orelse {
        std.debug.print("set SKIA_FONT_DIR and SKIA_DEMO_OUT\n", .{});
        return error.InvalidArguments;
    });
    const output_path = std.c.getenv("SKIA_DEMO_OUT") orelse {
        std.debug.print("set SKIA_FONT_DIR and SKIA_DEMO_OUT\n", .{});
        return error.InvalidArguments;
    };

    const fonts = try skia.FontManager.initFromDirectory(font_dir);
    defer fonts.deinit();

    var name_buffer: [256]u8 = undefined;
    const family_count = fonts.countFamilies();
    std.debug.print("loaded {d} families from {s}:\n", .{ family_count, font_dir });
    for (0..family_count) |index| {
        const name = fonts.familyName(@intCast(index), &name_buffer) orelse continue;
        std.debug.print("  - \"{s}\"\n", .{name});
    }

    try reportStyleCoverage(fonts);

    // 300x150 is the HTML default canvas size.
    const surface = try skia.Surface.init(300, 150);
    defer surface.deinit();

    const canvas = surface.canvas();
    canvas.clear(skia.Color.white);

    const paint = skia.Paint.init();
    defer paint.deinit();

    // #f60 rectangle, as in the standard probe.
    paint.setColor(.{ .r = 0xff, .g = 0x66, .b = 0x00 });
    canvas.drawRect(125, 1, 62, 20, paint);

    const arial = try fonts.matchFamily("Arial");
    defer arial.deinit();

    // Two overlapping runs, the second semi-transparent, so the output
    // depends on both glyph rasterization and alpha compositing.
    paint.setColor(.{ .r = 0x00, .g = 0x66, .b = 0x99 });
    canvas.drawText(probe_text, 2, 15, arial, 14, paint);

    paint.setColor(.{ .r = 102, .g = 204, .b = 0, .a = 178 });
    canvas.drawText(probe_text, 4, 17, arial, 14, paint);

    std.debug.print("\nmeasureText(\"{s}\") @14px Arial = {d}\n", .{
        probe_text,
        arial.measureText(probe_text, 14),
    });

    // Cross-check against widths captured from real Chrome (16px baseline),
    // as recorded in the browser's TextMetrics reference table.
    const probes = [_]struct { label: []const u8, text: []const u8, chrome: ?f64 }{
        .{ .label = "\"Hello\"", .text = "Hello", .chrome = null },
        // What the fingerprint probe actually sends: the emoji's UTF-8 bytes
        // each widened into their own code unit, so Chrome measures four
        // characters rather than one glyph.
        .{
            .label = "mojibake U+1F600",
            .text = "\xC3\xB0\xC2\x9F\xC2\x98\xC2\x80",
            .chrome = 57.727996826171875,
        },
        .{ .label = "real U+1F600", .text = "\u{1F600}", .chrome = null },
    };
    std.debug.print("\n16px Arial vs captured Chrome:\n", .{});
    for (probes) |probe| {
        const measured = arial.measureText(probe.text, 16);
        if (probe.chrome) |expected| {
            std.debug.print("  {s:<18} skia={d:<12.6} chrome={d:<12.6} delta={d:.6}\n", .{
                probe.label,
                measured,
                expected,
                measured - expected,
            });
        } else {
            std.debug.print("  {s:<18} skia={d:.6}\n", .{ probe.label, measured });
        }
    }

    if (fonts.matchFamily("Segoe UI Emoji")) |emoji| {
        defer emoji.deinit();
        paint.setColor(skia.Color.black);
        canvas.drawText(probe_emoji, 8, 60, emoji, 32, paint);
        std.debug.print("measureText(emoji) @32px Segoe UI Emoji = {d}\n", .{
            emoji.measureText(probe_emoji, 32),
        });
    } else |_| {
        std.debug.print("Segoe UI Emoji not present, skipping emoji run\n", .{});
    }

    // A couple of shapes to exercise the AA rasterizer.
    paint.setColor(.{ .r = 40, .g = 90, .b = 220, .a = 200 });
    canvas.drawCircle(230, 100, 34, paint);
    paint.setStroke(true, 3);
    paint.setColor(.{ .r = 20, .g = 20, .b = 20 });
    canvas.drawCircle(230, 100, 34, paint);

    const png = try surface.encodePng();
    defer png.deinit();

    const bytes = png.bytes();
    const file = std.c.fopen(output_path, "wb") orelse return error.OpenFailed;
    defer _ = std.c.fclose(file);
    if (std.c.fwrite(bytes.ptr, 1, bytes.len, file) != bytes.len) return error.WriteFailed;

    std.debug.print("\nwrote {s} ({d} bytes)\n", .{ output_path, bytes.len });
}
