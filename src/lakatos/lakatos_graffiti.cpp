#include "lakatos_graffiti.h"
#ifdef ARDUINO
#include "lakatos_state.h"
#else
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef memcpy_P
#define memcpy_P memcpy
#endif
#endif

#include <math.h>
#include <string.h>

namespace {

constexpr int kNumResamplePts = 16;
constexpr int kMaxStrokeWork  = 128;

struct FloatPt {
  float x;
  float y;
};

struct GraffitiTemplate {
  char    ch;         // Output ASCII character
  uint8_t pane;       // LAKATOS_PANE_LETTERS or LAKATOS_PANE_DIGITS
  uint8_t primary;    // 1 = shown on the [?] visual reference chart, 0 = alternate allograph
  uint8_t numPts;     // Number of control vertices in pts[] (2..14)
  uint8_t pts[14][2]; // Control vertices in [0..100] x [0..100]
};

// ---------------------------------------------------------------------------
// PROGMEM Palm Graffiti 1 Unistroke Template Table (0 bytes of static RAM)
// ---------------------------------------------------------------------------
static const GraffitiTemplate kTemplates[] PROGMEM = {
  // ======================= LEFT PANE: LETTERS (a-z) ========================
  // a: Caret '^' (bottom-left -> top-mid -> bottom-right)
  {'a', LAKATOS_PANE_LETTERS, 1, 3, {{5, 95}, {50, 5}, {95, 95}}},
  // b: Palm Graffiti 1 'B' (down left stem, up, and two distinct rounded right bowls)
  {'b', LAKATOS_PANE_LETTERS, 1, 13,
   {{12, 6}, {12, 94}, {16, 8}, {68, 8}, {90, 18}, {90, 36}, {68, 48}, {20, 48},
    {70, 50}, {94, 62}, {94, 82}, {68, 94}, {12, 94}}},
  {'b', LAKATOS_PANE_LETTERS, 0, 12,
   {{12, 94}, {12, 8}, {68, 8}, {90, 18}, {90, 36}, {68, 48}, {20, 48},
    {70, 50}, {94, 62}, {94, 82}, {68, 94}, {12, 94}}},
  {'b', LAKATOS_PANE_LETTERS, 0, 7,
   {{12, 6}, {12, 94}, {18, 50}, {75, 50}, {92, 65}, {92, 85}, {12, 94}}},
  // c: Open C arc (top-right -> left -> bottom-right)
  {'c', LAKATOS_PANE_LETTERS, 1, 6, {{90, 12}, {35, 5}, {8, 28}, {8, 72}, {35, 95}, {90, 88}}},
  // d: Palm Graffiti 1 'D' (down left stem, up, and wide rounded right D-bowl)
  {'d', LAKATOS_PANE_LETTERS, 1, 9,
   {{12, 6}, {12, 94}, {16, 8}, {62, 8}, {90, 24}, {95, 50}, {90, 76}, {62, 94}, {12, 94}}},
  {'d', LAKATOS_PANE_LETTERS, 0, 8,
   {{12, 94}, {12, 8}, {62, 8}, {90, 24}, {95, 50}, {90, 76}, {62, 94}, {12, 94}}},
  {'d', LAKATOS_PANE_LETTERS, 0, 6, {{85, 55}, {15, 55}, {15, 95}, {85, 95}, {85, 5}, {85, 95}}},
  // e: Greek epsilon / backward-3 (top-right -> left -> mid-cusp -> left -> bottom-right)
  {'e', LAKATOS_PANE_LETTERS, 1, 7, {{90, 10}, {18, 8}, {8, 32}, {75, 50}, {8, 68}, {18, 92}, {90, 90}}},
  // f: Gamma 'Γ' (top-right -> top-left -> bottom-left)
  {'f', LAKATOS_PANE_LETTERS, 1, 3, {{95, 8}, {12, 8}, {12, 95}}},
  // g: C-arc that hooks up and left into center
  {'g', LAKATOS_PANE_LETTERS, 1, 7, {{90, 15}, {22, 5}, {5, 50}, {22, 95}, {90, 85}, {90, 50}, {48, 50}}},
  // h: Down left stem, up and over right arch
  {'h', LAKATOS_PANE_LETTERS, 1, 6, {{12, 5}, {12, 95}, {18, 50}, {55, 45}, {88, 55}, {90, 95}}},
  // i: Straight vertical line down (also handled by 1D fast-path)
  {'i', LAKATOS_PANE_LETTERS, 1, 2, {{50, 5}, {50, 95}}},
  // j: Down right stem, curving left and up at bottom
  {'j', LAKATOS_PANE_LETTERS, 1, 5, {{82, 5}, {82, 72}, {65, 94}, {35, 94}, {10, 72}}},
  // k: Original Palm Graffiti 1 alpha/fish '∝' (top-right -> down-left -> left loop -> bottom-right)
  {'k', LAKATOS_PANE_LETTERS, 1, 7,
   {{90, 8}, {18, 86}, {8, 68}, {8, 38}, {26, 30}, {48, 42}, {92, 94}}},
  {'k', LAKATOS_PANE_LETTERS, 0, 6, {{10, 5}, {10, 95}, {10, 50}, {88, 10}, {22, 50}, {90, 95}}},
  // l: L-shape (top-left -> bottom-left -> bottom-right)
  {'l', LAKATOS_PANE_LETTERS, 1, 3, {{12, 5}, {12, 95}, {95, 95}}},
  // m: M-shape (bottom-left -> top-left -> mid-bottom -> top-right -> bottom-right)
  {'m', LAKATOS_PANE_LETTERS, 1, 5, {{5, 95}, {15, 8}, {50, 82}, {85, 8}, {95, 95}}},
  {'m', LAKATOS_PANE_LETTERS, 0, 6, {{5, 10}, {5, 95}, {15, 12}, {50, 82}, {85, 12}, {95, 95}}},
  // n: N-shape (bottom-left -> top-left -> bottom-right -> top-right) + lowercase arch
  {'n', LAKATOS_PANE_LETTERS, 1, 4, {{8, 95}, {8, 8}, {92, 95}, {92, 8}}},
  {'n', LAKATOS_PANE_LETTERS, 0, 5, {{10, 15}, {10, 95}, {20, 15}, {82, 15}, {92, 95}}},
  // o: Counter-clockwise loop starting at top (plus clockwise allograph)
  {'o', LAKATOS_PANE_LETTERS, 1, 9,
   {{50, 5}, {18, 15}, {5, 50}, {18, 85}, {50, 95}, {82, 85}, {95, 50}, {82, 15}, {50, 5}}},
  {'o', LAKATOS_PANE_LETTERS, 0, 9,
   {{50, 5}, {82, 15}, {95, 50}, {82, 85}, {50, 95}, {18, 85}, {5, 50}, {18, 15}, {50, 5}}},
  // p: Palm Graffiti 1 'P' (down/up left stem and rounded clockwise top bowl)
  {'p', LAKATOS_PANE_LETTERS, 1, 8,
   {{12, 6}, {12, 94}, {16, 8}, {68, 8}, {92, 20}, {92, 40}, {68, 52}, {12, 52}}},
  {'p', LAKATOS_PANE_LETTERS, 0, 7,
   {{12, 94}, {12, 8}, {68, 8}, {92, 20}, {92, 40}, {68, 52}, {12, 52}}},
  // q: Counter-clockwise circle starting at top with outward top-right tail
  {'q', LAKATOS_PANE_LETTERS, 1, 8, {{50, 8}, {8, 28}, {8, 72}, {48, 95}, {85, 72}, {82, 25}, {45, 8}, {98, 22}}},
  // r: Palm Graffiti 1 'R' (P-bowl with diagonal leg to bottom-right)
  {'r', LAKATOS_PANE_LETTERS, 1, 9,
   {{12, 6}, {12, 94}, {16, 8}, {68, 8}, {90, 20}, {90, 40}, {65, 50}, {18, 50}, {94, 94}}},
  {'r', LAKATOS_PANE_LETTERS, 0, 8,
   {{12, 94}, {12, 8}, {68, 8}, {90, 20}, {90, 40}, {65, 50}, {18, 50}, {94, 94}}},
  // s: Standard S-curve (top-right -> top-left -> bottom-right -> bottom-left)
  {'s', LAKATOS_PANE_LETTERS, 1, 6, {{88, 15}, {25, 5}, {8, 32}, {92, 68}, {75, 95}, {12, 85}}},
  // t: Top bar left-to-right, then down right side '┐'
  {'t', LAKATOS_PANE_LETTERS, 1, 3, {{5, 8}, {92, 8}, {92, 95}}},
  // u: U-cup (top-left -> bottom-left -> bottom-right -> top-right)
  {'u', LAKATOS_PANE_LETTERS, 1, 6, {{10, 8}, {10, 70}, {30, 94}, {70, 94}, {90, 70}, {90, 8}}},
  // v: Palm Graffiti 1 'V' with right horizontal tail at top (distinguishes V from U), plus plain V
  {'v', LAKATOS_PANE_LETTERS, 1, 4, {{5, 8}, {38, 95}, {72, 8}, {98, 8}}},
  {'v', LAKATOS_PANE_LETTERS, 0, 3, {{8, 8}, {50, 95}, {92, 8}}},
  // w: W-shape (top-left -> bottom -> mid -> bottom -> top-right)
  {'w', LAKATOS_PANE_LETTERS, 1, 5, {{5, 8}, {22, 95}, {50, 22}, {78, 95}, {95, 8}}},
  // x: Single-stroke X (top-left -> bottom-right -> top-right -> bottom-left)
  {'x', LAKATOS_PANE_LETTERS, 1, 4, {{10, 8}, {90, 92}, {90, 8}, {10, 92}}},
  {'x', LAKATOS_PANE_LETTERS, 0, 4, {{10, 8}, {90, 92}, {10, 92}, {90, 8}}},
  // y: Palm Graffiti 1 loop Y (top-left -> mid -> top-right -> down right -> bottom-left loop)
  {'y', LAKATOS_PANE_LETTERS, 1, 7, {{8, 8}, {45, 45}, {88, 8}, {88, 88}, {42, 96}, {20, 72}, {88, 55}}},
  {'y', LAKATOS_PANE_LETTERS, 0, 4, {{8, 8}, {50, 50}, {92, 8}, {15, 95}}},
  // z: Z-shape (top-left -> top-right -> bottom-left -> bottom-right)
  {'z', LAKATOS_PANE_LETTERS, 1, 4, {{8, 8}, {92, 8}, {8, 92}, {92, 92}}},

  // =================== RIGHT PANE: DIGITS (0-9) & MATH =====================
  // 0: Counter-clockwise or clockwise oval starting at top
  {'0', LAKATOS_PANE_DIGITS, 1, 7, {{50, 5}, {10, 25}, {5, 72}, {50, 95}, {95, 72}, {90, 25}, {50, 5}}},
  {'0', LAKATOS_PANE_DIGITS, 0, 7, {{50, 5}, {90, 25}, {95, 72}, {50, 95}, {5, 72}, {10, 25}, {50, 5}}},
  // 1: Vertical line down (also handled by 1D fast-path)
  {'1', LAKATOS_PANE_DIGITS, 1, 2, {{50, 5}, {50, 95}}},
  // 2: Clockwise top arch -> diagonal to bottom-left -> horizontal to bottom-right
  {'2', LAKATOS_PANE_DIGITS, 1, 5, {{10, 25}, {50, 5}, {90, 25}, {8, 95}, {95, 95}}},
  // 3: Two right-facing curves with middle cusp
  {'3', LAKATOS_PANE_DIGITS, 1, 7, {{12, 10}, {88, 10}, {85, 42}, {25, 50}, {90, 60}, {85, 92}, {12, 90}}},
  // 4: L-shape (top-left -> bottom-left -> bottom-right)
  {'4', LAKATOS_PANE_DIGITS, 1, 3, {{12, 5}, {12, 92}, {95, 92}}},
  // 5: Top-right -> top-left -> mid-left -> mid-right -> bottom curve -> bottom-left
  {'5', LAKATOS_PANE_DIGITS, 1, 7, {{92, 8}, {12, 8}, {12, 45}, {85, 45}, {95, 78}, {50, 95}, {10, 85}}},
  // 6: Top-right -> down left curve -> closed bottom loop
  {'6', LAKATOS_PANE_DIGITS, 1, 7, {{80, 5}, {12, 42}, {10, 82}, {52, 96}, {92, 78}, {80, 50}, {15, 55}}},
  // 7: Top-left -> top-right -> diagonal down-left
  {'7', LAKATOS_PANE_DIGITS, 1, 3, {{8, 8}, {95, 8}, {28, 95}}},
  {'7', LAKATOS_PANE_DIGITS, 0, 3, {{8, 8}, {95, 8}, {85, 95}}},
  // 8: Figure-8 starting at top-center
  {'8', LAKATOS_PANE_DIGITS, 1, 7, {{50, 5}, {10, 22}, {90, 75}, {50, 95}, {10, 75}, {90, 22}, {50, 5}}},
  {'8', LAKATOS_PANE_DIGITS, 0, 7, {{50, 5}, {90, 22}, {10, 75}, {50, 95}, {90, 75}, {10, 22}, {50, 5}}},
  // 9: Counter-clockwise top loop from top-right, then straight down right stem
  {'9', LAKATOS_PANE_DIGITS, 1, 6, {{88, 25}, {45, 5}, {8, 25}, {45, 48}, {88, 25}, {88, 95}}},
  // +: Down vertical center, diagonal to mid-left, horizontal to mid-right
  {'+', LAKATOS_PANE_DIGITS, 1, 4, {{50, 8}, {50, 92}, {8, 50}, {92, 50}}},
  // -: Straight horizontal right (also handled by 1D fast-path)
  {'-', LAKATOS_PANE_DIGITS, 1, 2, {{10, 50}, {90, 50}}},
  // *: Single-stroke X/star (top-left -> bottom-right -> top-right -> bottom-left)
  {'*', LAKATOS_PANE_DIGITS, 1, 4, {{10, 10}, {90, 90}, {90, 10}, {10, 90}}},
  // /: Diagonal slash top-right to bottom-left (also handled by 1D fast-path)
  {'/', LAKATOS_PANE_DIGITS, 1, 2, {{85, 10}, {15, 90}}},
  // =: Z-like double horizontal bar (left-to-right, diagonal back, left-to-right)
  {'=', LAKATOS_PANE_DIGITS, 1, 4, {{10, 28}, {90, 28}, {10, 72}, {90, 72}}},
  // (: Narrow left parenthesis arc (top -> mid-left -> bottom)
  {'(', LAKATOS_PANE_DIGITS, 1, 5, {{68, 5}, {35, 25}, {25, 50}, {35, 75}, {68, 95}}},
  // ): Narrow right parenthesis arc (top -> mid-right -> bottom)
  {')', LAKATOS_PANE_DIGITS, 1, 5, {{32, 5}, {65, 25}, {75, 50}, {65, 75}, {32, 95}}},
};

constexpr int kNumTemplates = sizeof(kTemplates) / sizeof(kTemplates[0]);

static float polyPathLength(const FloatPt* pts, int n) {
  float len = 0.0f;
  for (int i = 1; i < n; i++) {
    len += hypotf(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
  }
  return len;
}

// Resamples an arbitrary polyline `inPts[0..inCount-1]` into `kNumResamplePts` (16)
// equidistant points along the path, then normalizes them into [-100..+100] with
// aspect-ratio floor protection and outputs the aspect ratio `spanX / (spanX + spanY)`.
static float resampleAndNormalize16(const FloatPt* inPts, int inCount,
                                    FloatPt out16[kNumResamplePts]) {
  if (inCount <= 0) {
    memset(out16, 0, sizeof(FloatPt) * kNumResamplePts);
    return 0.5f;
  }
  if (inCount == 1) {
    for (int i = 0; i < kNumResamplePts; i++) out16[i] = inPts[0];
    return 0.5f;
  }

  float totalLen = polyPathLength(inPts, inCount);
  if (totalLen < 1e-3f) {
    for (int i = 0; i < kNumResamplePts; i++) out16[i] = inPts[0];
    return 0.5f;
  }

  float step = totalLen / (float)(kNumResamplePts - 1);
  out16[0] = inPts[0];
  int outIdx = 1;
  float accum = 0.0f;

  FloatPt prev = inPts[0];
  int i = 1;
  while (i < inCount && outIdx < kNumResamplePts - 1) {
    FloatPt cur = inPts[i];
    float d = hypotf(cur.x - prev.x, cur.y - prev.y);
    if (accum + d >= step && d > 1e-4f) {
      float t = (step - accum) / d;
      FloatPt q = {prev.x + t * (cur.x - prev.x), prev.y + t * (cur.y - prev.y)};
      out16[outIdx++] = q;
      prev = q;
      accum = 0.0f;
    } else {
      accum += d;
      prev = cur;
      i++;
    }
  }
  while (outIdx < kNumResamplePts) {
    out16[outIdx++] = inPts[inCount - 1];
  }

  float minX = out16[0].x, maxX = out16[0].x;
  float minY = out16[0].y, maxY = out16[0].y;
  for (int k = 1; k < kNumResamplePts; k++) {
    if (out16[k].x < minX) minX = out16[k].x;
    if (out16[k].x > maxX) maxX = out16[k].x;
    if (out16[k].y < minY) minY = out16[k].y;
    if (out16[k].y > maxY) maxY = out16[k].y;
  }

  float spanX = maxX - minX;
  float spanY = maxY - minY;
  float cx    = 0.5f * (minX + maxX);
  float cy    = 0.5f * (minY + maxY);

  float scaleX = fmaxf(spanX, 0.42f * spanY);
  float scaleY = fmaxf(spanY, 0.42f * spanX);
  if (scaleX < 1.0f) scaleX = 1.0f;
  if (scaleY < 1.0f) scaleY = 1.0f;

  for (int k = 0; k < kNumResamplePts; k++) {
    out16[k].x = (out16[k].x - cx) * (200.0f / scaleX);
    out16[k].y = (out16[k].y - cy) * (200.0f / scaleY);
  }

  float denom = spanX + spanY;
  return (denom > 1e-3f) ? (spanX / denom) : 0.5f;
}

static float weightedPathDistance(const FloatPt u[kNumResamplePts],
                                  const FloatPt t[kNumResamplePts],
                                  float cosA, float sinA) {
  float sum  = 0.0f;
  float wSum = 0.0f;
  for (int i = 0; i < kNumResamplePts; i++) {
    float rx = u[i].x * cosA - u[i].y * sinA;
    float ry = u[i].x * sinA + u[i].y * cosA;
    float d  = hypotf(rx - t[i].x, ry - t[i].y);
    float w  = 1.0f;
    if (i == 0 || i == kNumResamplePts - 1) {
      w = 1.6f;
    } else if (i == 1 || i == kNumResamplePts - 2) {
      w = 1.25f;
    }
    sum  += w * d;
    wSum += w;
  }
  return sum / wSum;
}

}  // namespace

LakatosGraffitiResult lakatosRecognizeGraffitiStroke(const LakatosGraffitiPt* pts,
                                                     int count,
                                                     uint8_t pane) {
  LakatosGraffitiResult res = {LAKATOS_GRAFFITI_NONE, 0};
  if (!pts || count <= 0) return res;

  // 1. Copy and deduplicate raw points into stack work array
  FloatPt work[kMaxStrokeWork];
  int n = 0;
  for (int i = 0; i < count && n < kMaxStrokeWork; i++) {
    float x = (float)pts[i].x;
    float y = (float)pts[i].y;
    if (n == 0 || fabsf(x - work[n - 1].x) >= 1.0f || fabsf(y - work[n - 1].y) >= 1.0f) {
      work[n++] = {x, y};
    }
  }

  // 1b. Reject sudden excursion spikes (where a point jumps out and right back)
  for (int pass = 0; pass < 2 && n >= 4; pass++) {
    if (n >= 3) {
      float d01 = hypotf(work[1].x - work[0].x, work[1].y - work[0].y);
      float d12 = hypotf(work[2].x - work[1].x, work[2].y - work[1].y);
      if (d01 > 28.0f && d12 < 14.0f) {
        memmove(&work[0], &work[1], (size_t)(n - 1) * sizeof(FloatPt));
        n--;
      }
    }
    if (n >= 3) {
      float dLast = hypotf(work[n - 1].x - work[n - 2].x, work[n - 1].y - work[n - 2].y);
      float dPrev = hypotf(work[n - 2].x - work[n - 3].x, work[n - 2].y - work[n - 3].y);
      if (dLast > 28.0f && dPrev < 14.0f) {
        n--;
      }
    }
    int outN = 1;
    for (int i = 1; i < n - 1; i++) {
      FloatPt prev = work[outN - 1];
      FloatPt cur  = work[i];
      FloatPt next = work[i + 1];
      float dPrev   = hypotf(cur.x - prev.x, cur.y - prev.y);
      float dNext   = hypotf(next.x - cur.x, next.y - cur.y);
      float dBridge = hypotf(next.x - prev.x, next.y - prev.y);
      if (dPrev > 18.0f && dNext > 18.0f && (dPrev + dNext) > 2.4f * dBridge + 8.0f) {
        continue;  // Drop spurious excursion spike
      }
      work[outN++] = cur;
    }
    work[outN++] = work[n - 1];
    n = outN;
  }

  float minX = work[0].x, maxX = work[0].x;
  float minY = work[0].y, maxY = work[0].y;
  for (int i = 1; i < n; i++) {
    if (work[i].x < minX) minX = work[i].x;
    if (work[i].x > maxX) maxX = work[i].x;
    if (work[i].y < minY) minY = work[i].y;
    if (work[i].y > maxY) maxY = work[i].y;
  }
  float spanX = maxX - minX;
  float spanY = maxY - minY;
  float rawLen = polyPathLength(work, n);

  // 2. Dot / Tap check -> '.'
  if (rawLen < 12.0f && spanX < 10.0f && spanY < 10.0f) {
    res.ch         = '.';
    res.confidence = 99;
    return res;
  }

  // 3. Light 3-point smoothing on interior vertices to suppress resistive ADC jitter
  if (n >= 5) {
    FloatPt prev = work[0];
    for (int i = 1; i < n - 1; i++) {
      FloatPt cur = work[i];
      work[i].x = 0.25f * prev.x + 0.50f * cur.x + 0.25f * work[i + 1].x;
      work[i].y = 0.25f * prev.y + 0.50f * cur.y + 0.25f * work[i + 1].y;
      prev = cur;
    }
  }

  float smoothLen = polyPathLength(work, n);
  float dx0N      = work[n - 1].x - work[0].x;
  float dy0N      = work[n - 1].y - work[0].y;
  float endDist   = hypotf(dx0N, dy0N);

  // 4. 1D Linear Stroke Fast-Path (Space, Backspace, Shift, 'i'/'1', '/', '-')
  if (smoothLen >= 13.0f && endDist / smoothLen >= 0.85f) {
    float deg = atan2f(dy0N, dx0N) * (180.0f / (float)M_PI);
    if (deg >= -28.0f && deg <= 28.0f) {
      res.ch         = (pane == LAKATOS_PANE_DIGITS) ? '-' : ' ';
      res.confidence = 95;
      return res;
    }
    if (deg >= 152.0f || deg <= -152.0f) {
      res.ch         = LAKATOS_GRAFFITI_BACKSPACE;
      res.confidence = 95;
      return res;
    }
    if (deg >= -118.0f && deg <= -62.0f) {
      res.ch         = LAKATOS_GRAFFITI_SHIFT;
      res.confidence = 95;
      return res;
    }
    if (deg >= 62.0f && deg <= 118.0f) {
      res.ch         = (pane == LAKATOS_PANE_DIGITS) ? '1' : 'i';
      res.confidence = 95;
      return res;
    }
    if ((deg > 118.0f && deg < 152.0f) || (deg > -62.0f && deg < -28.0f)) {
      res.ch         = '/';
      res.confidence = 92;
      return res;
    }
  }

  // 5. 2D Resampled (N=16) Bounded-Orientation Template Matching
  FloatPt user16[kNumResamplePts];
  float userAspect = resampleAndNormalize16(work, n, user16);

  // Bounded rotation angles: -9 deg, 0 deg, +9 deg
  static const float kCosA[3] = {0.9876883f, 1.0f, 0.9876883f};
  static const float kSinA[3] = {-0.1564345f, 0.0f, 0.1564345f};

  float bestScore = 1e9f;
  char  bestChar  = LAKATOS_GRAFFITI_NONE;

  for (int tIdx = 0; tIdx < kNumTemplates; tIdx++) {
    GraffitiTemplate tpl;
    memcpy_P(&tpl, &kTemplates[tIdx], sizeof(GraffitiTemplate));
    if (tpl.pane != pane || tpl.numPts < 2) continue;

    FloatPt tplRaw[14];
    for (int k = 0; k < tpl.numPts; k++) {
      tplRaw[k].x = (float)tpl.pts[k][0];
      tplRaw[k].y = (float)tpl.pts[k][1];
    }

    FloatPt tpl16[kNumResamplePts];
    float tplAspect = resampleAndNormalize16(tplRaw, tpl.numPts, tpl16);
    float aspectPen = 24.0f * fabsf(userAspect - tplAspect);

    for (int a = 0; a < 3; a++) {
      float d = weightedPathDistance(user16, tpl16, kCosA[a], kSinA[a]) + aspectPen;
      if (d < bestScore) {
        bestScore = d;
        bestChar  = tpl.ch;
      }
    }
  }

  if (bestChar != LAKATOS_GRAFFITI_NONE && bestScore < 115.0f) {
    int conf = (int)(100.0f - bestScore * 0.7f);
    if (conf < 10) conf = 10;
    if (conf > 99) conf = 99;
    res.ch         = bestChar;
    res.confidence = (uint8_t)conf;
  }
  return res;
}

#ifdef ARDUINO
// ---------------------------------------------------------------------------
// Visual 2-Page Graffiti Stroke Reference Modal ([?] Button)
// ---------------------------------------------------------------------------
static void drawStrokeCell(int cx, int cy, int cw, int ch,
                           const char* label, const uint8_t pts[][2], int numPts) {
  tft.fillRoundRect(cx, cy, cw, ch, 4, 0x1082);
  tft.drawRoundRect(cx, cy, cw, ch, 4, 0x39E7);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, 0x1082);
  bool twoChar = (label[0] != '\0' && label[1] != '\0');
  tft.drawString(label, cx + 3, cy + 3, twoChar ? 1 : 2);

  int leftIn = twoChar ? 16 : 13;
  int padX = cx + leftIn;
  int padY = cy + 5;
  int padW = cw - leftIn - 4;
  int padH = ch - 10;
  if (padW < 10) padW = 10;
  if (padH < 10) padH = 10;

  for (int i = 1; i < numPts; i++) {
    int x0 = padX + (pts[i - 1][0] * padW) / 100;
    int y0 = padY + (pts[i - 1][1] * padH) / 100;
    int x1 = padX + (pts[i][0] * padW) / 100;
    int y1 = padY + (pts[i][1] * padH) / 100;
    tft.drawLine(x0, y0, x1, y1, TFT_CYAN);
  }
  if (numPts > 0) {
    int sx = padX + (pts[0][0] * padW) / 100;
    int sy = padY + (pts[0][1] * padH) / 100;
    tft.fillCircle(sx, sy, 2, TFT_GREEN);
  }
}

static void drawGraffitiHelpPage(int page) {
  LakatosTftGuard tftLock;
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);

  tft.fillRoundRect(2, 2, 56, 20, 4, 0x2945);
  tft.drawRoundRect(2, 2, 56, 20, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("BACK", 30, 12, 1);

  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString((page == 0) ? "GRAFFITI LETTERS (1/2)" : "GRAFFITI DIGITS (2/2)", 152, 12, 2);

  tft.fillRoundRect(244, 2, 74, 20, 4, 0x2945);
  tft.drawRoundRect(244, 2, 74, 20, 4, TFT_YELLOW);
  tft.setTextColor(TFT_YELLOW, 0x2945);
  tft.drawString((page == 0) ? "DIGITS >" : "< LETTERS", 281, 12, 1);

  if (page == 0) {
    // 7 cols x 4 rows = 28 cells (a..z + SP + BS)
    int cellIdx = 0;
    for (int tIdx = 0; tIdx < kNumTemplates && cellIdx < 26; tIdx++) {
      GraffitiTemplate tpl;
      memcpy_P(&tpl, &kTemplates[tIdx], sizeof(GraffitiTemplate));
      if (tpl.pane != LAKATOS_PANE_LETTERS || !tpl.primary) continue;

      int col = cellIdx % 7;
      int row = cellIdx / 7;
      int cx  = 2 + col * 45;
      int cy  = 26 + row * 46;
      char lbl[2] = {tpl.ch, '\0'};
      drawStrokeCell(cx, cy, 43, 44, lbl, tpl.pts, tpl.numPts);
      cellIdx++;
    }
    const uint8_t spPts[2][2] = {{10, 50}, {90, 50}};
    const uint8_t bsPts[2][2] = {{90, 50}, {10, 50}};
    drawStrokeCell(2 + 5 * 45, 26 + 3 * 46, 43, 44, "SP", spPts, 2);
    drawStrokeCell(2 + 6 * 45, 26 + 3 * 46, 43, 44, "BS", bsPts, 2);
  } else {
    // 5 cols x 4 rows = 20 cells (0..9, +, -, *, /, =, (, ), ., BS, SH)
    int cellIdx = 0;
    for (int tIdx = 0; tIdx < kNumTemplates && cellIdx < 17; tIdx++) {
      GraffitiTemplate tpl;
      memcpy_P(&tpl, &kTemplates[tIdx], sizeof(GraffitiTemplate));
      if (tpl.pane != LAKATOS_PANE_DIGITS || !tpl.primary) continue;

      int col = cellIdx % 5;
      int row = cellIdx / 5;
      int cx  = 3 + col * 63;
      int cy  = 26 + row * 46;
      char lbl[2] = {tpl.ch, '\0'};
      drawStrokeCell(cx, cy, 61, 44, lbl, tpl.pts, tpl.numPts);
      cellIdx++;
    }
    const uint8_t dotPts[1][2] = {{50, 50}};
    const uint8_t bsPts[2][2]  = {{90, 50}, {10, 50}};
    const uint8_t shPts[2][2]  = {{50, 90}, {50, 10}};
    drawStrokeCell(3 + 2 * 63, 26 + 3 * 46, 61, 44, ".", dotPts, 1);
    drawStrokeCell(3 + 3 * 63, 26 + 3 * 46, 61, 44, "BS", bsPts, 2);
    drawStrokeCell(3 + 4 * 63, 26 + 3 * 46, 61, 44, "SH", shPts, 2);
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_GREEN, 0x0841);
  tft.drawString("Green dot = stroke start  |  Up = Shift  |  Left = Backspace", 160, 224, 1);
  tft.setTextDatum(TL_DATUM);
}

void lakatosRunGraffitiHelpModal() {
  int page = 0;
  drawGraffitiHelpPage(page);
  lakatosWaitTouchRelease();

  bool wasDown = false;
  uint32_t lastRel = millis();
  while (true) {
    if (lakatosShouldExitApp()) return;
    updateLakatosConfigBackground();
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();
    if (t.touched) {
      if (!wasDown && (uint32_t)(now - lastRel) >= 90u) {
        wasDown = true;
        if (t.y <= 28 && t.x <= 64) {
          lakatosWaitTouchRelease();
          return;
        }
        page = 1 - page;
        drawGraffitiHelpPage(page);
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}
#endif

