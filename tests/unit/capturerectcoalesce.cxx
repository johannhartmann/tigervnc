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

#include <rfb/CaptureRectCoalesce.h>

using core::Rect;
using rfb::capture::coalesceRects;

namespace {

// Order-independent helper: do the two vectors describe the same set of
// rectangles? coalesceRects's output order is implementation-defined.
bool sameSet(std::vector<Rect> a, std::vector<Rect> b) {
  auto cmp = [](const Rect& l, const Rect& r) {
    if (l.tl.x != r.tl.x) return l.tl.x < r.tl.x;
    if (l.tl.y != r.tl.y) return l.tl.y < r.tl.y;
    if (l.br.x != r.br.x) return l.br.x < r.br.x;
    return l.br.y < r.br.y;
  };
  std::sort(a.begin(), a.end(), cmp);
  std::sort(b.begin(), b.end(), cmp);
  return a == b;
}

}  // namespace

TEST(CaptureRectCoalesce, Empty) {
  EXPECT_EQ(coalesceRects({}).size(), 0u);
}

TEST(CaptureRectCoalesce, Single) {
  std::vector<Rect> in = {Rect(0, 0, 10, 10)};
  EXPECT_EQ(coalesceRects(in), in);
}

TEST(CaptureRectCoalesce, EmptyRectDropped) {
  // is_empty() rects (width or height <= 0) get dropped silently.
  std::vector<Rect> in = {Rect(5, 5, 5, 5),     // zero area
                          Rect(10, 10, 20, 20)};  // real
  auto out = coalesceRects(in);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], Rect(10, 10, 20, 20));
}

TEST(CaptureRectCoalesce, FarApartKeptSeparate) {
  // Two 10x10 rects 1000 pixels apart. Bounding box would be ~1010x1010
  // = ~1.02M pixels vs. 200 pixels of actual content. Ratio explodes,
  // shouldn't merge under any sane growth bound.
  std::vector<Rect> in = {Rect(0, 0, 10, 10),
                          Rect(1000, 1000, 1010, 1010)};
  auto out = coalesceRects(in, 1.5);
  EXPECT_TRUE(sameSet(out, in));
}

TEST(CaptureRectCoalesce, OverlappingMerged) {
  std::vector<Rect> in = {Rect(0, 0, 20, 20),
                          Rect(10, 10, 30, 30)};
  auto out = coalesceRects(in, 1.5);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], Rect(0, 0, 30, 30));
}

TEST(CaptureRectCoalesce, AdjacentMerged) {
  // Edge-touching: union has zero wasted area, growth ratio = 1.0
  // which the function permits even at the lowest legal bound.
  std::vector<Rect> in = {Rect(0, 0, 10, 10),
                          Rect(10, 0, 20, 10)};
  auto out = coalesceRects(in, 1.0);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], Rect(0, 0, 20, 10));
}

TEST(CaptureRectCoalesce, GrowthBoundEnforced) {
  // Two 10x10 rects diagonally arranged so the union is 20x20 = 400
  // pixels, sum-of-areas is 200. Growth ratio = 2.0.
  std::vector<Rect> in = {Rect(0, 0, 10, 10),
                          Rect(10, 10, 20, 20)};

  // ratio bound 1.5: must NOT merge.
  auto strict = coalesceRects(in, 1.5);
  EXPECT_TRUE(sameSet(strict, in));

  // ratio bound 2.0: now permitted (boundary case is inclusive).
  auto relaxed = coalesceRects(in, 2.0);
  ASSERT_EQ(relaxed.size(), 1u);
  EXPECT_EQ(relaxed[0], Rect(0, 0, 20, 20));
}

TEST(CaptureRectCoalesce, ChainCoalesces) {
  // Three rects in a horizontal strip, each touching the next. Greedy
  // merge sweeps them into one row.
  std::vector<Rect> in = {Rect(0,  0, 10, 10),
                          Rect(10, 0, 20, 10),
                          Rect(20, 0, 30, 10)};
  auto out = coalesceRects(in, 1.0);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], Rect(0, 0, 30, 10));
}

TEST(CaptureRectCoalesce, ClampsBelowOne) {
  // maxGrowthRatio < 1.0 is geometrically nonsensical; the function
  // clamps it to 1.0 rather than refusing to merge anything.
  std::vector<Rect> in = {Rect(0, 0, 10, 10),
                          Rect(10, 0, 20, 10)};
  auto out = coalesceRects(in, 0.5);  // clamped to 1.0
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], Rect(0, 0, 20, 10));
}

TEST(CaptureRectCoalesce, NeverGrowsCount) {
  // Property: |output| <= |input| for any growth ratio.
  std::vector<Rect> in = {
      Rect(0,    0,    100,  100),
      Rect(50,   50,   150,  150),
      Rect(200,  200,  300,  300),
      Rect(1000, 1000, 1100, 1100),
      Rect(1050, 1050, 1150, 1150),
  };
  for (double r : {1.0, 1.25, 1.5, 2.0, 5.0}) {
    auto out = coalesceRects(in, r);
    EXPECT_LE(out.size(), in.size())
        << "ratio=" << r << " grew the count";
  }
}
