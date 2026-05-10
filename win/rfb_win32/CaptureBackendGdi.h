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

// CaptureBackendGdi.h -- BitBlt-based fallback backend.
//
// Phase 1: marker only. The legacy capture path lives in SDisplay /
// DeviceFrameBuffer / DIBSectionBuffer and continues to drive pixel
// acquisition. initialize() reports Ok so the factory's auto walk
// always terminates here, and captureNextFrame() reports NotSupported
// because no caller exercises the new interface yet.
//
// Phase 2 will move the BitBlt code under this class so SDisplay can
// route through CaptureBackend uniformly.

#ifndef __RFB_WIN32_CAPTURE_BACKEND_GDI_H__
#define __RFB_WIN32_CAPTURE_BACKEND_GDI_H__

#include <rfb_win32/CaptureBackend.h>

namespace rfb {
namespace win32 {
namespace capture {

class GdiCaptureBackend : public CaptureBackend {
public:
  GdiCaptureBackend();
  ~GdiCaptureBackend() override;

  BackendInfo info() const override;
  std::vector<MonitorInfo> enumerateMonitors() const override;
  CaptureStatus initialize(int monitorIndex) override;
  CaptureStatus captureNextFrame(uint32_t timeoutMs,
                                 const uint8_t** pixels,
                                 int* strideBytes,
                                 FrameMeta* meta) override;
  void shutdown() override;

private:
  bool initialized_;
  int monitorIndex_;
};

}  // namespace capture
}  // namespace win32
}  // namespace rfb

#endif  // __RFB_WIN32_CAPTURE_BACKEND_GDI_H__
