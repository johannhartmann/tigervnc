/* Copyright (C) 2026 TigerVNC team. All Rights Reserved.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <rfb/EncodingPolicy.h>

#include <cstring>
#include <cstdio>
#include <string>

namespace rfb {
namespace encoding {

// -=- Naming -----------------------------------------------------------

const char* presetName(Preset p) {
  switch (p) {
    case Preset::LANCrisp:       return "LANCrisp";
    case Preset::Balanced:       return "Balanced";
    case Preset::LowBandwidth:   return "LowBandwidth";
    case Preset::VideoOptimized: return "VideoOptimized";
    case Preset::Custom:         return "Custom";
  }
  return "unknown";
}

namespace {
int ascii_tolower(int c) {
  return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}
bool ieq(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (ascii_tolower(*a) != ascii_tolower(*b)) return false;
    ++a; ++b;
  }
  return *a == 0 && *b == 0;
}
}  // namespace

bool parsePreset(const char* str, Preset* out) {
  if (!str || !out) return false;
  if (ieq(str, "LANCrisp"))       { *out = Preset::LANCrisp;       return true; }
  if (ieq(str, "Balanced"))       { *out = Preset::Balanced;       return true; }
  if (ieq(str, "LowBandwidth"))   { *out = Preset::LowBandwidth;   return true; }
  if (ieq(str, "VideoOptimized")) { *out = Preset::VideoOptimized; return true; }
  if (ieq(str, "Custom"))         { *out = Preset::Custom;         return true; }
  return false;
}

const char* recommendedEncoderName(RecommendedEncoder e) {
  switch (e) {
    case RecommendedEncoder::Auto:      return "Auto";
    case RecommendedEncoder::Raw:       return "Raw";
    case RecommendedEncoder::Tight:     return "Tight";
    case RecommendedEncoder::TightJPEG: return "TightJPEG";
    case RecommendedEncoder::ZRLE:      return "ZRLE";
    case RecommendedEncoder::Hextile:   return "Hextile";
    case RecommendedEncoder::H264:      return "H264";
    case RecommendedEncoder::Max:       break;
  }
  return "unknown";
}

// -=- Preset tuning ----------------------------------------------------
//
// Numbers below are deliberately conservative. The intent is to give
// operators a coherent set of defaults that EncodeManager and (future)
// the H.264 encoder can both consume without further tuning. The
// "Custom" preset returns neutral values so that installing the policy
// doesn't perturb behaviour for users who don't pick a preset.

PresetTuning tuningFor(Preset preset) {
  PresetTuning t = {};
  switch (preset) {
    case Preset::LANCrisp:
      t.jpegQuality        = 92;
      t.tightCompression   = 2;
      t.zrleCompression    = 2;
      t.h264Enabled        = false;  // Lossless preset, no codec churn
      t.h264BitrateKbps    = 0;
      t.h264KeyframeIntervalFrames = 0;
      t.maxFrameRateHz     = 60;
      t.maxLatencyMs       = 20;
      return t;
    case Preset::Balanced:
      t.jpegQuality        = 75;
      t.tightCompression   = 6;
      t.zrleCompression    = 6;
      t.h264Enabled        = false;  // Flip when encoder lands
      t.h264BitrateKbps    = 0;
      t.h264KeyframeIntervalFrames = 0;
      t.maxFrameRateHz     = 30;
      t.maxLatencyMs       = 80;
      return t;
    case Preset::LowBandwidth:
      t.jpegQuality        = 45;
      t.tightCompression   = 9;
      t.zrleCompression    = 9;
      t.h264Enabled        = true;   // big win once the encoder lands
      t.h264BitrateKbps    = 2000;
      t.h264KeyframeIntervalFrames = 120;
      t.maxFrameRateHz     = 15;
      t.maxLatencyMs       = 300;
      return t;
    case Preset::VideoOptimized:
      t.jpegQuality        = 70;
      t.tightCompression   = 4;
      t.zrleCompression    = 6;
      t.h264Enabled        = true;
      t.h264BitrateKbps    = 8000;
      t.h264KeyframeIntervalFrames = 60;
      t.maxFrameRateHz     = 30;
      t.maxLatencyMs       = 50;
      return t;
    case Preset::Custom:
      // Sentinel-ish values: callers should treat Custom as "leave the
      // existing encoder defaults alone". We still emit reasonable
      // numbers for any caller that pulls the struct unconditionally.
      t.jpegQuality        = -1;
      t.tightCompression   = -1;
      t.zrleCompression    = -1;
      t.h264Enabled        = false;
      t.h264BitrateKbps    = 0;
      t.h264KeyframeIntervalFrames = 0;
      t.maxFrameRateHz     = 0;
      t.maxLatencyMs       = 0;
      return t;
  }
  return t;
}

// -=- Decision logic ---------------------------------------------------
//
// Thresholds are deliberately simple and well-named so the test cases
// below can pin them. "Tiny" is the area where the per-rect protocol
// overhead exceeds the value of any compression. "BigArea" is the
// minimum at which video-style codecs start to amortise their setup
// cost. "VideoFps" is the frames-per-second above which we suspect
// motion / video content rather than incidental UI redraws.

namespace {
constexpr int kTinyAreaThreshold     = 256;       // 16x16
constexpr int kJpegMinAreaThreshold  = 64 * 64;
constexpr int kH264MinAreaThreshold  = 256 * 256;
constexpr int kVideoFpsThreshold     = 5;
constexpr int kIndexedSmallPalette   = 96;        // matches Tight's
constexpr int kIndexedLargePalette   = 256;       // matches ZRLE's
}  // namespace

RecommendedEncoder pickEncoder(Preset preset,
                               const RectStats& stats,
                               const ClientCaps& caps) {
  if (stats.width <= 0 || stats.height <= 0)
    return RecommendedEncoder::Auto;

  PresetTuning t = tuningFor(preset);
  const int area = stats.width * stats.height;

  // 1. Tiny rects. The protocol's per-rectangle overhead dominates;
  // there's nothing to compress. Tight (with no JPEG / no zlib in this
  // size class) is the fast path in the existing encoder; fall back
  // to Raw if Tight isn't supported.
  if (area < kTinyAreaThreshold) {
    if (caps.supportsTight) return RecommendedEncoder::Tight;
    return RecommendedEncoder::Raw;
  }

  // 2. Large + high-change-rate + H.264 enabled by preset + supported
  // by client. This is the only case where the policy actively asks
  // for H.264. Until the server-side encoder lands, EncodeManager will
  // observe this recommendation, log a fallback via recordFallback(),
  // and continue with whatever it would have picked anyway.
  if (t.h264Enabled && caps.supportsH264 &&
      area >= kH264MinAreaThreshold &&
      stats.recentChangeFps >= kVideoFpsThreshold) {
    return RecommendedEncoder::H264;
  }

  // 3. Smooth / natural image content (caller's hasGradient flag).
  // TightJPEG amortises JPEG's overhead well above ~64x64 and is a
  // strict win over plain Tight on photos / video frames / gradients.
  if (stats.hasGradient && area >= kJpegMinAreaThreshold) {
    if (caps.supportsTightJPEG) return RecommendedEncoder::TightJPEG;
    if (caps.supportsZRLE)      return RecommendedEncoder::ZRLE;
    return RecommendedEncoder::Auto;
  }

  // 4. Indexed content with a small palette. Tight's indexed sub-
  // encoding is the best lossless choice here.
  if (stats.paletteSize > 0 &&
      stats.paletteSize <= kIndexedSmallPalette &&
      caps.supportsTight) {
    return RecommendedEncoder::Tight;
  }

  // 5. Indexed content with a larger palette where Tight's indexed
  // mode loses to ZRLE's.
  if (stats.paletteSize > 0 &&
      stats.paletteSize <= kIndexedLargePalette &&
      caps.supportsZRLE) {
    return RecommendedEncoder::ZRLE;
  }

  // 6. Unknown character: defer to the caller's existing heuristic.
  return RecommendedEncoder::Auto;

  (void)t;  // silence unused-variable for non-h264 builds
}

// -=- Diagnostics ------------------------------------------------------

void resetDiagnostics(Diagnostics* d) {
  if (!d) return;
  std::memset(d, 0, sizeof(*d));
}

void recordFrame(Diagnostics* d, RecommendedEncoder used,
                 uint64_t bytes, uint64_t encodeTimeUs) {
  if (!d) return;
  int idx = (int)used;
  if (idx < 0 || idx >= (int)RecommendedEncoder::Max) return;
  d->frames[idx]        += 1;
  d->bytes[idx]         += bytes;
  d->encodeTimeUs[idx]  += encodeTimeUs;
  d->totalFrames        += 1;
}

void recordFallback(Diagnostics* d, RecommendedEncoder /*wanted*/,
                    RecommendedEncoder /*usedInstead*/) {
  if (!d) return;
  d->fallbacks += 1;
}

std::string diagnosticsSummary(const Diagnostics& d) {
  // Bounded-size formatting via snprintf -> std::string. We list only
  // the non-zero entries to keep the line short on busy connections.
  char buf[512];
  int off = std::snprintf(buf, sizeof(buf),
                          "frames=%llu (",
                          (unsigned long long)d.totalFrames);
  if (off < 0) return std::string();
  bool first = true;
  for (int i = 0; i < (int)RecommendedEncoder::Max; ++i) {
    if (d.frames[i] == 0) continue;
    int n = std::snprintf(
        buf + off, (size_t)(sizeof(buf) - off),
        "%s%s=%llu", first ? "" : " ",
        recommendedEncoderName((RecommendedEncoder)i),
        (unsigned long long)d.frames[i]);
    if (n < 0 || (size_t)(off + n) >= sizeof(buf)) break;
    off += n;
    first = false;
  }
  std::snprintf(buf + off, (size_t)(sizeof(buf) - off),
                ") fallbacks=%llu",
                (unsigned long long)d.fallbacks);
  return std::string(buf);
}

}  // namespace encoding
}  // namespace rfb
