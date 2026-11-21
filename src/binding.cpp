// C++ implementation of the C shim declared in binding.h.
//
// Handles are the Skia objects themselves, reinterpret_cast to the opaque C
// types. Refcounted Skia objects (SkData, SkTypeface, SkFontMgr, SkShader) are
// handed out with a reference the caller owns and must release; plain heap
// objects (SkPaint, SkPath, the shaper bundle) are new/delete.

#include "binding.h"

#include "include/core/SkAlphaType.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkColorType.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathTypes.h"
#include "include/core/SkPathUtils.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkString.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTileMode.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkImageFilters.h"
#include "include/encode/SkJpegEncoder.h"
#include "include/encode/SkPngEncoder.h"
#include "include/encode/SkWebpEncoder.h"
#include "include/ports/SkFontMgr_directory.h"

#include "modules/skshaper/include/SkShaper.h"
#include "modules/skshaper/include/SkShaper_harfbuzz.h"
#include "modules/skshaper/include/SkShaper_skunicode.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "modules/skunicode/include/SkUnicode_icu.h"

#include <cstring>
#include <memory>
#include <vector>

namespace {

SkSurface* asSurface(skiac_surface* s) { return reinterpret_cast<SkSurface*>(s); }
SkCanvas* asCanvas(skiac_canvas* c) { return reinterpret_cast<SkCanvas*>(c); }
SkPaint* asPaint(skiac_paint* p) { return reinterpret_cast<SkPaint*>(p); }
SkPathBuilder* asPath(skiac_path* p) { return reinterpret_cast<SkPathBuilder*>(p); }
SkShader* asShader(skiac_shader* s) { return reinterpret_cast<SkShader*>(s); }
SkData* asData(skiac_data* d) { return reinterpret_cast<SkData*>(d); }
SkFontMgr* asFontMgr(skiac_fontmgr* m) { return reinterpret_cast<SkFontMgr*>(m); }
SkTypeface* asTypeface(skiac_typeface* t) { return reinterpret_cast<SkTypeface*>(t); }

// Everything HarfBuzz shaping needs, built once and reused: the shaper itself,
// the ICU-backed bidi/script data, and the font manager consulted when the
// requested typeface has no glyph for a character.
struct ShaperBundle {
    sk_sp<SkFontMgr> fallback;
    sk_sp<SkUnicode> unicode;
    std::unique_ptr<SkShaper> shaper;
};

ShaperBundle* asShaper(skiac_shaper* s) { return reinterpret_cast<ShaperBundle*>(s); }

// Releases ownership of `data` to the caller as an opaque handle.
skiac_data* releaseData(sk_sp<SkData> data) {
    if (!data) return nullptr;
    return reinterpret_cast<skiac_data*>(data.release());
}

bool peekSurfacePixels(SkSurface* surface, SkPixmap* out) {
    return surface != nullptr && surface->peekPixels(out);
}

// Builds a font configured the way a browser configures canvas text.
//
// Hinting is off and metrics are linear on purpose: grid-fitting would snap
// advances to whole pixels, while measureText() is specified to report the
// unrounded advance.
// Windows 上 Chrome（DirectWrite）的字型框 ascent／descent 取自 OS/2 表：
// 有 USE_TYPO_METRICS（fsSelection 第 7 位）用 sTypoAscender／sTypoDescender，否則用 usWinAscent／usWinDescent。
// FreeType 這邊的 SkFontMetrics 是 hhea，兩者不同的字型（Noto Sans SC：hhea 1000/200、win 1160/288）
// measureText 的 fontBoundingBox 就會跟 Chrome 不同。沒有 OS/2 表時回傳 false，照用 hhea
bool dwriteVerticalMetrics(SkTypeface* typeface, float size, float* ascent, float* descent,
                           float* typo_descent = nullptr) {
    if (typeface == nullptr) return false;
    const SkFontTableTag tag = SkSetFourByteTag('O', 'S', '/', '2');
    uint8_t os2[78];
    if (typeface->getTableSize(tag) < sizeof(os2)) return false;
    if (typeface->getTableData(tag, 0, sizeof(os2), os2) != sizeof(os2)) return false;
    const int upm = typeface->getUnitsPerEm();
    if (upm <= 0) return false;
    auto u16 = [&](size_t at) { return static_cast<uint16_t>((os2[at] << 8) | os2[at + 1]); };
    auto s16 = [&](size_t at) { return static_cast<int16_t>(u16(at)); };
    float a, d;
    if (u16(62) & 0x80) {
        a = s16(68);
        d = -s16(70);
    } else {
        a = u16(74);
        d = u16(76);
    }
    if (a + d <= 0) return false;
    *ascent = a * size / upm;
    *descent = d * size / upm;
    if (typo_descent != nullptr) *typo_descent = -s16(70) * size / upm;
    return true;
}

SkFont makeCanvasFont(SkTypeface* typeface, float size) {
    SkFont font(sk_ref_sp(typeface), size);
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setSubpixel(true);
    font.setHinting(SkFontHinting::kNone);
    font.setLinearMetrics(true);
    return font;
}

// CSS shadowBlur is twice the Gaussian sigma Skia takes.
constexpr float kBlurToSigma = 0.5f;

// The gradient factories take SkColor4f spans; the C API speaks 32-bit ARGB.
std::vector<SkColor4f> toColor4f(const uint32_t* colors, int32_t count) {
    std::vector<SkColor4f> out;
    out.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        out.push_back(SkColor4f::FromColor(colors[i]));
    }
    return out;
}

SkMatrix canvasMatrix(float a, float b, float c, float d, float e, float f) {
    // Canvas order (a b c d e f) is column-major: x' = a*x + c*y + e.
    return SkMatrix::MakeAll(a, c, e, b, d, f, 0, 0, 1);
}

// Wraps the stock blob builder to also collect what canvas text metrics need.
//
// SkTextBlobBuilderRunHandler keeps the horizontal pen position private and
// its endPoint() only advances vertically, so the total advance has to be
// summed here. SkTextBlob::bounds() is likewise only a conservative box built
// from font-wide extents — measureText's actualBoundingBox* is specified as
// the tight ink extent, so per-run glyph bounds are accumulated instead.
class MetricsRunHandler final : public SkShaper::RunHandler {
public:
    MetricsRunHandler(const char* utf8, SkPoint offset) : fBlob(utf8, offset) {}

    void beginLine() override { fBlob.beginLine(); }
    void runInfo(const RunInfo& info) override { fBlob.runInfo(info); }
    void commitRunInfo() override { fBlob.commitRunInfo(); }

    Buffer runBuffer(const RunInfo& info) override {
        fBuffer = fBlob.runBuffer(info);
        fRunFont = info.fFont;
        fRunGlyphCount = static_cast<int>(info.glyphCount);
        fRunOrigin = fAdvance;
        return fBuffer;
    }

    void commitRunBuffer(const RunInfo& info) override {
        if (fRunGlyphCount > 0 && fBuffer.glyphs != nullptr && fBuffer.positions != nullptr) {
            std::vector<SkRect> bounds(static_cast<size_t>(fRunGlyphCount));
            fRunFont.getBounds(
                SkSpan(fBuffer.glyphs, fRunGlyphCount), SkSpan(bounds), nullptr);
            // 上下的墨跡範圍照 Windows 的 Chrome 用 hinting 過的字形（DirectWrite 垂直方向對齊像素：
            // 16px Arial 的 q 下緣 3.37px 是 3、泰文的 0.x 是 0），左右照舊用沒 hinting 的值
            SkFont hinted = fRunFont;
            hinted.setHinting(SkFontHinting::kNormal);
            std::vector<SkRect> vbounds(static_cast<size_t>(fRunGlyphCount));
            hinted.getBounds(
                SkSpan(fBuffer.glyphs, fRunGlyphCount), SkSpan(vbounds), nullptr);
            for (int i = 0; i < fRunGlyphCount; ++i) {
                SkRect glyph = bounds[static_cast<size_t>(i)];
                const SkRect& vg = vbounds[static_cast<size_t>(i)];
                if (!vg.isEmpty()) {
                    glyph.fTop = vg.fTop;
                    glyph.fBottom = vg.fBottom;
                }
                glyph.offset(fBuffer.positions[i].fX, fBuffer.positions[i].fY);
                if (glyph.isEmpty()) continue;
                if (fInk.isEmpty()) {
                    fInk = glyph;
                } else {
                    fInk.join(glyph);
                }
            }
        }
        fBlob.commitRunBuffer(info);
        fAdvance += info.fAdvance;
    }

    void commitLine() override { fBlob.commitLine(); }

    sk_sp<SkTextBlob> makeBlob() { return fBlob.makeBlob(); }
    SkPoint advance() const { return fAdvance; }
    const SkRect& ink() const { return fInk; }

private:
    SkTextBlobBuilderRunHandler fBlob;
    Buffer fBuffer{};
    SkFont fRunFont;
    int fRunGlyphCount = 0;
    SkPoint fRunOrigin{0, 0};
    SkPoint fAdvance{0, 0};
    SkRect fInk = SkRect::MakeEmpty();
};

// Shapes a run and hands the resulting blob back, along with the advance the
// shaper accumulated. Returns null for empty or unshapeable input.
sk_sp<SkTextBlob> shapeRun(
    ShaperBundle* bundle,
    const char* utf8,
    size_t utf8_len,
    SkTypeface* typeface,
    float size,
    SkPoint* out_end,
    SkRect* out_ink) {
    if (bundle == nullptr || utf8 == nullptr || utf8_len == 0) return nullptr;

    const SkFont font = makeCanvasFont(typeface, size);

    // The font-mgr iterator is what gives per-character fallback: a codepoint
    // the requested face lacks starts a new run in whichever family does have
    // it, which is how a browser renders emoji inside a Latin string.
    auto font_runs = SkShaper::MakeFontMgrRunIterator(utf8, utf8_len, font, bundle->fallback);
    if (!font_runs) return nullptr;

    auto bidi_runs = SkShapers::unicode::BidiRunIterator(bundle->unicode, utf8, utf8_len, 0);
    auto script_runs = SkShapers::HB::ScriptRunIterator(utf8, utf8_len);
    auto language_runs = SkShaper::MakeStdLanguageRunIterator(utf8, utf8_len);
    if (!bidi_runs || !script_runs || !language_runs) return nullptr;

    MetricsRunHandler handler(utf8, SkPoint{0, 0});
    bundle->shaper->shape(
        utf8,
        utf8_len,
        *font_runs,
        *bidi_runs,
        *script_runs,
        *language_runs,
        nullptr,
        0,
        std::numeric_limits<SkScalar>::max(),
        &handler);

    if (out_end != nullptr) *out_end = handler.advance();
    if (out_ink != nullptr) *out_ink = handler.ink();
    return handler.makeBlob();
}

} // namespace

// --- surface -----------------------------------------------------------

skiac_surface* skiac_surface_create_rgba(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return nullptr;

    // kRGBA_8888 premultiplied in sRGB matches a browser 2D canvas backing
    // store, so readPixels/putImageData need no colour conversion.
    const SkImageInfo info = SkImageInfo::Make(
        width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());

    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface) return nullptr;

    // A fresh canvas is transparent black.
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    return reinterpret_cast<skiac_surface*>(surface.release());
}

void skiac_surface_destroy(skiac_surface* surface) {
    SkSafeUnref(asSurface(surface));
}

skiac_canvas* skiac_surface_get_canvas(skiac_surface* surface) {
    if (surface == nullptr) return nullptr;
    return reinterpret_cast<skiac_canvas*>(asSurface(surface)->getCanvas());
}

bool skiac_surface_read_pixels(
    skiac_surface* surface,
    uint8_t* destination,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height) {
    if (surface == nullptr || destination == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    // Unpremultiplied: ImageData exposes straight alpha.
    const SkImageInfo info = SkImageInfo::Make(
        width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB());
    return asSurface(surface)->readPixels(
        info, destination, static_cast<size_t>(width) * 4, x, y);
}

bool skiac_surface_write_pixels(
    skiac_surface* surface,
    const uint8_t* source,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height) {
    if (surface == nullptr || source == nullptr || width <= 0 || height <= 0) return false;

    const SkImageInfo info = SkImageInfo::Make(
        width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB());
    SkPixmap pixmap(info, source, static_cast<size_t>(width) * 4);
    asSurface(surface)->writePixels(pixmap, x, y);
    return true;
}

skiac_data* skiac_surface_encode_png(skiac_surface* surface) {
    SkPixmap pixmap;
    if (!peekSurfacePixels(asSurface(surface), &pixmap)) return nullptr;
    return releaseData(SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{}));
}

skiac_data* skiac_surface_encode_jpeg(skiac_surface* surface, int32_t quality) {
    SkPixmap pixmap;
    if (!peekSurfacePixels(asSurface(surface), &pixmap)) return nullptr;
    SkJpegEncoder::Options options;
    options.fQuality = quality;
    return releaseData(SkJpegEncoder::Encode(pixmap, options));
}

skiac_data* skiac_surface_encode_webp(skiac_surface* surface, int32_t quality) {
    SkPixmap pixmap;
    if (!peekSurfacePixels(asSurface(surface), &pixmap)) return nullptr;
    SkWebpEncoder::Options options;
    options.fQuality = static_cast<float>(quality);
    return releaseData(SkWebpEncoder::Encode(pixmap, options));
}

void skiac_canvas_draw_surface_rect(
    skiac_canvas* canvas,
    skiac_surface* source,
    float src_x,
    float src_y,
    float src_width,
    float src_height,
    float dst_x,
    float dst_y,
    float dst_width,
    float dst_height,
    skiac_paint* paint) {
    if (canvas == nullptr || source == nullptr) return;

    sk_sp<SkImage> image = asSurface(source)->makeImageSnapshot();
    if (!image) return;

    asCanvas(canvas)->drawImageRect(
        image,
        SkRect::MakeXYWH(src_x, src_y, src_width, src_height),
        SkRect::MakeXYWH(dst_x, dst_y, dst_width, dst_height),
        SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
        paint == nullptr ? nullptr : asPaint(paint),
        SkCanvas::kStrict_SrcRectConstraint);
}

// --- data --------------------------------------------------------------

const uint8_t* skiac_data_bytes(skiac_data* data) {
    if (data == nullptr) return nullptr;
    return asData(data)->bytes();
}

size_t skiac_data_size(skiac_data* data) {
    if (data == nullptr) return 0;
    return asData(data)->size();
}

void skiac_data_destroy(skiac_data* data) {
    SkSafeUnref(asData(data));
}

// --- paint -------------------------------------------------------------

skiac_paint* skiac_paint_create(void) {
    SkPaint* paint = new SkPaint();
    // Canvas 2D antialiases everything by default.
    paint->setAntiAlias(true);
    return reinterpret_cast<skiac_paint*>(paint);
}

void skiac_paint_destroy(skiac_paint* paint) {
    delete asPaint(paint);
}

void skiac_paint_set_color(skiac_paint* paint, uint32_t argb) {
    if (paint == nullptr) return;
    asPaint(paint)->setColor(argb);
}

void skiac_paint_set_alpha(skiac_paint* paint, float alpha) {
    if (paint == nullptr) return;
    asPaint(paint)->setAlphaf(alpha);
}

void skiac_paint_set_anti_alias(skiac_paint* paint, bool anti_alias) {
    if (paint == nullptr) return;
    asPaint(paint)->setAntiAlias(anti_alias);
}

void skiac_paint_set_stroke(
    skiac_paint* paint,
    bool stroke,
    float width,
    int32_t cap,
    int32_t join,
    float miter_limit) {
    if (paint == nullptr) return;
    SkPaint* p = asPaint(paint);
    p->setStyle(stroke ? SkPaint::kStroke_Style : SkPaint::kFill_Style);
    p->setStrokeWidth(width);
    p->setStrokeCap(static_cast<SkPaint::Cap>(
        cap >= 0 && cap <= SkPaint::kLast_Cap ? cap : SkPaint::kButt_Cap));
    p->setStrokeJoin(static_cast<SkPaint::Join>(
        join >= 0 && join <= SkPaint::kLast_Join ? join : SkPaint::kMiter_Join));
    p->setStrokeMiter(miter_limit);
}

void skiac_paint_set_blend_mode(skiac_paint* paint, int32_t blend_mode) {
    if (paint == nullptr) return;
    if (blend_mode < 0 || blend_mode > static_cast<int32_t>(SkBlendMode::kLastMode)) return;
    asPaint(paint)->setBlendMode(static_cast<SkBlendMode>(blend_mode));
}

void skiac_paint_set_shader(skiac_paint* paint, skiac_shader* shader) {
    if (paint == nullptr) return;
    asPaint(paint)->setShader(shader == nullptr ? nullptr : sk_ref_sp(asShader(shader)));
}

void skiac_paint_set_effects(
    skiac_paint* paint,
    const float* color_matrix,
    float blur_sigma,
    float shadow_offset_x,
    float shadow_offset_y,
    float shadow_blur,
    uint32_t shadow_argb) {
    if (paint == nullptr) return;

    sk_sp<SkImageFilter> filter;

    // CSS `filter` runs over the element's own rendering first...
    if (color_matrix != nullptr) {
        filter = SkImageFilters::ColorFilter(
            SkColorFilters::Matrix(color_matrix), std::move(filter));
    }
    if (blur_sigma > 0) {
        filter = SkImageFilters::Blur(blur_sigma, blur_sigma, std::move(filter));
    }

    // ...and the canvas shadow is cast by the filtered result. A fully
    // transparent colour, or a zero blur with no offset, means no shadow.
    const bool has_shadow = SkColorGetA(shadow_argb) != 0 &&
        (shadow_blur > 0 || shadow_offset_x != 0 || shadow_offset_y != 0);
    if (has_shadow) {
        const float sigma = shadow_blur * kBlurToSigma;
        filter = SkImageFilters::DropShadow(
            shadow_offset_x, shadow_offset_y, sigma, sigma, shadow_argb, std::move(filter));
    }

    asPaint(paint)->setImageFilter(std::move(filter));
}

// --- shader ------------------------------------------------------------

skiac_shader* skiac_shader_linear_gradient(
    float x0,
    float y0,
    float x1,
    float y1,
    const uint32_t* colors,
    const float* positions,
    int32_t count) {
    if (colors == nullptr || positions == nullptr || count <= 0) return nullptr;

    std::vector<SkColor4f> stops = toColor4f(colors, count);
    const SkGradient gradient(
        SkGradient::Colors(
            SkSpan(stops), SkSpan(positions, static_cast<size_t>(count)), SkTileMode::kClamp),
        SkGradient::Interpolation{});

    const SkPoint points[2] = {{x0, y0}, {x1, y1}};
    sk_sp<SkShader> shader = SkShaders::LinearGradient(points, gradient);
    if (!shader) return nullptr;
    return reinterpret_cast<skiac_shader*>(shader.release());
}

skiac_shader* skiac_shader_radial_gradient(
    float x0,
    float y0,
    float r0,
    float x1,
    float y1,
    float r1,
    const uint32_t* colors,
    const float* positions,
    int32_t count) {
    if (colors == nullptr || positions == nullptr || count <= 0) return nullptr;

    std::vector<SkColor4f> stops = toColor4f(colors, count);
    const SkGradient gradient(
        SkGradient::Colors(
            SkSpan(stops), SkSpan(positions, static_cast<size_t>(count)), SkTileMode::kClamp),
        SkGradient::Interpolation{});

    // Canvas createRadialGradient is specified as a two-point conical
    // gradient, not a plain radial one.
    sk_sp<SkShader> shader = SkShaders::TwoPointConicalGradient(
        SkPoint{x0, y0}, r0, SkPoint{x1, y1}, r1, gradient);
    if (!shader) return nullptr;
    return reinterpret_cast<skiac_shader*>(shader.release());
}

void skiac_shader_destroy(skiac_shader* shader) {
    SkSafeUnref(asShader(shader));
}

// --- path --------------------------------------------------------------

// SkPath is immutable, so a path handle is really a builder; drawing snapshots
// it. Canvas keeps one mutable "current path" that is appended to across
// calls, which is exactly the builder's model.
skiac_path* skiac_path_create(void) {
    return reinterpret_cast<skiac_path*>(new SkPathBuilder());
}

void skiac_path_destroy(skiac_path* path) {
    delete asPath(path);
}

void skiac_path_reset(skiac_path* path) {
    if (path == nullptr) return;
    asPath(path)->reset();
}

bool skiac_path_is_empty(skiac_path* path) {
    return path == nullptr || asPath(path)->isEmpty();
}

void skiac_path_set_fill_type(skiac_path* path, int32_t fill_type) {
    if (path == nullptr) return;
    asPath(path)->setFillType(fill_type == 1 ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding);
}

void skiac_path_move_to(skiac_path* path, float x, float y) {
    if (path == nullptr) return;
    asPath(path)->moveTo(x, y);
}

void skiac_path_line_to(skiac_path* path, float x, float y) {
    if (path == nullptr) return;
    // Per spec a lineTo with no current point behaves as a moveTo.
    if (asPath(path)->isEmpty()) {
        asPath(path)->moveTo(x, y);
        return;
    }
    asPath(path)->lineTo(x, y);
}

void skiac_path_quad_to(skiac_path* path, float cx, float cy, float x, float y) {
    if (path == nullptr) return;
    if (asPath(path)->isEmpty()) asPath(path)->moveTo(cx, cy);
    asPath(path)->quadTo(cx, cy, x, y);
}

void skiac_path_cubic_to(
    skiac_path* path,
    float c1x,
    float c1y,
    float c2x,
    float c2y,
    float x,
    float y) {
    if (path == nullptr) return;
    if (asPath(path)->isEmpty()) asPath(path)->moveTo(c1x, c1y);
    asPath(path)->cubicTo(c1x, c1y, c2x, c2y, x, y);
}

void skiac_path_arc_to_tangent(
    skiac_path* path,
    float x1,
    float y1,
    float x2,
    float y2,
    float radius) {
    if (path == nullptr) return;
    if (asPath(path)->isEmpty()) asPath(path)->moveTo(x1, y1);
    asPath(path)->arcTo(SkPoint{x1, y1}, SkPoint{x2, y2}, radius);
}

void skiac_path_ellipse(
    skiac_path* path,
    float x,
    float y,
    float radius_x,
    float radius_y,
    float rotation,
    float start_angle,
    float end_angle,
    bool anticlockwise) {
    if (path == nullptr || radius_x < 0 || radius_y < 0) return;

    // Skia's arcTo works on an axis-aligned oval in degrees, so the rotation
    // is applied by transforming the arc rather than the oval.
    const float start_degrees = SkRadiansToDegrees(start_angle);
    const float delta = end_angle - start_angle;
    float sweep_degrees;

    if (SkScalarAbs(delta) >= SK_ScalarPI * 2) {
        // A span of a full turn or more is the whole ellipse whichever way it
        // is travelled — `arc(x, y, r, 0, 2*PI, true)` is a circle, not the
        // empty sweep that normalising the angles into (-360, 0] would give.
        sweep_degrees = anticlockwise ? -360.0f : 360.0f;
    } else {
        sweep_degrees = SkRadiansToDegrees(delta);
        if (anticlockwise) {
            if (sweep_degrees > 0) sweep_degrees -= 360;
        } else {
            if (sweep_degrees < 0) sweep_degrees += 360;
        }
    }

    const SkRect oval = SkRect::MakeLTRB(-radius_x, -radius_y, radius_x, radius_y);

    SkPathBuilder arc;
    // A full turn has to be emitted as two halves: arcTo collapses a 360°
    // sweep to nothing.
    if (SkScalarNearlyEqual(SkScalarAbs(sweep_degrees), 360.0f)) {
        const float half = sweep_degrees / 2;
        arc.arcTo(oval, start_degrees, half, true);
        arc.arcTo(oval, start_degrees + half, half, false);
    } else {
        arc.arcTo(oval, start_degrees, sweep_degrees, true);
    }

    SkMatrix transform = SkMatrix::RotateRad(rotation);
    transform.postTranslate(x, y);

    // Extend rather than append, so the arc joins the existing subpath with a
    // straight line the way canvas specifies.
    asPath(path)->addPath(arc.snapshot(&transform), SkPath::kExtend_AddPathMode);
}

void skiac_path_rect(skiac_path* path, float x, float y, float width, float height) {
    if (path == nullptr) return;
    asPath(path)->addRect(SkRect::MakeXYWH(x, y, width, height));
}

void skiac_path_close(skiac_path* path) {
    if (path == nullptr || asPath(path)->isEmpty()) return;
    asPath(path)->close();
}

// --- canvas state ------------------------------------------------------

void skiac_canvas_save(skiac_canvas* canvas) {
    if (canvas == nullptr) return;
    asCanvas(canvas)->save();
}

void skiac_canvas_restore(skiac_canvas* canvas) {
    if (canvas == nullptr) return;
    asCanvas(canvas)->restore();
}

void skiac_canvas_translate(skiac_canvas* canvas, float dx, float dy) {
    if (canvas == nullptr) return;
    asCanvas(canvas)->translate(dx, dy);
}

void skiac_canvas_scale(skiac_canvas* canvas, float sx, float sy) {
    if (canvas == nullptr) return;
    asCanvas(canvas)->scale(sx, sy);
}

void skiac_canvas_rotate(skiac_canvas* canvas, float radians) {
    if (canvas == nullptr) return;
    asCanvas(canvas)->rotate(SkRadiansToDegrees(radians));
}

void skiac_canvas_concat(
    skiac_canvas* canvas,
    float a,
    float b,
    float c,
    float d,
    float e,
    float f) {
    if (canvas == nullptr) return;
    asCanvas(canvas)->concat(canvasMatrix(a, b, c, d, e, f));
}

void skiac_canvas_set_matrix(
    skiac_canvas* canvas,
    float a,
    float b,
    float c,
    float d,
    float e,
    float f) {
    if (canvas == nullptr) return;
    asCanvas(canvas)->setMatrix(canvasMatrix(a, b, c, d, e, f));
}

void skiac_canvas_reset_matrix(skiac_canvas* canvas) {
    if (canvas == nullptr) return;
    asCanvas(canvas)->resetMatrix();
}

void skiac_canvas_get_matrix(skiac_canvas* canvas, float* out_six) {
    if (canvas == nullptr || out_six == nullptr) return;
    const SkMatrix matrix = asCanvas(canvas)->getTotalMatrix();
    out_six[0] = matrix.getScaleX();
    out_six[1] = matrix.getSkewY();
    out_six[2] = matrix.getSkewX();
    out_six[3] = matrix.getScaleY();
    out_six[4] = matrix.getTranslateX();
    out_six[5] = matrix.getTranslateY();
}

void skiac_canvas_clip_path(skiac_canvas* canvas, skiac_path* path, bool evenodd) {
    if (canvas == nullptr || path == nullptr) return;
    SkPathBuilder clip = *asPath(path);
    clip.setFillType(evenodd ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding);
    asCanvas(canvas)->clipPath(clip.snapshot(), SkClipOp::kIntersect, true);
}

// --- canvas drawing ----------------------------------------------------

void skiac_canvas_clear(skiac_canvas* canvas, uint32_t argb) {
    if (canvas == nullptr) return;
    asCanvas(canvas)->clear(argb);
}

void skiac_canvas_draw_rect(
    skiac_canvas* canvas,
    float x,
    float y,
    float width,
    float height,
    skiac_paint* paint) {
    if (canvas == nullptr || paint == nullptr) return;
    asCanvas(canvas)->drawRect(SkRect::MakeXYWH(x, y, width, height), *asPaint(paint));
}

void skiac_canvas_draw_path(skiac_canvas* canvas, skiac_path* path, skiac_paint* paint) {
    if (canvas == nullptr || path == nullptr || paint == nullptr) return;
    asCanvas(canvas)->drawPath(asPath(path)->snapshot(), *asPaint(paint));
}

// --- fonts -------------------------------------------------------------

skiac_fontmgr* skiac_fontmgr_create_from_dir(const char* directory) {
    if (directory == nullptr) return nullptr;
    sk_sp<SkFontMgr> manager = SkFontMgr_New_Custom_Directory(directory);
    if (!manager) return nullptr;
    return reinterpret_cast<skiac_fontmgr*>(manager.release());
}

void skiac_fontmgr_destroy(skiac_fontmgr* manager) {
    SkSafeUnref(asFontMgr(manager));
}

int32_t skiac_fontmgr_count_families(skiac_fontmgr* manager) {
    if (manager == nullptr) return 0;
    return asFontMgr(manager)->countFamilies();
}

int32_t skiac_fontmgr_get_family_name(
    skiac_fontmgr* manager,
    int32_t index,
    char* buffer,
    int32_t buffer_len) {
    if (manager == nullptr || buffer == nullptr || buffer_len <= 0) return -1;
    if (index < 0 || index >= asFontMgr(manager)->countFamilies()) return -1;

    SkString name;
    asFontMgr(manager)->getFamilyName(index, &name);
    const int32_t length = static_cast<int32_t>(name.size());
    if (length + 1 > buffer_len) return -1;

    std::memcpy(buffer, name.c_str(), static_cast<size_t>(length));
    buffer[length] = '\0';
    return length;
}

skiac_typeface* skiac_fontmgr_match_family(skiac_fontmgr* manager, const char* family) {
    return skiac_fontmgr_match_family_style(manager, family, 400, 5, 0);
}

skiac_typeface* skiac_fontmgr_match_family_style(
    skiac_fontmgr* manager,
    const char* family,
    int32_t weight,
    int32_t width,
    int32_t slant) {
    if (manager == nullptr || family == nullptr) return nullptr;

    const SkFontStyle style(weight, width, static_cast<SkFontStyle::Slant>(slant));
    sk_sp<SkTypeface> typeface = asFontMgr(manager)->matchFamilyStyle(family, style);
    if (!typeface) return nullptr;
    return reinterpret_cast<skiac_typeface*>(typeface.release());
}

void skiac_typeface_destroy(skiac_typeface* typeface) {
    SkSafeUnref(asTypeface(typeface));
}

void skiac_typeface_get_style(
    skiac_typeface* typeface,
    int32_t* weight,
    int32_t* width,
    int32_t* slant) {
    if (typeface == nullptr) return;
    const SkFontStyle style = asTypeface(typeface)->fontStyle();
    if (weight != nullptr) *weight = style.weight();
    if (width != nullptr) *width = style.width();
    if (slant != nullptr) *slant = static_cast<int32_t>(style.slant());
}

int32_t skiac_typeface_get_family_name(skiac_typeface* typeface, char* buffer, int32_t buffer_len) {
    if (typeface == nullptr || buffer == nullptr || buffer_len <= 0) return -1;

    SkString name;
    asTypeface(typeface)->getFamilyName(&name);
    const int32_t length = static_cast<int32_t>(name.size());
    if (length + 1 > buffer_len) return -1;

    std::memcpy(buffer, name.c_str(), static_cast<size_t>(length));
    buffer[length] = '\0';
    return length;
}

// --- text --------------------------------------------------------------

skiac_shaper* skiac_shaper_create(skiac_fontmgr* manager) {
    if (manager == nullptr) return nullptr;

    sk_sp<SkUnicode> unicode = SkUnicodes::ICU::Make();
    if (!unicode) return nullptr;

    sk_sp<SkFontMgr> fallback = sk_ref_sp(asFontMgr(manager));
    std::unique_ptr<SkShaper> shaper = SkShapers::HB::ShapeDontWrapOrReorder(unicode, fallback);
    if (!shaper) return nullptr;

    ShaperBundle* bundle = new ShaperBundle{
        std::move(fallback), std::move(unicode), std::move(shaper)};
    return reinterpret_cast<skiac_shaper*>(bundle);
}

void skiac_shaper_destroy(skiac_shaper* shaper) {
    delete asShaper(shaper);
}

void skiac_shaper_measure(
    skiac_shaper* shaper,
    const char* utf8,
    size_t utf8_len,
    skiac_typeface* typeface,
    float size,
    skiac_text_metrics* out) {
    if (out == nullptr) return;
    std::memset(out, 0, sizeof(*out));

    const SkFont font = makeCanvasFont(asTypeface(typeface), size);

    SkFontMetrics font_metrics;
    font.getMetrics(&font_metrics);
    // Skia reports ascent above the baseline as negative; the canvas API
    // reports the same distance as positive.
    // em_height_* 保留 hhea（shaper 排版座標用的 ascent，呼叫端靠它把墨跡範圍換回基線）；
    // font_bounding_box_* 與基線照 DirectWrite 的 OS/2 值（dwriteVerticalMetrics）
    // ideographic_baseline 是基線下方的距離（正值）：Blink 用 OS/2 的 sTypoDescender
    // （11-11 CF 錄製：16px sans-serif＝Noto Sans SC 的 ideographicBaseline 是 -1.92＝-120/1000×16）
    float box_ascent = -font_metrics.fAscent, box_descent = font_metrics.fDescent;
    float typo_descent = box_descent;
    dwriteVerticalMetrics(asTypeface(typeface), size, &box_ascent, &box_descent, &typo_descent);
    out->font_bounding_box_ascent = box_ascent;
    out->font_bounding_box_descent = box_descent;
    out->em_height_ascent = -font_metrics.fAscent;
    out->em_height_descent = font_metrics.fDescent;
    out->alphabetic_baseline = 0;
    out->hanging_baseline = box_ascent * 0.8f;
    out->ideographic_baseline = typo_descent;

    SkPoint end{0, 0};
    SkRect ink = SkRect::MakeEmpty();
    sk_sp<SkTextBlob> blob =
        shapeRun(asShaper(shaper), utf8, utf8_len, asTypeface(typeface), size, &end, &ink);
    if (!blob) return;

    out->width = end.fX;
    if (ink.isEmpty()) return;

    // Canvas reports distances outward from the anchor, so left and ascent
    // flip sign relative to Skia's rect.
    out->actual_bounding_box_left = -ink.fLeft;
    out->actual_bounding_box_right = ink.fRight;
    out->actual_bounding_box_ascent = -ink.fTop;
    out->actual_bounding_box_descent = ink.fBottom;
}

void skiac_shaper_draw(
    skiac_shaper* shaper,
    skiac_canvas* canvas,
    const char* utf8,
    size_t utf8_len,
    float x,
    float y,
    skiac_typeface* typeface,
    float size,
    int32_t align,
    int32_t baseline,
    skiac_paint* paint) {
    if (canvas == nullptr || paint == nullptr) return;

    SkPoint end{0, 0};
    sk_sp<SkTextBlob> blob =
        shapeRun(asShaper(shaper), utf8, utf8_len, asTypeface(typeface), size, &end, nullptr);
    if (!blob) return;

    const SkFont font = makeCanvasFont(asTypeface(typeface), size);
    SkFontMetrics metrics;
    font.getMetrics(&metrics);

    float dx = 0;
    switch (align) {
        case 1: dx = -end.fX; break;      // end / right
        case 2: dx = -end.fX / 2; break;  // center
        default: break;                   // start / left
    }

    float dy = 0;
    switch (baseline) {
        case 1: dy = -metrics.fAscent; break;                       // top
        case 2: dy = -metrics.fAscent * 0.8f; break;                // hanging
        case 3: dy = (-metrics.fAscent - metrics.fDescent) / 2; break; // middle
        case 4: dy = -metrics.fDescent; break;                      // ideographic
        case 5: dy = -metrics.fDescent; break;                      // bottom
        default: break;                                             // alphabetic
    }

    asCanvas(canvas)->drawTextBlob(blob, x + dx, y + dy, *asPaint(paint));
}

// --- 擴充 ------------------------------------------------------------------

namespace {

// canvas 規格：虛線清單是奇數個時重複一次；有負數、非有限值或全為 0 時不畫虛線（由呼叫端過濾）
sk_sp<SkPathEffect> makeDash(const float* intervals, int32_t count, float phase) {
    if (intervals == nullptr || count <= 0) return nullptr;
    std::vector<SkScalar> list(intervals, intervals + count);
    if (list.size() % 2 == 1) list.insert(list.end(), intervals, intervals + count);
    return SkDashPathEffect::Make(SkSpan<const SkScalar>(list.data(), list.size()), phase);
}

SkSamplingOptions samplingFor(int32_t sampling) {
    switch (sampling) {
        case 0: return SkSamplingOptions(SkFilterMode::kNearest, SkMipmapMode::kNone);
        case 2: return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNearest);
        case 3: return SkSamplingOptions(SkCubicResampler::Mitchell());
        default: return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone);
    }
}

} // namespace

bool skiac_path_contains(skiac_path* path, float x, float y, int32_t fill_type) {
    if (path == nullptr) return false;
    SkPathBuilder copy = *asPath(path);
    copy.setFillType(fill_type == 1 ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding);
    return copy.snapshot().contains(x, y);
}

bool skiac_path_stroke_contains(
    skiac_path* path,
    float x,
    float y,
    float width,
    int32_t cap,
    int32_t join,
    float miter_limit,
    const float* dashes,
    int32_t dash_count,
    float dash_phase) {
    if (path == nullptr) return false;
    SkPaint paint;
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(width);
    paint.setStrokeCap(static_cast<SkPaint::Cap>(
        cap >= 0 && cap <= SkPaint::kLast_Cap ? cap : SkPaint::kButt_Cap));
    paint.setStrokeJoin(static_cast<SkPaint::Join>(
        join >= 0 && join <= SkPaint::kLast_Join ? join : SkPaint::kMiter_Join));
    paint.setStrokeMiter(miter_limit);
    paint.setPathEffect(makeDash(dashes, dash_count, dash_phase));
    SkPath outline = skpathutils::FillPathWithPaint(asPath(path)->snapshot(), paint);
    outline.setFillType(SkPathFillType::kWinding);
    return outline.contains(x, y);
}

void skiac_paint_set_dash(skiac_paint* paint, const float* intervals, int32_t count, float phase) {
    if (paint == nullptr) return;
    asPaint(paint)->setPathEffect(makeDash(intervals, count, phase));
}

skiac_shader* skiac_shader_conic_gradient(
    float cx,
    float cy,
    float start_radians,
    const uint32_t* colors,
    const float* positions,
    int32_t count) {
    if (colors == nullptr || positions == nullptr || count <= 0) return nullptr;
    std::vector<SkColor4f> stops = toColor4f(colors, count);
    const SkGradient gradient(
        SkGradient::Colors(
            SkSpan(stops), SkSpan(positions, static_cast<size_t>(count)), SkTileMode::kClamp),
        SkGradient::Interpolation{});
    // Blink：CanvasGradient 的 conic 角度是 Rad2deg(startAngle) + 90，產生 shader 時再減 90（Skia 從 x 軸起算）
    SkMatrix local = SkMatrix::RotateDeg(SkRadiansToDegrees(start_radians), SkPoint{cx, cy});
    sk_sp<SkShader> shader = SkShaders::SweepGradient(SkPoint{cx, cy}, 0, 360, gradient, &local);
    if (!shader) return nullptr;
    return reinterpret_cast<skiac_shader*>(shader.release());
}

skiac_shader* skiac_shader_surface_pattern(
    skiac_surface* source,
    bool repeat_x,
    bool repeat_y,
    int32_t sampling,
    const float* matrix_six) {
    if (source == nullptr) return nullptr;
    sk_sp<SkImage> image = asSurface(source)->makeImageSnapshot();
    if (!image) return nullptr;
    SkMatrix local = SkMatrix::I();
    if (matrix_six != nullptr) {
        local = canvasMatrix(matrix_six[0], matrix_six[1], matrix_six[2], matrix_six[3], matrix_six[4], matrix_six[5]);
    }
    sk_sp<SkShader> shader = image->makeShader(
        repeat_x ? SkTileMode::kRepeat : SkTileMode::kDecal,
        repeat_y ? SkTileMode::kRepeat : SkTileMode::kDecal,
        samplingFor(sampling),
        &local);
    if (!shader) return nullptr;
    return reinterpret_cast<skiac_shader*>(shader.release());
}

void skiac_canvas_draw_surface_rect_sampled(
    skiac_canvas* canvas,
    skiac_surface* source,
    float src_x,
    float src_y,
    float src_width,
    float src_height,
    float dst_x,
    float dst_y,
    float dst_width,
    float dst_height,
    int32_t sampling,
    skiac_paint* paint) {
    if (canvas == nullptr || source == nullptr) return;
    sk_sp<SkImage> image = asSurface(source)->makeImageSnapshot();
    if (!image) return;
    asCanvas(canvas)->drawImageRect(
        image,
        SkRect::MakeXYWH(src_x, src_y, src_width, src_height),
        SkRect::MakeXYWH(dst_x, dst_y, dst_width, dst_height),
        samplingFor(sampling),
        paint == nullptr ? nullptr : asPaint(paint),
        SkCanvas::kStrict_SrcRectConstraint);
}

void skiac_canvas_save_layer(skiac_canvas* canvas, skiac_paint* paint) {
    if (canvas == nullptr) return;
    asCanvas(canvas)->saveLayer(nullptr, paint == nullptr ? nullptr : asPaint(paint));
}

void skiac_path_add_arc(
    skiac_path* path,
    float cx,
    float cy,
    float radius_x,
    float radius_y,
    float rotation,
    float start_angle,
    float sweep,
    const float* matrix_six) {
    if (path == nullptr || radius_x < 0 || radius_y < 0) return;
    const SkRect oval = SkRect::MakeLTRB(-radius_x, -radius_y, radius_x, radius_y);
    const SkScalar start_degrees = SkRadiansToDegrees(start_angle);
    const SkScalar sweep_degrees = SkRadiansToDegrees(sweep);
    SkPathBuilder arc;
    if (SkScalarNearlyEqual(sweep_degrees, 360)) {
        arc.arcTo(oval, start_degrees, 180, true);
        arc.arcTo(oval, start_degrees + 180, 180, false);
    } else if (SkScalarNearlyEqual(sweep_degrees, -360)) {
        arc.arcTo(oval, start_degrees, -180, true);
        arc.arcTo(oval, start_degrees - 180, -180, false);
    } else {
        arc.arcTo(oval, start_degrees, sweep_degrees, true);
    }
    SkMatrix local = SkMatrix::RotateRad(rotation);
    local.postTranslate(cx, cy);
    if (matrix_six != nullptr) {
        local.postConcat(canvasMatrix(matrix_six[0], matrix_six[1], matrix_six[2], matrix_six[3], matrix_six[4], matrix_six[5]));
    }
    // kExtend：用直線接上目前的子路徑（canvas 規格）；沒有目前點時就是新的子路徑
    asPath(path)->addPath(arc.snapshot(&local), SkPath::kExtend_AddPathMode);
}
