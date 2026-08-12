// Timeline-anchored annotations drawn by the user on top of the video.
//
// Design notes for the export milestone: every annotation stores its
// appearance and removal times in media seconds (start_pts / end_pts) and its
// geometry in *normalized* video coordinates (0..1 relative to the frame), so
// a later "bake into the exported video" pass can reproduce the exact overlay
// at any resolution without re-editing.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "external/stb/stb_truetype.h"

struct AnnoPoint {
  float x = 0.0f;  // normalized 0..1, relative to the video frame width
  float y = 0.0f;  // normalized 0..1, relative to the video frame height
};

// h/s/v in [0,1]; r/g/b out in [0,1].
inline void hsv_to_rgb(float h, float s, float v, float &r, float &g,
                       float &b) {
  float c = v * s;
  float hp = h * 6.0f;
  float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
  float m = v - c;
  if (hp < 1.0f) {
    r = c; g = x; b = 0.0f;
  } else if (hp < 2.0f) {
    r = x; g = c; b = 0.0f;
  } else if (hp < 3.0f) {
    r = 0.0f; g = c; b = x;
  } else if (hp < 4.0f) {
    r = 0.0f; g = x; b = c;
  } else if (hp < 5.0f) {
    r = x; g = 0.0f; b = c;
  } else {
    r = c; g = 0.0f; b = x;
  }
  r += m;
  g += m;
  b += m;
}

enum class AnnoShape : int { Rect = 0, Ellipse, Arrow, Freehand, Text };

struct Annotation {
  int id = 0;
  AnnoShape shape = AnnoShape::Rect;
  float color[4] = {1.0f, 0.3f, 0.3f, 1.0f};
  float width = 0.004f;  // stroke width, normalized to frame width
  std::vector<AnnoPoint> pts;  // 2 pts for rect/ellipse/arrow; polyline for freehand
  std::string text;            // content for Text annotations

  double start_pts = 0.0;  // media seconds when the annotation appeared
  double end_pts =         // media seconds when it was removed (inf = still live)
      std::numeric_limits<double>::infinity();

  bool visible_at(double t) const { return t >= start_pts && t < end_pts; }
};

// Ownership of the timeline of annotations.
struct AnnoStore {
  std::vector<Annotation> items;
  int next_id = 1;

  int add(Annotation a) {
    a.id = next_id++;
    items.push_back(std::move(a));
    return a.id;
  }

  // Remove (from media time `t` onward) the annotation with this id.
  bool remove_at(double t, int id) {
    for (auto &a : items)
      if (a.id == id && std::isinf(a.end_pts)) {
        a.end_pts = t;
        return true;
      }
    return false;
  }

  int last_id() const { return items.empty() ? -1 : items.back().id; }

  // Topmost visible annotation that contains point `p`, or -1.
  int hit_test(AnnoPoint p, double t) const {
    constexpr float kTol = 0.015f;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
      const Annotation &a = *it;
      if (!a.visible_at(t) || a.pts.empty())
        continue;
      if (a.shape == AnnoShape::Freehand) {
        for (size_t i = 1; i < a.pts.size(); i++)
          if (seg_dist(p, a.pts[i - 1], a.pts[i]) < kTol)
            return a.id;
      } else if (a.shape == AnnoShape::Text) {
        constexpr float kTextW = 0.22f;
        constexpr float kTextH = 0.07f;
        if (p.x >= a.pts[0].x - 0.01f && p.x <= a.pts[0].x + kTextW &&
            p.y >= a.pts[0].y - 0.01f && p.y <= a.pts[0].y + kTextH)
          return a.id;
      } else {
        float x0 = std::min(a.pts[0].x, a.pts[1].x) - kTol;
        float x1 = std::max(a.pts[0].x, a.pts[1].x) + kTol;
        float y0 = std::min(a.pts[0].y, a.pts[1].y) - kTol;
        float y1 = std::max(a.pts[0].y, a.pts[1].y) + kTol;
        if (p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1)
          return a.id;
      }
    }
    return -1;
  }

 private:
  static float seg_dist(AnnoPoint p, AnnoPoint a, AnnoPoint b) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f
                  ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2,
                               0.0f, 1.0f)
                  : 0.0f;
    float cx = a.x + t * dx;
    float cy = a.y + t * dy;
    return std::sqrt((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy));
  }
};

// CPU rasterizer: draws annotations into an RGBA8 buffer covering the window.
// Overlay pixels are in "display space" with straight (non-premultiplied)
// alpha; the blend pass composites them over the rendered video.
struct AnnoRaster {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> buf;  // RGBA8, tightly packed, y-down

  // The aspect-corrected rectangle of the video within the window, in pixels
  // (y-down). Normalized annotation coordinates map into it.
  int vx = 0;
  int vy = 0;
  int vw = 0;
  int vh = 0;

  void resize(int ww, int hh) {
    if (ww == w && hh == h)
      return;
    w = ww;
    h = hh;
    buf.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
  }

  void clear() { std::fill(buf.begin(), buf.end(), 0); }

  void set_display_rect(int x, int y, int ww, int hh) {
    vx = x;
    vy = y;
    vw = ww;
    vh = hh;
  }

  float px(AnnoPoint p) const { return static_cast<float>(vx) + p.x * vw; }
  float py(AnnoPoint p) const { return static_cast<float>(vy) + p.y * vh; }
  float pw(float n) const { return n * vw; }  // normalized width -> pixels

  void draw_annotation(const Annotation &a) {
    switch (a.shape) {
      case AnnoShape::Rect: {
        float x0 = px(a.pts[0]), y0 = py(a.pts[0]);
        float x1 = px(a.pts[1]), y1 = py(a.pts[1]);
        float t = thick(a.width);
        stamp_segment(x0, y0, x1, y0, t, a.color);
        stamp_segment(x1, y0, x1, y1, t, a.color);
        stamp_segment(x1, y1, x0, y1, t, a.color);
        stamp_segment(x0, y1, x0, y0, t, a.color);
        break;
      }
      case AnnoShape::Ellipse: {
        float cx = (px(a.pts[0]) + px(a.pts[1])) * 0.5f;
        float cy = (py(a.pts[0]) + py(a.pts[1])) * 0.5f;
        float rx = std::fabs(px(a.pts[1]) - px(a.pts[0])) * 0.5f;
        float ry = std::fabs(py(a.pts[1]) - py(a.pts[0])) * 0.5f;
        float t = thick(a.width);
        float prev_x = cx + rx, prev_y = cy;
        constexpr int kSteps = 48;
        for (int i = 1; i <= kSteps; i++) {
          float ang = static_cast<float>(i) * 2.0f * 3.14159265f / kSteps;
          float x = cx + rx * std::cos(ang);
          float y = cy + ry * std::sin(ang);
          stamp_segment(prev_x, prev_y, x, y, t, a.color);
          prev_x = x;
          prev_y = y;
        }
        break;
      }
      case AnnoShape::Arrow: {
        float ax = px(a.pts[0]), ay = py(a.pts[0]);
        float bx = px(a.pts[1]), by = py(a.pts[1]);
        float t = thick(a.width);
        stamp_segment(ax, ay, bx, by, t, a.color);
        // Arrowhead at the tip.
        float dx = bx - ax, dy = by - ay;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1e-4f) {
          dx /= len;
          dy /= len;
          float head = std::max(10.0f, t * 4.0f);
          float hx = bx - dx * head, hy = by - dy * head;
          stamp_segment(bx, by, hx - dy * head * 0.6f, hy + dx * head * 0.6f, t,
                        a.color);
          stamp_segment(bx, by, hx + dy * head * 0.6f, hy - dx * head * 0.6f, t,
                        a.color);
        }
        break;
      }
      case AnnoShape::Freehand: {
        float t = thick(a.width);
        for (size_t i = 1; i < a.pts.size(); i++)
          stamp_segment(px(a.pts[i - 1]), py(a.pts[i - 1]), px(a.pts[i]),
                        py(a.pts[i]), t, a.color);
        break;
      }
      case AnnoShape::Text:
        draw_text(a.text, a.pts[0], a.color);
        break;
    }
  }

 private:
  float thick(float normalized_width) const {
    return std::max(2.0f, pw(normalized_width));
  }

  void blend_px(int x, int y, const float c[4], float cov) {
    if (cov <= 0.0f || x < 0 || y < 0 || x >= w || y >= h)
      return;
    size_t i = (static_cast<size_t>(y) * w + static_cast<size_t>(x)) * 4;
    float a = c[3] * cov;
    float inv = 1.0f - a;
    buf[i + 0] = static_cast<uint8_t>(
        std::clamp(c[0] * a * 255.0f + buf[i + 0] * inv, 0.0f, 255.0f));
    buf[i + 1] = static_cast<uint8_t>(
        std::clamp(c[1] * a * 255.0f + buf[i + 1] * inv, 0.0f, 255.0f));
    buf[i + 2] = static_cast<uint8_t>(
        std::clamp(c[2] * a * 255.0f + buf[i + 2] * inv, 0.0f, 255.0f));
    buf[i + 3] = static_cast<uint8_t>(
        std::clamp((a * 255.0f + buf[i + 3] * inv), 0.0f, 255.0f));
  }

  // Anti-aliased thick line segment via analytic distance-to-segment coverage.
  void stamp_segment(float ax, float ay, float bx, float by, float thick,
                     const float c[4]) {
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    if (len2 < 1e-6f) {
      stamp_dot(ax, ay, thick, c);
      return;
    }
    int x0 = static_cast<int>(std::floor(std::min(ax, bx) - thick));
    int x1 = static_cast<int>(std::ceil(std::max(ax, bx) + thick));
    int y0 = static_cast<int>(std::floor(std::min(ay, by) - thick));
    int y1 = static_cast<int>(std::ceil(std::max(ay, by) + thick));
    float radius = thick * 0.5f;
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        float fx = x + 0.5f - ax, fy = y + 0.5f - ay;
        float t = std::clamp((fx * dx + fy * dy) / len2, 0.0f, 1.0f);
        float d = std::sqrt((fx - t * dx) * (fx - t * dx) +
                            (fy - t * dy) * (fy - t * dy));
        float cov = std::clamp(radius + 0.5f - d, 0.0f, 1.0f);
        blend_px(x, y, c, cov);
      }
  }

  void stamp_dot(float cx, float cy, float thick, const float c[4]) {
    float radius = thick * 0.5f;
    int x0 = static_cast<int>(std::floor(cx - radius));
    int x1 = static_cast<int>(std::ceil(cx + radius));
    int y0 = static_cast<int>(std::floor(cy - radius));
    int y1 = static_cast<int>(std::ceil(cy + radius));
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) +
                            (y + 0.5f - cy) * (y + 0.5f - cy));
        float cov = std::clamp(radius + 0.5f - d, 0.0f, 1.0f);
        blend_px(x, y, c, cov);
      }
  }

  // 5x7 bitmap font (fallback glyphs): 7 rows per glyph, MSB of each row =
  // leftmost column.
  static constexpr uint8_t kGlyph(char c) {
    switch (c) {
      case 'A': return 0;
      case 'B': return 1;
      case 'C': return 2;
      case 'D': return 3;
      case 'E': return 4;
      case 'F': return 5;
      case 'G': return 6;
      case 'H': return 7;
      case 'I': return 8;
      case 'J': return 9;
      case 'K': return 10;
      case 'L': return 11;
      case 'M': return 12;
      case 'N': return 13;
      case 'O': return 14;
      case 'P': return 15;
      case 'Q': return 16;
      case 'R': return 17;
      case 'S': return 18;
      case 'T': return 19;
      case 'U': return 20;
      case 'V': return 21;
      case 'W': return 22;
      case 'X': return 23;
      case 'Y': return 24;
      case 'Z': return 25;
      case '0': return 26;
      case '1': return 27;
      case '2': return 28;
      case '3': return 29;
      case '4': return 30;
      case '5': return 31;
      case '6': return 32;
      case '7': return 33;
      case '8': return 34;
      case '9': return 35;
      case '!': return 36;
      case '?': return 37;
      case '.': return 38;
      case ',': return 39;
      case '\'': return 40;
      case '"': return 41;
      case '-': return 42;
      case '_': return 43;
      case '+': return 44;
      case '=': return 45;
      case '/': return 46;
      case '\\': return 47;
      case '(': return 48;
      case ')': return 49;
      case '[': return 50;
      case ']': return 51;
      case '<': return 52;
      case '>': return 53;
      case ':': return 54;
      case ';': return 55;
      case '#': return 56;
      case '$': return 57;
      case '%': return 58;
      case '&': return 59;
      case '*': return 60;
      case '@': return 61;
      case ' ': return 62;
      default: return 37;  // fallback: '?'
    }
  }

  static constexpr uint8_t kFont[][7] = {
      // A-Z
      {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
      {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
      {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
      {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
      {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
      {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
      {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
      {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
      {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
      {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
      {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
      {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
      {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
      {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
      {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
      {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
      {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
      {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
      {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
      {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
      {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
      {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
      {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},
      {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
      {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
      {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
      // 0-9
      {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
      {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
      {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
      {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
      {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
      {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
      {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E},
      {0x1F, 0x01, 0x02, 0x04, 0x04, 0x04, 0x04},
      {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
      {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E},
      // punctuation
      {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},  // !
      {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},  // ?
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},  // .
      {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08},  // ,
      {0x08, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00},  // '
      {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00},  // "
      {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},  // -
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},  // _
      {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},  // +
      {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},  // =
      {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10},  // /
      {0x10, 0x08, 0x08, 0x04, 0x02, 0x02, 0x01},  // backslash
      {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},  // (
      {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},  // )
      {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},  // [
      {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E},  // ]
      {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},  // <
      {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},  // >
      {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},  // :
      {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08},  // ;
      {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A},  // #
      {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04},  // $
      {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13},  // %
      {0x08, 0x14, 0x14, 0x08, 0x15, 0x12, 0x0D},  // &
      {0x00, 0x0A, 0x04, 0x1F, 0x04, 0x0A, 0x00},  // *
      {0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E},  // @
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // (space)
  };

  // System TTF font for Text annotations, loaded lazily via stb_truetype.
  // The font file bytes must outlive the stbtt_fontinfo, so both live together.
  struct AnnoFont {
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    bool loaded = false;
  } font_;

  inline static constexpr const char *const kFontCandidates[] = {
      // Windows
      "C:\\Windows\\Fonts\\segoeui.ttf",
      "C:\\Windows\\Fonts\\arial.ttf",
      "C:\\Windows\\Fonts\\consola.ttf",
      // Linux (Debian/Ubuntu, Fedora, Arch)
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      // macOS
      "/System/Library/Fonts/Supplemental/Arial.ttf",
  };

  bool load_font() {
    if (font_.loaded)
      return true;
    for (const char *path : kFontCandidates) {
      std::ifstream f(path, std::ios::binary);
      if (!f)
        continue;
      f.seekg(0, std::ios::end);
      std::streamoff size = f.tellg();
      f.seekg(0, std::ios::beg);
      if (size <= 0)
        continue;
      font_.data.resize(static_cast<size_t>(size));
      f.read(reinterpret_cast<char *>(font_.data.data()),
             static_cast<std::streamsize>(size));
      if (!f) {  // short read: font file is unusable
        font_.data.clear();
        continue;
      }
      int offset = stbtt_GetFontOffsetForIndex(font_.data.data(), 0);
      if (offset >= 0 &&
          stbtt_InitFont(&font_.info, font_.data.data(), offset))
        font_.loaded = true;
      if (font_.loaded)
        break;
      font_.data.clear();
    }
    return font_.loaded;
  }

  // Decode the UTF-8 codepoint starting at `i`, advancing `i` past it.
  static int utf8_next(const std::string &s, size_t &i) {
    uint8_t b0 = static_cast<uint8_t>(s[i]);
    if (b0 < 0x80) {
      i += 1;
      return b0;
    }
    int cp = 0;
    size_t n = 0;
    if ((b0 & 0xE0) == 0xC0) {
      cp = b0 & 0x1F;
      n = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
      cp = b0 & 0x0F;
      n = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
      cp = b0 & 0x07;
      n = 4;
    } else {
      i += 1;  // stray continuation byte: pass through as-is
      return b0;
    }
    if (i + n > s.size()) {
      i += 1;
      return b0;
    }
    for (size_t k = 1; k < n; k++) {
      uint8_t b = static_cast<uint8_t>(s[i + k]);
      if ((b & 0xC0) != 0x80) {
        i += 1;
        return b0;
      }
      cp = (cp << 6) | (b & 0x3F);
    }
    i += n;
    return cp;
  }

  void draw_text(const std::string &s, AnnoPoint pos, const float c[4]) {
    if (s.empty())
      return;
    if (!load_font()) {
      draw_text_bitmap(s, pos, c);
      return;
    }
    int ascent = 0, descent = 0, linegap = 0;
    stbtt_GetFontVMetrics(&font_.info, &ascent, &descent, &linegap);
    // Scale so the cap height matches the old bitmap font (~5% of video
    // height): stbtt_ScaleForPixelHeight targets (ascent - descent), so back
    // that out to land on `ascent` pixels.
    float scale =
        stbtt_ScaleForPixelHeight(&font_.info,
                                  vh * 0.05f * (ascent - descent) / ascent);
    float baseline = py(pos) + ascent * scale;
    float pen_x = px(pos);
    int prev_cp = 0;
    for (size_t i = 0; i < s.size();) {
      int cp = utf8_next(s, i);
      int advance = 0, lsb = 0;
      stbtt_GetCodepointHMetrics(&font_.info, cp, &advance, &lsb);
      if (prev_cp != 0)
        pen_x += stbtt_GetCodepointKernAdvance(&font_.info, prev_cp, cp) * scale;
      if (cp != ' ') {
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char *glyph = stbtt_GetCodepointBitmap(
            &font_.info, scale, scale, cp, &w, &h, &xoff, &yoff);
        if (glyph != nullptr) {
          int gx0 = static_cast<int>(std::lround(pen_x + xoff));
          int gy0 = static_cast<int>(std::lround(baseline + yoff));
          for (int gy = 0; gy < h; gy++)
            for (int gx = 0; gx < w; gx++)
              blend_px(gx0 + gx, gy0 + gy, c, glyph[gy * w + gx] / 255.0f);
          std::free(glyph);  // stb's default allocator is malloc/free
        }
      }
      pen_x += advance * scale;
      prev_cp = cp;
    }
  }

  // 5x7 bitmap font fallback (used only when no system TTF font is found):
  // 7 rows per glyph, MSB of each row = leftmost column.
  void draw_text_bitmap(const std::string &s, AnnoPoint pos, const float c[4]) {
    if (s.empty())
      return;
    int scale = std::max(1, static_cast<int>(std::llround(vh * 0.05f / 7.0f)));
    int origin_x = static_cast<int>(std::lround(px(pos)));
    int origin_y = static_cast<int>(std::lround(py(pos)));
    int pen_x = origin_x;
    for (unsigned char ch : s) {
      if (ch >= 128)  // the bitmap font is ASCII-only
        ch = '?';
      unsigned char u = static_cast<unsigned char>(std::toupper(ch));
      const uint8_t *g = kFont[kGlyph(static_cast<char>(u))];
      for (int row = 0; row < 7; row++) {
        uint8_t mask = g[row];
        for (int col = 0; col < 5; col++) {
          if (!(mask & (1 << (4 - col))))
            continue;
          for (int dy = 0; dy < scale; dy++)
            for (int dx = 0; dx < scale; dx++)
              blend_px(pen_x + col * scale + dx, origin_y + row * scale + dy, c,
                       1.0f);
        }
      }
      pen_x += 6 * scale;
    }
  }
};
