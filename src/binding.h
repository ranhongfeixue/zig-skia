// Pure-C surface over Skia, consumed from Zig via translate-c.
//
// Nothing in this header may reference C++ types: it is parsed by translate-c.
// Every handle is an opaque pointer whose lifetime is explicit — each
// `*_create*` has a matching `*_destroy`.
//
// Colours are 32-bit ARGB (0xAARRGGBB), the layout SkColor uses.

#ifndef SKIAC_BINDING_H
#define SKIAC_BINDING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct skiac_surface skiac_surface;
typedef struct skiac_canvas skiac_canvas;
typedef struct skiac_paint skiac_paint;
typedef struct skiac_path skiac_path;
typedef struct skiac_shader skiac_shader;
typedef struct skiac_data skiac_data;
typedef struct skiac_fontmgr skiac_fontmgr;
typedef struct skiac_typeface skiac_typeface;
typedef struct skiac_shaper skiac_shaper;

// --- surface -----------------------------------------------------------
// Always 8-bit RGBA, premultiplied, sRGB — the pixel format a 2D canvas
// backing store uses.
skiac_surface* skiac_surface_create_rgba(int32_t width, int32_t height);
void skiac_surface_destroy(skiac_surface* surface);
skiac_canvas* skiac_surface_get_canvas(skiac_surface* surface);

// Reads into a caller-owned buffer of width*height*4 bytes, unpremultiplied
// so it can be handed straight to ImageData.
bool skiac_surface_read_pixels(
    skiac_surface* surface,
    uint8_t* destination,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height);

// `source` is unpremultiplied RGBA, as ImageData stores it.
bool skiac_surface_write_pixels(
    skiac_surface* surface,
    const uint8_t* source,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height);

// Returned data is owned by the caller; release with skiac_data_destroy.
// Returns NULL on failure. `quality` is 0-100 and ignored by PNG.
skiac_data* skiac_surface_encode_png(skiac_surface* surface);
skiac_data* skiac_surface_encode_jpeg(skiac_surface* surface, int32_t quality);
skiac_data* skiac_surface_encode_webp(skiac_surface* surface, int32_t quality);

// Draws `source` into `surface`, mapping the src rect onto the dst rect.
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
    skiac_paint* paint);

// --- data --------------------------------------------------------------
const uint8_t* skiac_data_bytes(skiac_data* data);
size_t skiac_data_size(skiac_data* data);
void skiac_data_destroy(skiac_data* data);

// --- paint -------------------------------------------------------------
skiac_paint* skiac_paint_create(void);
void skiac_paint_destroy(skiac_paint* paint);
void skiac_paint_set_color(skiac_paint* paint, uint32_t argb);
void skiac_paint_set_alpha(skiac_paint* paint, float alpha);
void skiac_paint_set_anti_alias(skiac_paint* paint, bool anti_alias);

// `stroke` false selects fill. Cap: 0 butt, 1 round, 2 square.
// Join: 0 miter, 1 round, 2 bevel.
void skiac_paint_set_stroke(
    skiac_paint* paint,
    bool stroke,
    float width,
    int32_t cap,
    int32_t join,
    float miter_limit);

// SkBlendMode ordinal; the caller maps globalCompositeOperation onto it.
void skiac_paint_set_blend_mode(skiac_paint* paint, int32_t blend_mode);

// Passing NULL clears the shader and the paint falls back to its colour.
void skiac_paint_set_shader(skiac_paint* paint, skiac_shader* shader);

// Installs the paint's image-filter chain: the CSS `filter` property first,
// then the canvas shadow, matching the order a browser applies them.
//
// `color_matrix` is a 20-entry row-major matrix or NULL, `blur_sigma` is the
// CSS filter's blur (0 for none). `shadow_blur` is the CSS shadowBlur and is
// converted to a Gaussian sigma internally; a zero-alpha shadow colour, or a
// zero blur with no offset, means no shadow. Passing no effects at all clears
// any previously installed filter.
void skiac_paint_set_effects(
    skiac_paint* paint,
    const float* color_matrix,
    float blur_sigma,
    float shadow_offset_x,
    float shadow_offset_y,
    float shadow_blur,
    uint32_t shadow_argb);

// --- shader ------------------------------------------------------------
// `colors` and `positions` are parallel arrays of `count` entries.
skiac_shader* skiac_shader_linear_gradient(
    float x0,
    float y0,
    float x1,
    float y1,
    const uint32_t* colors,
    const float* positions,
    int32_t count);

// Two-point conical, which is what canvas createRadialGradient specifies.
skiac_shader* skiac_shader_radial_gradient(
    float x0,
    float y0,
    float r0,
    float x1,
    float y1,
    float r1,
    const uint32_t* colors,
    const float* positions,
    int32_t count);

void skiac_shader_destroy(skiac_shader* shader);

// --- path --------------------------------------------------------------
skiac_path* skiac_path_create(void);
void skiac_path_destroy(skiac_path* path);
void skiac_path_reset(skiac_path* path);
bool skiac_path_is_empty(skiac_path* path);
// 0 winding (nonzero), 1 even-odd.
void skiac_path_set_fill_type(skiac_path* path, int32_t fill_type);

void skiac_path_move_to(skiac_path* path, float x, float y);
void skiac_path_line_to(skiac_path* path, float x, float y);
void skiac_path_quad_to(skiac_path* path, float cx, float cy, float x, float y);
void skiac_path_cubic_to(
    skiac_path* path,
    float c1x,
    float c1y,
    float c2x,
    float c2y,
    float x,
    float y);
void skiac_path_arc_to_tangent(
    skiac_path* path,
    float x1,
    float y1,
    float x2,
    float y2,
    float radius);
void skiac_path_ellipse(
    skiac_path* path,
    float x,
    float y,
    float radius_x,
    float radius_y,
    float rotation,
    float start_angle,
    float end_angle,
    bool anticlockwise);
void skiac_path_rect(skiac_path* path, float x, float y, float width, float height);
void skiac_path_close(skiac_path* path);

// --- canvas state ------------------------------------------------------
void skiac_canvas_save(skiac_canvas* canvas);
void skiac_canvas_restore(skiac_canvas* canvas);
void skiac_canvas_translate(skiac_canvas* canvas, float dx, float dy);
void skiac_canvas_scale(skiac_canvas* canvas, float sx, float sy);
void skiac_canvas_rotate(skiac_canvas* canvas, float radians);
// Column-major CSS/canvas order: a b c d e f.
void skiac_canvas_concat(
    skiac_canvas* canvas,
    float a,
    float b,
    float c,
    float d,
    float e,
    float f);
void skiac_canvas_set_matrix(
    skiac_canvas* canvas,
    float a,
    float b,
    float c,
    float d,
    float e,
    float f);
void skiac_canvas_reset_matrix(skiac_canvas* canvas);
// Writes a b c d e f.
void skiac_canvas_get_matrix(skiac_canvas* canvas, float* out_six);

void skiac_canvas_clip_path(skiac_canvas* canvas, skiac_path* path, bool evenodd);

// --- canvas drawing ----------------------------------------------------
void skiac_canvas_clear(skiac_canvas* canvas, uint32_t argb);
void skiac_canvas_draw_rect(
    skiac_canvas* canvas,
    float x,
    float y,
    float width,
    float height,
    skiac_paint* paint);
void skiac_canvas_draw_path(skiac_canvas* canvas, skiac_path* path, skiac_paint* paint);

// --- fonts -------------------------------------------------------------
// Scans `directory` for font files and serves only those, so the font set is
// identical on every host regardless of what the OS has installed.
skiac_fontmgr* skiac_fontmgr_create_from_dir(const char* directory);
void skiac_fontmgr_destroy(skiac_fontmgr* manager);
int32_t skiac_fontmgr_count_families(skiac_fontmgr* manager);

// Writes a NUL-terminated family name into `buffer`; returns the name length,
// or -1 if the index is out of range or the buffer is too small.
int32_t skiac_fontmgr_get_family_name(
    skiac_fontmgr* manager,
    int32_t index,
    char* buffer,
    int32_t buffer_len);

// Returns NULL when the family is not present — the caller decides how to
// fall back, rather than silently substituting a different face.
skiac_typeface* skiac_fontmgr_match_family(skiac_fontmgr* manager, const char* family);

// `weight` and `width` are CSS numeric values (400/700, 5 = normal width);
// `slant` is 0 upright, 1 italic, 2 oblique.
skiac_typeface* skiac_fontmgr_match_family_style(
    skiac_fontmgr* manager,
    const char* family,
    int32_t weight,
    int32_t width,
    int32_t slant);

void skiac_typeface_destroy(skiac_typeface* typeface);

// Reports the style of the face actually selected. A request for weight 700
// that comes back as 400 means the family has no real bold and the renderer
// would have to synthesize one.
void skiac_typeface_get_style(
    skiac_typeface* typeface,
    int32_t* weight,
    int32_t* width,
    int32_t* slant);

int32_t skiac_typeface_get_family_name(skiac_typeface* typeface, char* buffer, int32_t buffer_len);

// --- text --------------------------------------------------------------
// A shaper carries the HarfBuzz/ICU machinery plus the font manager used for
// per-glyph fallback, so it is created once and reused.
skiac_shaper* skiac_shaper_create(skiac_fontmgr* manager);
void skiac_shaper_destroy(skiac_shaper* shaper);

// Metrics of a shaped run, in the same units measureText reports.
typedef struct skiac_text_metrics {
    float width;
    float actual_bounding_box_left;
    float actual_bounding_box_right;
    float actual_bounding_box_ascent;
    float actual_bounding_box_descent;
    float font_bounding_box_ascent;
    float font_bounding_box_descent;
    float em_height_ascent;
    float em_height_descent;
    float hanging_baseline;
    float alphabetic_baseline;
    float ideographic_baseline;
} skiac_text_metrics;

// Shapes `utf8` with HarfBuzz, resolving characters the primary typeface
// lacks through the font manager, and fills `out`.
void skiac_shaper_measure(
    skiac_shaper* shaper,
    const char* utf8,
    size_t utf8_len,
    skiac_typeface* typeface,
    float size,
    skiac_text_metrics* out);

// Draws at (x, y) on the alphabetic baseline. `align` is 0 start/left,
// 1 end/right, 2 center; `baseline` is 0 alphabetic, 1 top, 2 hanging,
// 3 middle, 4 ideographic, 5 bottom.
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
    skiac_paint* paint);

// --- 擴充：canvas 2D 其餘的功能 -------------------------------------------
// isPointInPath：路徑在 (x, y) 是否被填滿（fill_type 0 nonzero、1 evenodd）。
bool skiac_path_contains(skiac_path* path, float x, float y, int32_t fill_type);

// isPointInStroke：照筆畫參數（含虛線）把路徑展開成填滿的輪廓後判斷。
// `dashes` 可為 NULL（dash_count 為 0 時不套用虛線）。
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
    float dash_phase);

// setLineDash／lineDashOffset：`count` 為 0 時清除虛線。count 為奇數時照 canvas 規格重複一次。
void skiac_paint_set_dash(skiac_paint* paint, const float* intervals, int32_t count, float phase);

// createConicGradient：中心 (cx, cy)、從 x 軸起算 start_radians（順時針）一整圈。
skiac_shader* skiac_shader_conic_gradient(
    float cx,
    float cy,
    float start_radians,
    const uint32_t* colors,
    const float* positions,
    int32_t count);

// createPattern：以 `source` 目前的內容為圖樣。repeat_x／repeat_y 為 false 時該方向畫成透明（decal）。
// `sampling` 0 nearest、1 linear、2 linear＋mipmap、3 cubic；`matrix_six` 是圖樣的 setTransform（a b c d e f），可為 NULL。
skiac_shader* skiac_shader_surface_pattern(
    skiac_surface* source,
    bool repeat_x,
    bool repeat_y,
    int32_t sampling,
    const float* matrix_six);

// drawImage：跟 skiac_canvas_draw_surface_rect 一樣，但取樣方式照 imageSmoothingEnabled／imageSmoothingQuality。
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
    skiac_paint* paint);

// canvas 的 arc／ellipse 照 Blink 的 Path::AddEllipse：用 SkPath::arcTo（conic 曲線），整圈拆成兩個 180°；
// 弧在 (cx, cy) 為中心、旋轉 rotation 的座標系建立，再乘上 matrix_six（a b c d e f，可為 NULL）接在目前的子路徑後面。
void skiac_path_add_arc(
    skiac_path* path,
    float cx,
    float cy,
    float radius_x,
    float radius_y,
    float rotation,
    float start_angle,
    float sweep,
    const float* matrix_six);

// 整張畫布的圖層（Blink 的 full-canvas composite：source-in／copy 等會影響形狀外面的像素），
// restore 時用 `paint` 的合成模式、透明度與濾鏡合成回去。paint 可為 NULL。
void skiac_canvas_save_layer(skiac_canvas* canvas, skiac_paint* paint);

#ifdef __cplusplus
}
#endif

#endif // SKIAC_BINDING_H
