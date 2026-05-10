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

#include <rfb/CaptureRectCoalesce.h>

#include <algorithm>
#include <cstdint>

namespace rfb {
namespace capture {

namespace {

inline int64_t rectArea(const core::Rect& r) {
  if (r.is_empty()) return 0;
  return static_cast<int64_t>(r.width()) *
         static_cast<int64_t>(r.height());
}

inline core::Rect unionRect(const core::Rect& a, const core::Rect& b) {
  // Component-wise outer bounding box. Both inputs are expected to be
  // non-empty; callers filter empties out before calling.
  return core::Rect(std::min(a.tl.x, b.tl.x),
                    std::min(a.tl.y, b.tl.y),
                    std::max(a.br.x, b.br.x),
                    std::max(a.br.y, b.br.y));
}

}  // namespace

std::vector<core::Rect>
coalesceRects(const std::vector<core::Rect>& in,
              double maxGrowthRatio) {
  // Defensive lower bound: maxGrowthRatio < 1.0 makes no geometric
  // sense (a union can't be smaller than its largest input). Clamp.
  if (maxGrowthRatio < 1.0)
    maxGrowthRatio = 1.0;

  // Drop empty rects up front.
  std::vector<core::Rect> work;
  work.reserve(in.size());
  for (const auto& r : in)
    if (!r.is_empty())
      work.push_back(r);

  // Greedy fixed-point merge. For each pair (i, j) with j > i, if the
  // bounding-box growth ratio is acceptable, merge j into i and drop j.
  // Restart the inner loop because the merged rect may now combine with
  // earlier candidates.
  //
  // O(n^3) worst case but n is typically small (DXGI frames cap dirty
  // rects at low hundreds; coalescing usually drops it below a dozen).
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < work.size(); ++i) {
      for (size_t j = i + 1; j < work.size();) {
        core::Rect u = unionRect(work[i], work[j]);
        int64_t areaSum = rectArea(work[i]) + rectArea(work[j]);
        int64_t areaU = rectArea(u);
        if (areaSum > 0 &&
            static_cast<double>(areaU) <=
                static_cast<double>(areaSum) * maxGrowthRatio) {
          work[i] = u;
          work.erase(work.begin() + j);
          changed = true;
          // continue j at same index to test the new neighbour
        } else {
          ++j;
        }
      }
    }
  }

  return work;
}

}  // namespace capture
}  // namespace rfb
