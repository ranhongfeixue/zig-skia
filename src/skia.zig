//! Zig surface over the Skia C shim.
//!
//! Every type here owns a Skia handle and must be released with `deinit`.
//! Nothing allocates through a Zig allocator: Skia owns its own memory, and
//! pixel buffers are always caller-provided.

const std = @import("std");
const c = @import("binding");

pub const Error = error{
    SurfaceCreationFailed,
    EncodeFailed,
    ReadPixelsFailed,
    WritePixelsFailed,
    FontDirectoryNotFound,
    FamilyNotFound,
    OutOfMemory,
    ShaperUnavailable,
};

pub const Color = struct {
    r: u8,
    g: u8,
    b: u8,
    a: u8 = 255,

    pub const transparent: Color = .{ .r = 0, .g = 0, .b = 0, .a = 0 };
    pub const black: Color = .{ .r = 0, .g = 0, .b = 0 };
    pub const white: Color = .{ .r = 255, .g = 255, .b = 255 };

    /// Skia stores colours as 0xAARRGGBB.
    pub fn argb(self: Color) u32 {
        return (@as(u32, self.a) << 24) |
            (@as(u32, self.r) << 16) |
            (@as(u32, self.g) << 8) |
            @as(u32, self.b);
    }
};

/// The subset of SkBlendMode canvas exposes as globalCompositeOperation.
/// Ordinals must track SkBlendMode.
pub const BlendMode = enum(i32) {
    clear = 0,
    src = 1,
    dst = 2,
    src_over = 3,
    dst_over = 4,
    src_in = 5,
    dst_in = 6,
    src_out = 7,
    dst_out = 8,
    src_atop = 9,
    dst_atop = 10,
    xor = 11,
    plus = 12,
    modulate = 13,
    screen = 14,
    overlay = 15,
    darken = 16,
    lighten = 17,
    color_dodge = 18,
    color_burn = 19,
    hard_light = 20,
    soft_light = 21,
    difference = 22,
    exclusion = 23,
    multiply = 24,
    hue = 25,
    saturation = 26,
    color = 27,
    luminosity = 28,

    pub fn fromCss(name: []const u8) ?BlendMode {
        const table = .{
            .{ "source-over", BlendMode.src_over },
            .{ "source-in", BlendMode.src_in },
            .{ "source-out", BlendMode.src_out },
            .{ "source-atop", BlendMode.src_atop },
            .{ "destination-over", BlendMode.dst_over },
            .{ "destination-in", BlendMode.dst_in },
            .{ "destination-out", BlendMode.dst_out },
            .{ "destination-atop", BlendMode.dst_atop },
            .{ "lighter", BlendMode.plus },
            .{ "copy", BlendMode.src },
            .{ "xor", BlendMode.xor },
            .{ "multiply", BlendMode.multiply },
            .{ "screen", BlendMode.screen },
            .{ "overlay", BlendMode.overlay },
            .{ "darken", BlendMode.darken },
            .{ "lighten", BlendMode.lighten },
            .{ "color-dodge", BlendMode.color_dodge },
            .{ "color-burn", BlendMode.color_burn },
            .{ "hard-light", BlendMode.hard_light },
            .{ "soft-light", BlendMode.soft_light },
            .{ "difference", BlendMode.difference },
            .{ "exclusion", BlendMode.exclusion },
            .{ "hue", BlendMode.hue },
            .{ "saturation", BlendMode.saturation },
            .{ "color", BlendMode.color },
            .{ "luminosity", BlendMode.luminosity },
        };
        inline for (table) |entry| {
            if (std.mem.eql(u8, name, entry[0])) return entry[1];
        }
        return null;
    }

    pub fn toCss(self: BlendMode) []const u8 {
        return switch (self) {
            .src_over => "source-over",
            .src_in => "source-in",
            .src_out => "source-out",
            .src_atop => "source-atop",
            .dst_over => "destination-over",
            .dst_in => "destination-in",
            .dst_out => "destination-out",
            .dst_atop => "destination-atop",
            .plus => "lighter",
            .src => "copy",
            .xor => "xor",
            .multiply => "multiply",
            .screen => "screen",
            .overlay => "overlay",
            .darken => "darken",
            .lighten => "lighten",
            .color_dodge => "color-dodge",
            .color_burn => "color-burn",
            .hard_light => "hard-light",
            .soft_light => "soft-light",
            .difference => "difference",
            .exclusion => "exclusion",
            .hue => "hue",
            .saturation => "saturation",
            .color => "color",
            .luminosity => "luminosity",
            else => "source-over",
        };
    }
};

pub const LineCap = enum(i32) { butt = 0, round = 1, square = 2 };
/// 圖片取樣：imageSmoothingEnabled＝false 是 nearest；quality low／medium／high（擴充）。
pub const Sampling = enum(i32) { nearest = 0, linear = 1, mipmap = 2, cubic = 3 };
pub const LineJoin = enum(i32) { miter = 0, round = 1, bevel = 2 };
pub const FillRule = enum(i32) { nonzero = 0, evenodd = 1 };
pub const TextAlign = enum(i32) { start = 0, end = 1, center = 2 };
pub const TextBaseline = enum(i32) {
    alphabetic = 0,
    top = 1,
    hanging = 2,
    middle = 3,
    ideographic = 4,
    bottom = 5,
};

/// Encoded image bytes owned by Skia.
pub const Data = struct {
    handle: *c.skiac_data,

    pub fn deinit(self: Data) void {
        c.skiac_data_destroy(self.handle);
    }

    pub fn bytes(self: Data) []const u8 {
        const ptr = c.skiac_data_bytes(self.handle) orelse return &.{};
        return ptr[0..c.skiac_data_size(self.handle)];
    }
};

pub const Shader = struct {
    handle: *c.skiac_shader,

    pub fn deinit(self: Shader) void {
        c.skiac_shader_destroy(self.handle);
    }

    pub const Stop = struct { position: f32, color: Color };

    fn build(stops: []const Stop, buffer_colors: []u32, buffer_positions: []f32) usize {
        const count = @min(stops.len, buffer_colors.len);
        for (stops[0..count], 0..) |stop, index| {
            buffer_colors[index] = stop.color.argb();
            buffer_positions[index] = stop.position;
        }
        return count;
    }

    pub fn linearGradient(x0: f32, y0: f32, x1: f32, y1: f32, stops: []const Stop) ?Shader {
        var colors: [max_stops]u32 = undefined;
        var positions: [max_stops]f32 = undefined;
        const count = build(stops, &colors, &positions);
        if (count == 0) return null;

        const handle = c.skiac_shader_linear_gradient(
            x0,
            y0,
            x1,
            y1,
            &colors,
            &positions,
            @intCast(count),
        ) orelse return null;
        return .{ .handle = handle };
    }

    pub fn radialGradient(
        x0: f32,
        y0: f32,
        r0: f32,
        x1: f32,
        y1: f32,
        r1: f32,
        stops: []const Stop,
    ) ?Shader {
        var colors: [max_stops]u32 = undefined;
        var positions: [max_stops]f32 = undefined;
        const count = build(stops, &colors, &positions);
        if (count == 0) return null;

        const handle = c.skiac_shader_radial_gradient(
            x0,
            y0,
            r0,
            x1,
            y1,
            r1,
            &colors,
            &positions,
            @intCast(count),
        ) orelse return null;
        return .{ .handle = handle };
    }

    /// Gradients with more stops than this are truncated rather than
    /// allocating; no real page defines anywhere near this many.
    pub const max_stops = 64;

    /// createConicGradient：從 x 軸起算 start_radians（順時針）一整圈（擴充）。
    pub fn conicGradient(cx: f32, cy: f32, start_radians: f32, stops: []const Stop) ?Shader {
        var colors: [max_stops]u32 = undefined;
        var positions: [max_stops]f32 = undefined;
        const count = build(stops, &colors, &positions);
        if (count == 0) return null;
        const handle = c.skiac_shader_conic_gradient(cx, cy, start_radians, &colors, &positions, @intCast(count)) orelse return null;
        return .{ .handle = handle };
    }

    /// createPattern：以 source 目前的內容為圖樣；不重複的方向畫成透明（擴充）。
    pub fn surfacePattern(source: Surface, repeat_x: bool, repeat_y: bool, sampling: Sampling, matrix: ?[6]f32) ?Shader {
        const handle = c.skiac_shader_surface_pattern(
            source.handle,
            repeat_x,
            repeat_y,
            @intFromEnum(sampling),
            if (matrix) |*m| m else null,
        ) orelse return null;
        return .{ .handle = handle };
    }
};

pub const Paint = struct {
    handle: *c.skiac_paint,

    pub fn init() Paint {
        return .{ .handle = c.skiac_paint_create().? };
    }

    pub fn deinit(self: Paint) void {
        c.skiac_paint_destroy(self.handle);
    }

    pub fn setColor(self: Paint, color: Color) void {
        c.skiac_paint_set_color(self.handle, color.argb());
    }

    pub fn setAlpha(self: Paint, alpha: f32) void {
        c.skiac_paint_set_alpha(self.handle, alpha);
    }

    pub fn setAntiAlias(self: Paint, anti_alias: bool) void {
        c.skiac_paint_set_anti_alias(self.handle, anti_alias);
    }

    pub fn setFill(self: Paint) void {
        c.skiac_paint_set_stroke(self.handle, false, 0, 0, 0, 10);
    }

    pub fn setStroke(self: Paint, width: f32, cap: LineCap, join: LineJoin, miter_limit: f32) void {
        c.skiac_paint_set_stroke(
            self.handle,
            true,
            width,
            @intFromEnum(cap),
            @intFromEnum(join),
            miter_limit,
        );
    }

    pub fn setBlendMode(self: Paint, mode: BlendMode) void {
        c.skiac_paint_set_blend_mode(self.handle, @intFromEnum(mode));
    }

    pub fn setShader(self: Paint, shader: ?Shader) void {
        c.skiac_paint_set_shader(self.handle, if (shader) |s| s.handle else null);
    }

    /// setLineDash／lineDashOffset；空清單清除虛線（擴充）。
    pub fn setDash(self: Paint, intervals: []const f32, phase: f32) void {
        c.skiac_paint_set_dash(self.handle, if (intervals.len > 0) intervals.ptr else null, @intCast(intervals.len), phase);
    }

    /// Installs the CSS `filter` chain and the canvas shadow together; Skia
    /// composes them as a single image filter.
    pub fn setEffects(self: Paint, effects: Effects) void {
        c.skiac_paint_set_effects(
            self.handle,
            if (effects.color_matrix) |*m| m else null,
            effects.blur_sigma,
            effects.shadow_offset_x,
            effects.shadow_offset_y,
            effects.shadow_blur,
            effects.shadow_color.argb(),
        );
    }

    pub const Effects = struct {
        color_matrix: ?[20]f32 = null,
        blur_sigma: f32 = 0,
        shadow_offset_x: f32 = 0,
        shadow_offset_y: f32 = 0,
        shadow_blur: f32 = 0,
        shadow_color: Color = Color.transparent,

        pub fn isEmpty(self: Effects) bool {
            return self.color_matrix == null and self.blur_sigma == 0 and
                self.shadow_color.a == 0;
        }
    };
};

pub const Path = struct {
    handle: *c.skiac_path,

    pub fn init() Path {
        return .{ .handle = c.skiac_path_create().? };
    }

    pub fn deinit(self: Path) void {
        c.skiac_path_destroy(self.handle);
    }

    pub fn reset(self: Path) void {
        c.skiac_path_reset(self.handle);
    }

    pub fn isEmpty(self: Path) bool {
        return c.skiac_path_is_empty(self.handle);
    }

    pub fn setFillRule(self: Path, rule: FillRule) void {
        c.skiac_path_set_fill_type(self.handle, @intFromEnum(rule));
    }

    /// arc／ellipse：Blink 的 Path::AddEllipse（conic 曲線），弧建立後乘上 matrix 接在目前子路徑後面（擴充）。
    pub fn addArc(self: Path, cx: f32, cy: f32, rx: f32, ry: f32, rotation: f32, start: f32, sweep: f32, matrix: ?[6]f32) void {
        c.skiac_path_add_arc(self.handle, cx, cy, rx, ry, rotation, start, sweep, if (matrix) |*m| m else null);
    }

    /// isPointInPath（擴充）。
    pub fn contains(self: Path, x: f32, y: f32, rule: FillRule) bool {
        return c.skiac_path_contains(self.handle, x, y, @intFromEnum(rule));
    }

    /// isPointInStroke：照筆畫參數（含虛線）展開輪廓後判斷（擴充）。
    pub fn strokeContains(self: Path, x: f32, y: f32, width: f32, cap: LineCap, join: LineJoin, miter_limit: f32, dashes: []const f32, phase: f32) bool {
        return c.skiac_path_stroke_contains(
            self.handle,
            x,
            y,
            width,
            @intFromEnum(cap),
            @intFromEnum(join),
            miter_limit,
            if (dashes.len > 0) dashes.ptr else null,
            @intCast(dashes.len),
            phase,
        );
    }

    pub fn moveTo(self: Path, x: f32, y: f32) void {
        c.skiac_path_move_to(self.handle, x, y);
    }

    pub fn lineTo(self: Path, x: f32, y: f32) void {
        c.skiac_path_line_to(self.handle, x, y);
    }

    pub fn quadTo(self: Path, cx: f32, cy: f32, x: f32, y: f32) void {
        c.skiac_path_quad_to(self.handle, cx, cy, x, y);
    }

    pub fn cubicTo(self: Path, c1x: f32, c1y: f32, c2x: f32, c2y: f32, x: f32, y: f32) void {
        c.skiac_path_cubic_to(self.handle, c1x, c1y, c2x, c2y, x, y);
    }

    pub fn arcToTangent(self: Path, x1: f32, y1: f32, x2: f32, y2: f32, radius: f32) void {
        c.skiac_path_arc_to_tangent(self.handle, x1, y1, x2, y2, radius);
    }

    pub fn ellipse(
        self: Path,
        x: f32,
        y: f32,
        radius_x: f32,
        radius_y: f32,
        rotation: f32,
        start_angle: f32,
        end_angle: f32,
        anticlockwise: bool,
    ) void {
        c.skiac_path_ellipse(
            self.handle,
            x,
            y,
            radius_x,
            radius_y,
            rotation,
            start_angle,
            end_angle,
            anticlockwise,
        );
    }

    /// Canvas has no separate circle primitive; arc() is an ellipse with equal
    /// radii and no rotation.
    pub fn arc(
        self: Path,
        x: f32,
        y: f32,
        radius: f32,
        start_angle: f32,
        end_angle: f32,
        anticlockwise: bool,
    ) void {
        self.ellipse(x, y, radius, radius, 0, start_angle, end_angle, anticlockwise);
    }

    pub fn rect(self: Path, x: f32, y: f32, width: f32, height: f32) void {
        c.skiac_path_rect(self.handle, x, y, width, height);
    }

    pub fn close(self: Path) void {
        c.skiac_path_close(self.handle);
    }
};

/// Borrowed from its Surface — do not outlive it, and there is no deinit.
pub const Canvas = struct {
    handle: *c.skiac_canvas,

    pub fn save(self: Canvas) void {
        c.skiac_canvas_save(self.handle);
    }

    pub fn restore(self: Canvas) void {
        c.skiac_canvas_restore(self.handle);
    }

    pub fn translate(self: Canvas, dx: f32, dy: f32) void {
        c.skiac_canvas_translate(self.handle, dx, dy);
    }

    pub fn scale(self: Canvas, sx: f32, sy: f32) void {
        c.skiac_canvas_scale(self.handle, sx, sy);
    }

    pub fn rotate(self: Canvas, radians: f32) void {
        c.skiac_canvas_rotate(self.handle, radians);
    }

    pub fn concat(self: Canvas, m: [6]f32) void {
        c.skiac_canvas_concat(self.handle, m[0], m[1], m[2], m[3], m[4], m[5]);
    }

    pub fn setMatrix(self: Canvas, m: [6]f32) void {
        c.skiac_canvas_set_matrix(self.handle, m[0], m[1], m[2], m[3], m[4], m[5]);
    }

    pub fn resetMatrix(self: Canvas) void {
        c.skiac_canvas_reset_matrix(self.handle);
    }

    pub fn matrix(self: Canvas) [6]f32 {
        var out: [6]f32 = undefined;
        c.skiac_canvas_get_matrix(self.handle, &out);
        return out;
    }

    pub fn clipPath(self: Canvas, path: Path, rule: FillRule) void {
        c.skiac_canvas_clip_path(self.handle, path.handle, rule == .evenodd);
    }

    pub fn clear(self: Canvas, color: Color) void {
        c.skiac_canvas_clear(self.handle, color.argb());
    }

    pub fn drawRect(self: Canvas, x: f32, y: f32, width: f32, height: f32, paint: Paint) void {
        c.skiac_canvas_draw_rect(self.handle, x, y, width, height, paint.handle);
    }

    pub fn drawPath(self: Canvas, path: Path, paint: Paint) void {
        c.skiac_canvas_draw_path(self.handle, path.handle, paint.handle);
    }

    pub fn drawSurfaceRect(
        self: Canvas,
        source: Surface,
        src: [4]f32,
        dst: [4]f32,
        paint: ?Paint,
    ) void {
        c.skiac_canvas_draw_surface_rect(
            self.handle,
            source.handle,
            src[0],
            src[1],
            src[2],
            src[3],
            dst[0],
            dst[1],
            dst[2],
            dst[3],
            if (paint) |p| p.handle else null,
        );
    }

    /// drawImage 的取樣方式照 imageSmoothing（擴充）。
    pub fn drawSurfaceRectSampled(self: Canvas, source: Surface, src: [4]f32, dst: [4]f32, sampling: Sampling, paint: ?Paint) void {
        c.skiac_canvas_draw_surface_rect_sampled(
            self.handle,
            source.handle,
            src[0],
            src[1],
            src[2],
            src[3],
            dst[0],
            dst[1],
            dst[2],
            dst[3],
            @intFromEnum(sampling),
            if (paint) |p| p.handle else null,
        );
    }

    /// 整張畫布的圖層，restore 時用 paint 的合成模式／透明度／濾鏡合成回去（擴充）。
    pub fn saveLayer(self: Canvas, paint: ?Paint) void {
        c.skiac_canvas_save_layer(self.handle, if (paint) |p| p.handle else null);
    }
};

pub const Surface = struct {
    handle: *c.skiac_surface,

    pub fn init(width: u32, height: u32) Error!Surface {
        const handle = c.skiac_surface_create_rgba(
            @intCast(width),
            @intCast(height),
        ) orelse return error.SurfaceCreationFailed;
        return .{ .handle = handle };
    }

    pub fn deinit(self: Surface) void {
        c.skiac_surface_destroy(self.handle);
    }

    pub fn canvas(self: Surface) Canvas {
        return .{ .handle = c.skiac_surface_get_canvas(self.handle).? };
    }

    /// `destination` must hold width*height*4 bytes; pixels come back
    /// unpremultiplied RGBA, ready for ImageData.
    pub fn readPixels(
        self: Surface,
        destination: []u8,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
    ) Error!void {
        const needed = @as(usize, width) * @as(usize, height) * 4;
        std.debug.assert(destination.len >= needed);
        if (!c.skiac_surface_read_pixels(
            self.handle,
            destination.ptr,
            x,
            y,
            @intCast(width),
            @intCast(height),
        )) {
            return error.ReadPixelsFailed;
        }
    }

    /// `source` is unpremultiplied RGBA, as ImageData stores it.
    pub fn writePixels(
        self: Surface,
        source: []const u8,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
    ) Error!void {
        const needed = @as(usize, width) * @as(usize, height) * 4;
        std.debug.assert(source.len >= needed);
        if (!c.skiac_surface_write_pixels(
            self.handle,
            source.ptr,
            x,
            y,
            @intCast(width),
            @intCast(height),
        )) {
            return error.WritePixelsFailed;
        }
    }

    pub fn encodePng(self: Surface) Error!Data {
        const data = c.skiac_surface_encode_png(self.handle) orelse return error.EncodeFailed;
        return .{ .handle = data };
    }

    pub fn encodeJpeg(self: Surface, quality: i32) Error!Data {
        const data = c.skiac_surface_encode_jpeg(self.handle, quality) orelse
            return error.EncodeFailed;
        return .{ .handle = data };
    }

    pub fn encodeWebp(self: Surface, quality: i32) Error!Data {
        const data = c.skiac_surface_encode_webp(self.handle, quality) orelse
            return error.EncodeFailed;
        return .{ .handle = data };
    }
};

/// Mirrors CSS font selection, which is also how SkFontStyle is expressed.
pub const FontStyle = struct {
    weight: i32 = 400,
    width: i32 = 5,
    slant: Slant = .upright,

    pub const Slant = enum(i32) { upright = 0, italic = 1, oblique = 2 };

    pub const normal: FontStyle = .{};
    pub const bold: FontStyle = .{ .weight = 700 };
    pub const italic: FontStyle = .{ .slant = .italic };
    pub const bold_italic: FontStyle = .{ .weight = 700, .slant = .italic };
};

pub const TextMetrics = c.skiac_text_metrics;

pub const Typeface = struct {
    handle: *c.skiac_typeface,

    pub fn deinit(self: Typeface) void {
        c.skiac_typeface_destroy(self.handle);
    }

    /// The style of the face that was actually selected, which is not
    /// necessarily the one asked for: a family with no real bold answers a
    /// weight-700 request with its 400 face, leaving the renderer to
    /// synthesize the difference.
    pub fn style(self: Typeface) FontStyle {
        var weight: i32 = 0;
        var width: i32 = 0;
        var slant: i32 = 0;
        c.skiac_typeface_get_style(self.handle, &weight, &width, &slant);
        return .{
            .weight = weight,
            .width = width,
            .slant = @enumFromInt(slant),
        };
    }

    pub fn familyName(self: Typeface, buffer: []u8) ?[]const u8 {
        const length = c.skiac_typeface_get_family_name(
            self.handle,
            buffer.ptr,
            @intCast(buffer.len),
        );
        if (length < 0) return null;
        return buffer[0..@intCast(length)];
    }
};

/// HarfBuzz shaping plus the font manager consulted for per-character
/// fallback. Created once and reused for every text operation.
pub const Shaper = struct {
    handle: *c.skiac_shaper,

    pub fn init(manager: FontManager) Error!Shaper {
        const handle = c.skiac_shaper_create(manager.handle) orelse return error.ShaperUnavailable;
        return .{ .handle = handle };
    }

    pub fn deinit(self: Shaper) void {
        c.skiac_shaper_destroy(self.handle);
    }

    pub fn measure(self: Shaper, text: []const u8, typeface: ?Typeface, size: f32) TextMetrics {
        var out: TextMetrics = undefined;
        c.skiac_shaper_measure(
            self.handle,
            text.ptr,
            text.len,
            if (typeface) |t| t.handle else null,
            size,
            &out,
        );
        return out;
    }

    pub fn draw(
        self: Shaper,
        canvas: Canvas,
        text: []const u8,
        x: f32,
        y: f32,
        typeface: ?Typeface,
        size: f32,
        align_: TextAlign,
        baseline: TextBaseline,
        paint: Paint,
    ) void {
        c.skiac_shaper_draw(
            self.handle,
            canvas.handle,
            text.ptr,
            text.len,
            x,
            y,
            if (typeface) |t| t.handle else null,
            size,
            @intFromEnum(align_),
            @intFromEnum(baseline),
            paint.handle,
        );
    }
};

/// Serves only the fonts found in one directory, so the available font set is
/// identical on every host rather than whatever the OS has installed.
pub const FontManager = struct {
    handle: *c.skiac_fontmgr,

    pub fn initFromDirectory(directory: [:0]const u8) Error!FontManager {
        const handle = c.skiac_fontmgr_create_from_dir(directory.ptr) orelse
            return error.FontDirectoryNotFound;
        return .{ .handle = handle };
    }

    pub fn deinit(self: FontManager) void {
        c.skiac_fontmgr_destroy(self.handle);
    }

    pub fn countFamilies(self: FontManager) u32 {
        return @intCast(@max(0, c.skiac_fontmgr_count_families(self.handle)));
    }

    /// Writes into `buffer` and returns the slice of it holding the name.
    pub fn familyName(self: FontManager, index: u32, buffer: []u8) ?[]const u8 {
        const length = c.skiac_fontmgr_get_family_name(
            self.handle,
            @intCast(index),
            buffer.ptr,
            @intCast(buffer.len),
        );
        if (length < 0) return null;
        return buffer[0..@intCast(length)];
    }

    pub fn matchFamily(self: FontManager, family: [:0]const u8) Error!Typeface {
        const handle = c.skiac_fontmgr_match_family(self.handle, family.ptr) orelse
            return error.FamilyNotFound;
        return .{ .handle = handle };
    }

    /// Returns the closest face in the family, degrading the way the CSS font
    /// matching algorithm does rather than failing.
    pub fn matchFamilyStyle(
        self: FontManager,
        family: [:0]const u8,
        font_style: FontStyle,
    ) Error!Typeface {
        const handle = c.skiac_fontmgr_match_family_style(
            self.handle,
            family.ptr,
            font_style.weight,
            font_style.width,
            @intFromEnum(font_style.slant),
        ) orelse return error.FamilyNotFound;
        return .{ .handle = handle };
    }
};

const testing = std.testing;

test "surface: draw and encode PNG" {
    const surface = try Surface.init(200, 100);
    defer surface.deinit();

    const canvas = surface.canvas();
    canvas.clear(Color.white);

    const paint = Paint.init();
    defer paint.deinit();

    paint.setColor(.{ .r = 220, .g = 40, .b = 40 });
    canvas.drawRect(10, 10, 80, 50, paint);

    const png = try surface.encodePng();
    defer png.deinit();

    const bytes = png.bytes();
    try testing.expect(bytes.len > 0);
    // PNG signature.
    try testing.expectEqualSlices(u8, &.{ 0x89, 'P', 'N', 'G' }, bytes[0..4]);
}

test "surface: readPixels returns what was drawn" {
    const surface = try Surface.init(4, 4);
    defer surface.deinit();

    const paint = Paint.init();
    defer paint.deinit();
    paint.setAntiAlias(false);
    paint.setColor(.{ .r = 10, .g = 20, .b = 30, .a = 255 });
    surface.canvas().drawRect(0, 0, 4, 4, paint);

    var pixels: [4 * 4 * 4]u8 = undefined;
    try surface.readPixels(&pixels, 0, 0, 4, 4);
    try testing.expectEqualSlices(u8, &.{ 10, 20, 30, 255 }, pixels[0..4]);
}

test "surface: writePixels round-trips through readPixels" {
    const surface = try Surface.init(2, 2);
    defer surface.deinit();

    const source = [_]u8{
        1,  2,  3,  255,
        4,  5,  6,  255,
        7,  8,  9,  255,
        10, 11, 12, 255,
    };
    try surface.writePixels(&source, 0, 0, 2, 2);

    var pixels: [16]u8 = undefined;
    try surface.readPixels(&pixels, 0, 0, 2, 2);
    try testing.expectEqualSlices(u8, &source, &pixels);
}

test "path: bezier and ellipse produce a fillable path" {
    const path = Path.init();
    defer path.deinit();
    try testing.expect(path.isEmpty());

    path.moveTo(0, 0);
    path.cubicTo(10, 0, 20, 10, 20, 20);
    path.quadTo(10, 30, 0, 20);
    path.close();
    try testing.expect(!path.isEmpty());

    path.reset();
    try testing.expect(path.isEmpty());
    path.ellipse(10, 10, 8, 5, 0.5, 0, std.math.tau, false);
    try testing.expect(!path.isEmpty());
}

test "gradient: a radial shader fills with interpolated colour" {
    const surface = try Surface.init(16, 16);
    defer surface.deinit();

    const shader = Shader.radialGradient(0, 0, 0, 0, 0, 16, &.{
        .{ .position = 0, .color = .{ .r = 255, .g = 0, .b = 0 } },
        .{ .position = 1, .color = .{ .r = 0, .g = 0, .b = 255 } },
    }) orelse return error.SurfaceCreationFailed;
    defer shader.deinit();

    const paint = Paint.init();
    defer paint.deinit();
    paint.setShader(shader);
    surface.canvas().drawRect(0, 0, 16, 16, paint);

    var pixels: [16 * 16 * 4]u8 = undefined;
    try surface.readPixels(&pixels, 0, 0, 16, 16);

    // Centre is the first stop, the far corner has moved towards the second.
    try testing.expect(pixels[0] > 200);
    const corner = (15 * 16 + 15) * 4;
    try testing.expect(pixels[corner + 2] > pixels[corner]);
}

test "canvas: save and restore unwind the matrix" {
    const surface = try Surface.init(8, 8);
    defer surface.deinit();
    const canvas = surface.canvas();

    canvas.save();
    canvas.scale(2, 3);
    try testing.expectApproxEqAbs(@as(f32, 2), canvas.matrix()[0], 0.0001);
    canvas.restore();
    try testing.expectApproxEqAbs(@as(f32, 1), canvas.matrix()[0], 0.0001);
}

test "fonts: shape and measure with a real family" {
    const directory = std.c.getenv("SKIA_FONT_DIR") orelse return error.SkipZigTest;
    const manager = try FontManager.initFromDirectory(std.mem.span(directory));
    defer manager.deinit();

    // A directory with no usable font still reports one family, named "",
    // because Skia inserts an empty placeholder — so the name has to be
    // checked, not just the count.
    try testing.expect(manager.countFamilies() > 0);
    var name_buffer: [256]u8 = undefined;
    const first = manager.familyName(0, &name_buffer) orelse return error.FamilyNotFound;
    try testing.expect(first.len > 0);

    const family = std.c.getenv("SKIA_FONT_FAMILY") orelse return error.SkipZigTest;
    const typeface = try manager.matchFamily(std.mem.span(family));
    defer typeface.deinit();

    const shaper = try Shaper.init(manager);
    defer shaper.deinit();

    // The shaped advance must agree with the unshaped one for plain Latin,
    // and the ink box must be the tight one rather than the font-wide box
    // SkTextBlob::bounds() would report.
    const metrics = shaper.measure("Hello", typeface, 16);
    try testing.expectApproxEqAbs(@as(f32, 36.460938), metrics.width, 0.01);
    try testing.expect(metrics.actual_bounding_box_right < metrics.width + 1);
    try testing.expect(metrics.font_bounding_box_ascent > 0);
}
