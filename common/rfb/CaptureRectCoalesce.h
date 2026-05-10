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

// CaptureRectCoalesce.h
//
// Helper used by capture backends (DXGI in particular) to compress a
// noisy list of dirty rectangles into something the encoder can consume
// without spraying the wire with hundreds of tiny updates.
//
// DXGI's DesktopDuplication API can hand the application many small
// dirty rects per frame -- one per redrawn glyph for example. Coalescing
// merges rects when the bounding box of the merge isn't much larger
// than the sum of the original areas (controlled by maxGrowthRatio).
// This trades a few redundantly-sent pixels for far fewer encoder calls
// and far fewer rectangles on the wire.
//
// Pure function with no side effects; lives in common/ so it's testable
// from tests/unit/ on Linux/macOS as well as Windows.

#ifndef __RFB_CAPTURE_RECT_COALESCE_H__
#define __RFB_CAPTURE_RECT_COALESCE_H__

#include <vector>

#include <core/Rect.h>

namespace rfb {
namespace capture {

// Merge rectangles that are close enough that a single bounding rect is
// cheaper than two separate ones.
//
//   in              input rectangles in arbitrary order; empty rects are
//                   ignored.
//   maxGrowthRatio  upper bound on (union_area / sum_of_input_areas) for
//                   a merge to be allowed. 1.0 means "only merge if the
//                   union has no extra area", i.e. the rects already
//                   cover the union (overlap or share an edge with no
//                   uncovered slack). Larger values let the function
//                   merge even when some pixels in the union aren't
//                   actually dirty, which is the desired tradeoff for
//                   DXGI's many-small-rects pattern. Default 1.5 means
//                   "allow up to 50% wasted pixels in exchange for one
//                   less rectangle".
//
// Returns a new vector. Output is never longer than input. If any input
// rect is empty (width or height <= 0) it's dropped silently.
std::vector<core::Rect>
coalesceRects(const std::vector<core::Rect>& in,
              double maxGrowthRatio = 1.5);

}  // namespace capture
}  // namespace rfb

#endif  // __RFB_CAPTURE_RECT_COALESCE_H__
