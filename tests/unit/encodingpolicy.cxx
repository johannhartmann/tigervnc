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

#include <gtest/gtest.h>

#include <rfb/EncodingPolicy.h>

using namespace rfb::encoding;

namespace {

// All capabilities; modern viewer.
ClientCaps fullCaps() {
  ClientCaps c{};
  c.supportsH264       = true;
  c.supportsTight      = true;
  c.supportsTightJPEG  = true;
  c.supportsZRLE       = true;
  c.supportsHextile    = true;
  return c;
}

// Bare-minimum viewer.
ClientCaps bareCaps() {
  ClientCaps c{};
  c.supportsH264       = false;
  c.supportsTight      = false;
  c.supportsTightJPEG  = false;
  c.supportsZRLE       = false;
  c.supportsHextile    = false;
  return c;
}

RectStats uiRect(int w = 200, int h = 80) {
  RectStats s{};
  s.width = w; s.height = h;
  s.paletteSize = 8;        // small palette -> looks like UI
  s.hasGradient = false;
  s.recentChangeFps = 0;
  return s;
}

RectStats photoRect(int w = 800, int h = 600) {
  RectStats s{};
  s.width = w; s.height = h;
  s.paletteSize = 257;      // many colors -> not indexed
  s.hasGradient = true;
  s.recentChangeFps = 1;    // occasional update
  return s;
}

RectStats videoRect(int w = 1280, int h = 720, int fps = 30) {
  RectStats s{};
  s.width = w; s.height = h;
  s.paletteSize = 257;
  s.hasGradient = true;
  s.recentChangeFps = fps;
  return s;
}

}  // namespace

// -=- Naming round-trips -----------------------------------------------

TEST(EncodingPolicy, ParseRoundTrip) {
  Preset p;
  ASSERT_TRUE(parsePreset("LANCrisp", &p));       EXPECT_EQ(p, Preset::LANCrisp);
  ASSERT_TRUE(parsePreset("Balanced", &p));       EXPECT_EQ(p, Preset::Balanced);
  ASSERT_TRUE(parsePreset("LowBandwidth", &p));   EXPECT_EQ(p, Preset::LowBandwidth);
  ASSERT_TRUE(parsePreset("VideoOptimized", &p)); EXPECT_EQ(p, Preset::VideoOptimized);
  ASSERT_TRUE(parsePreset("Custom", &p));         EXPECT_EQ(p, Preset::Custom);
  // Case-insensitive
  ASSERT_TRUE(parsePreset("balanced", &p));       EXPECT_EQ(p, Preset::Balanced);
  ASSERT_TRUE(parsePreset("VIDEOOPTIMIZED", &p)); EXPECT_EQ(p, Preset::VideoOptimized);
  // Junk
  EXPECT_FALSE(parsePreset("turbo", &p));
  EXPECT_FALSE(parsePreset("", &p));
  EXPECT_FALSE(parsePreset(nullptr, &p));
}

// -=- Tuning sanity ----------------------------------------------------

TEST(EncodingPolicy, TuningOrdering) {
  // JPEG quality should decrease as the preset gets stingier.
  EXPECT_GT(tuningFor(Preset::LANCrisp).jpegQuality,
            tuningFor(Preset::Balanced).jpegQuality);
  EXPECT_GT(tuningFor(Preset::Balanced).jpegQuality,
            tuningFor(Preset::LowBandwidth).jpegQuality);
  // Compression should go in the opposite direction.
  EXPECT_LT(tuningFor(Preset::LANCrisp).tightCompression,
            tuningFor(Preset::LowBandwidth).tightCompression);
  // H.264 must be off for the lossless preset.
  EXPECT_FALSE(tuningFor(Preset::LANCrisp).h264Enabled);
  // and on for the video preset.
  EXPECT_TRUE(tuningFor(Preset::VideoOptimized).h264Enabled);
}

TEST(EncodingPolicy, CustomPresetIsNeutral) {
  // Custom is supposed to leave existing knobs alone; the negative
  // numbers signal "don't override".
  PresetTuning c = tuningFor(Preset::Custom);
  EXPECT_LT(c.jpegQuality, 0);
  EXPECT_LT(c.tightCompression, 0);
  EXPECT_LT(c.zrleCompression, 0);
  EXPECT_FALSE(c.h264Enabled);
}

// -=- pickEncoder behaviour --------------------------------------------

TEST(EncodingPolicy, TinyRectAlwaysSimple) {
  // 8x8 == area 64 -> tiny. With Tight available -> Tight; without ->
  // Raw. Independent of preset.
  RectStats tiny{}; tiny.width = 8; tiny.height = 8;
  tiny.paletteSize = 4; tiny.hasGradient = false;

  EXPECT_EQ(pickEncoder(Preset::Balanced, tiny, fullCaps()),
            RecommendedEncoder::Tight);
  EXPECT_EQ(pickEncoder(Preset::LANCrisp, tiny, fullCaps()),
            RecommendedEncoder::Tight);
  EXPECT_EQ(pickEncoder(Preset::VideoOptimized, tiny, fullCaps()),
            RecommendedEncoder::Tight);
  EXPECT_EQ(pickEncoder(Preset::Balanced, tiny, bareCaps()),
            RecommendedEncoder::Raw);
}

TEST(EncodingPolicy, EmptyRectDeferred) {
  RectStats r{}; r.width = 0; r.height = 100;
  EXPECT_EQ(pickEncoder(Preset::Balanced, r, fullCaps()),
            RecommendedEncoder::Auto);
}

TEST(EncodingPolicy, PhotoBalancedPicksTightJpeg) {
  EXPECT_EQ(pickEncoder(Preset::Balanced, photoRect(), fullCaps()),
            RecommendedEncoder::TightJPEG);
}

TEST(EncodingPolicy, PhotoLowBandwidthDoesNotPickH264WithoutMotion) {
  // LowBandwidth has h264Enabled=true, but a photo with fps==1 is
  // not motion -- should fall back to TightJPEG.
  RectStats r = photoRect();
  r.recentChangeFps = 1;
  EXPECT_EQ(pickEncoder(Preset::LowBandwidth, r, fullCaps()),
            RecommendedEncoder::TightJPEG);
}

TEST(EncodingPolicy, VideoOptimizedPicksH264WhenSupported) {
  EXPECT_EQ(pickEncoder(Preset::VideoOptimized, videoRect(), fullCaps()),
            RecommendedEncoder::H264);
}

TEST(EncodingPolicy, VideoFallsBackWhenClientLacksH264) {
  ClientCaps c = fullCaps();
  c.supportsH264 = false;
  // Should NOT recommend H.264; should pick TightJPEG instead.
  EXPECT_EQ(pickEncoder(Preset::VideoOptimized, videoRect(), c),
            RecommendedEncoder::TightJPEG);
}

TEST(EncodingPolicy, LANCrispNeverPicksH264) {
  ClientCaps c = fullCaps();
  EXPECT_EQ(pickEncoder(Preset::LANCrisp, videoRect(), c),
            RecommendedEncoder::TightJPEG);
}

TEST(EncodingPolicy, IndexedSmallPaletteTight) {
  RectStats r = uiRect(400, 300);  // 120k area; not tiny
  r.paletteSize = 32;
  r.hasGradient = false;
  EXPECT_EQ(pickEncoder(Preset::Balanced, r, fullCaps()),
            RecommendedEncoder::Tight);
}

TEST(EncodingPolicy, IndexedLargerPaletteZRLE) {
  RectStats r = uiRect(400, 300);
  r.paletteSize = 200;       // > 96 -> ZRLE branch
  r.hasGradient = false;
  EXPECT_EQ(pickEncoder(Preset::Balanced, r, fullCaps()),
            RecommendedEncoder::ZRLE);
}

TEST(EncodingPolicy, FullColourNonGradientDefersToAuto) {
  RectStats r = uiRect(400, 300);
  r.paletteSize = 257;       // truecolour
  r.hasGradient = false;
  EXPECT_EQ(pickEncoder(Preset::Balanced, r, fullCaps()),
            RecommendedEncoder::Auto);
}

TEST(EncodingPolicy, H264NeedsLargeArea) {
  // 100x100 video frames are too small to be worth H.264's overhead.
  RectStats r = videoRect(100, 100, 30);
  EXPECT_NE(pickEncoder(Preset::VideoOptimized, r, fullCaps()),
            RecommendedEncoder::H264);
}

// -=- Diagnostics ------------------------------------------------------

TEST(EncodingPolicy, DiagnosticsAccumulate) {
  Diagnostics d;
  resetDiagnostics(&d);
  EXPECT_EQ(d.totalFrames, 0u);

  recordFrame(&d, RecommendedEncoder::Tight, 1234, 50);
  recordFrame(&d, RecommendedEncoder::Tight, 2222, 70);
  recordFrame(&d, RecommendedEncoder::TightJPEG, 9999, 200);

  EXPECT_EQ(d.totalFrames, 3u);
  EXPECT_EQ(d.frames[(int)RecommendedEncoder::Tight], 2u);
  EXPECT_EQ(d.bytes[(int)RecommendedEncoder::Tight], 1234u + 2222u);
  EXPECT_EQ(d.encodeTimeUs[(int)RecommendedEncoder::Tight], 50u + 70u);
  EXPECT_EQ(d.frames[(int)RecommendedEncoder::TightJPEG], 1u);
  EXPECT_EQ(d.fallbacks, 0u);

  recordFallback(&d, RecommendedEncoder::H264, RecommendedEncoder::TightJPEG);
  EXPECT_EQ(d.fallbacks, 1u);
}

TEST(EncodingPolicy, DiagnosticsSummaryOmitsZeroes) {
  Diagnostics d;
  resetDiagnostics(&d);
  recordFrame(&d, RecommendedEncoder::Tight, 100, 1);
  recordFrame(&d, RecommendedEncoder::Tight, 100, 1);
  recordFrame(&d, RecommendedEncoder::H264, 9999, 999);
  recordFallback(&d, RecommendedEncoder::H264, RecommendedEncoder::TightJPEG);
  std::string s = diagnosticsSummary(d);
  EXPECT_NE(s.find("Tight=2"), std::string::npos);
  EXPECT_NE(s.find("H264=1"), std::string::npos);
  EXPECT_NE(s.find("fallbacks=1"), std::string::npos);
  // Encoders we never used should not appear.
  EXPECT_EQ(s.find("Raw="), std::string::npos);
  EXPECT_EQ(s.find("ZRLE="), std::string::npos);
}

TEST(EncodingPolicy, DiagnosticsRejectOutOfRange) {
  Diagnostics d;
  resetDiagnostics(&d);
  // Sentinel value -- must not corrupt counters.
  recordFrame(&d, RecommendedEncoder::Max, 100, 100);
  EXPECT_EQ(d.totalFrames, 0u);
}
