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

// CaptureBackendDxgi.h -- DXGI Desktop Duplication backend.
//
// Phase 1: scaffold. probe() loads d3d11.dll dynamically and returns
// whether the runtime is available. initialize() and captureNextFrame()
// return NotSupported so the factory falls back to GDI; the real
// IDXGIOutputDuplication wiring lands in a follow-up patch. The point
// of phase 1 is to lock down the public interface, the runtime probe,
// and the logging shape so the implementation can land in isolation.

#ifndef __RFB_WIN32_CAPTURE_BACKEND_DXGI_H__
#define __RFB_WIN32_CAPTURE_BACKEND_DXGI_H__

#include <rfb_win32/CaptureBackend.h>

namespace rfb {
namespace win32 {
namespace capture {

class DxgiDesktopDuplicationBackend : public CaptureBackend {
public:
  DxgiDesktopDuplicationBackend();
  ~DxgiDesktopDuplicationBackend() override;

  BackendInfo info() const override;
  std::vector<MonitorInfo> enumerateMonitors() const override;
  CaptureStatus initialize(int monitorIndex) override;
  CaptureStatus captureNextFrame(uint32_t timeoutMs,
                                 const uint8_t** pixels,
                                 int* strideBytes,
                                 FrameMeta* meta) override;
  void shutdown() override;

  // Probe whether the DXGI Desktop Duplication runtime appears available
  // on this host without initialising any D3D11 resources. Returns true
  // if d3d11.dll is loadable, which on Windows 10+ is essentially always
  // true. Phase 2 may extend this to attempt a minimal D3D11 device
  // creation.
  static bool probe();

private:
  bool initialized_;
};

}  // namespace capture
}  // namespace win32
}  // namespace rfb

#endif  // __RFB_WIN32_CAPTURE_BACKEND_DXGI_H__
