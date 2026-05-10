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

// CaptureBackendWgc.h -- Windows.Graphics.Capture (WinRT) backend.
//
// Phase 1: scaffold. The full implementation is intentionally deferred
// because:
//
//   * It requires WinRT (Windows.Graphics.Capture, Windows 10 1903+),
//     which means dragging in C++/WinRT or hand-rolled IInspectable
//     wrappers.
//   * It always shows a system "share this monitor / window" yellow
//     border on Win 10 < 21H2; on 21H2+ the border can be suppressed
//     but still requires user-consent semantics (no silent service
//     capture).
//   * The WGC path is the right answer for *user-initiated* "share my
//     screen" sessions, NOT for unattended VNC server capture.
//
// initialize() detects WinRT availability and returns NotSupported so
// the factory falls back. Phase 2 will pick this up when we add a
// "user-mode interactive" capture path.

#ifndef __RFB_WIN32_CAPTURE_BACKEND_WGC_H__
#define __RFB_WIN32_CAPTURE_BACKEND_WGC_H__

#include <rfb_win32/CaptureBackend.h>

namespace rfb {
namespace win32 {
namespace capture {

class WindowsGraphicsCaptureBackend : public CaptureBackend {
public:
  WindowsGraphicsCaptureBackend();
  ~WindowsGraphicsCaptureBackend() override;

  BackendInfo info() const override;
  std::vector<MonitorInfo> enumerateMonitors() const override;
  CaptureStatus initialize(int monitorIndex) override;
  CaptureStatus captureNextFrame(uint32_t timeoutMs,
                                 const uint8_t** pixels,
                                 int* strideBytes,
                                 FrameMeta* meta) override;
  void shutdown() override;

  // Probe whether Windows.Graphics.Capture is available. The canonical
  // check is GraphicsCaptureSession.IsSupported() but that requires
  // WinRT. Phase 1 uses a build-number heuristic (Win 10 1903 = 18362).
  static bool probe();

private:
  bool initialized_;
};

}  // namespace capture
}  // namespace win32
}  // namespace rfb

#endif  // __RFB_WIN32_CAPTURE_BACKEND_WGC_H__
