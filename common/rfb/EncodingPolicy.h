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

// EncodingPolicy.h
//
// A small, pure-logic module that encapsulates "for this kind of dirty
// rect on this connection under this preset, what encoder should we
// use?". The intent is that EncodeManager (today) can either consult
// the policy or use it as a hint for diagnostics, and a future H.264
// server-side encoder can plug into the same decision boundary
// without changing the API.
//
// Important: as of this commit there is no server-side H.264 encoder
// in TigerVNC. The policy can return RecommendedEncoder::H264 when
// the preset and client capabilities permit it, and the calling code
// is expected to fall back to a classic encoding because the
// encoderH264 case in EncodeManager is unimplemented. This is
// intentional: getting the policy in place ahead of the encoder lets
// the framework, the presets, the diagnostics, and the tests land
// independently of the (much larger) Media Foundation / libav encoder
// work.
//
// Cross-platform. No Windows / D3D / WinRT headers reach this file.

#ifndef __RFB_ENCODING_POLICY_H__
#define __RFB_ENCODING_POLICY_H__

#include <cstdint>
#include <string>

namespace rfb {
namespace encoding {

// -=- Operator-facing presets ------------------------------------------

enum class Preset {
  LANCrisp = 0,        // Lossless / near-lossless on a fast network
  Balanced,            // Reasonable defaults for ~typical connections
  LowBandwidth,        // Aggressive compression for tight pipes
  VideoOptimized,      // Tuned for full-screen video / animation
  Custom,              // Don't override anything; existing knobs win
};

const char* presetName(Preset preset);
bool parsePreset(const char* str, Preset* out);

struct PresetTuning {
  // Quality / compression knobs the existing classic encoders consume.
  int jpegQuality;        // 0..100 (TightJPEGEncoder, JPEGEncoder)
  int tightCompression;   // 0..9  (TightEncoder)
  int zrleCompression;    // 0..9  (ZRLEEncoder)

  // H.264 knobs. Aspirational until the server-side encoder lands;
  // the policy will refuse to recommend H.264 when h264Enabled is
  // false even if the client supports it.
  bool h264Enabled;
  int  h264BitrateKbps;       // 0 == unbounded
  int  h264KeyframeIntervalFrames;  // 0 == encoder default

  // Pacing.
  int maxFrameRateHz;       // 0 == unbounded
  int maxLatencyMs;         // soft target; encoder may exceed it
};

// Returns the canned tuning for a preset. Custom returns "neutral"
// values that pass through to the existing encoder defaults so that
// installing the policy doesn't perturb behaviour for users who
// haven't picked a preset.
PresetTuning tuningFor(Preset preset);

// -=- Per-rect input ---------------------------------------------------

struct RectStats {
  int width;
  int height;

  // Number of distinct colours observed in the rect, or -1 if the
  // caller hasn't computed the palette yet. Capped at 257 by callers
  // (the Tight/ZRLE encoders treat 256+ as "true colour"); the policy
  // uses two thresholds: <=96 (Tight indexed) and <=256 (ZRLE).
  int paletteSize;

  // Heuristic from the caller: does this rect look like a smooth /
  // natural image (true) or a UI / text region (false)? EncodeManager
  // computes an equivalent flag today via solid / palette analysis;
  // the policy treats `true` as "lossy compression is acceptable here".
  bool hasGradient;

  // Approximate frames-per-second over the last second for a region
  // overlapping this rect. Caller derives this from a small ring of
  // recent UpdateTracker flushes; 0 means "cold area, no recent
  // change". The policy uses this to differentiate static UI from
  // animation / video.
  int recentChangeFps;
};

struct ClientCaps {
  bool supportsH264;
  bool supportsTight;
  bool supportsTightJPEG;     // separate flag because some viewers
                              // advertise Tight without JPEG sub-encoding
  bool supportsZRLE;
  bool supportsHextile;
};

// -=- Recommendation ---------------------------------------------------

enum class RecommendedEncoder {
  // No opinion: caller should run its existing heuristic. This is the
  // fallback that lets the policy stay strictly additive while
  // EncodeManager's mature logic continues to do most of the work.
  Auto = 0,
  Raw,
  Tight,
  TightJPEG,
  ZRLE,
  Hextile,
  // Aspirational. EncodeManager has no path to encoderClass H264 yet;
  // recordFallback() should be called when this slot is returned so
  // the diagnostics surface "wanted H.264, fell back".
  H264,
  // Sentinel; keep last.
  Max,
};

const char* recommendedEncoderName(RecommendedEncoder e);

// The decision function. Pure: same inputs always produce the same
// output. No I/O, no logging, no global state.
RecommendedEncoder pickEncoder(Preset preset,
                               const RectStats& stats,
                               const ClientCaps& caps);

// -=- Diagnostics ------------------------------------------------------
//
// EncodeManager is expected to maintain a Diagnostics instance per
// connection (or globally) and call recordFrame() each time a
// rectangle is encoded. recordFallback() is called when the policy
// recommended H.264 but the implementation had to use something else.
// Both helpers are tiny inline accumulators -- they don't take locks
// and the caller is expected to serialize calls per-instance.

struct Diagnostics {
  uint64_t frames     [(int)RecommendedEncoder::Max];
  uint64_t bytes      [(int)RecommendedEncoder::Max];
  uint64_t encodeTimeUs[(int)RecommendedEncoder::Max];
  uint64_t totalFrames;
  uint64_t fallbacks;     // policy said H.264, encoder produced classic
};

void resetDiagnostics(Diagnostics* d);
void recordFrame(Diagnostics* d, RecommendedEncoder used,
                 uint64_t bytes, uint64_t encodeTimeUs);
void recordFallback(Diagnostics* d, RecommendedEncoder wanted,
                    RecommendedEncoder usedInstead);

// Render diagnostics as a one-line summary suitable for periodic
// info-level logging. Output is bounded; no heap allocation outside
// std::string itself.
//
// Example: "frames=1234 (Tight=900 TightJPEG=300 ZRLE=34 H264=0)
//           fallbacks=0"
std::string diagnosticsSummary(const Diagnostics& d);

}  // namespace encoding
}  // namespace rfb

#endif  // __RFB_ENCODING_POLICY_H__
