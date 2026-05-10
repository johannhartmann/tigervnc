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

#include <rfb_win32/CaptureBackendDxgi.h>

#include <core/LogWriter.h>

#include <windows.h>

namespace rfb {
namespace win32 {
namespace capture {

static core::LogWriter vlog("DxgiCapture");

DxgiDesktopDuplicationBackend::DxgiDesktopDuplicationBackend()
    : initialized_(false) {}

DxgiDesktopDuplicationBackend::~DxgiDesktopDuplicationBackend() {
  shutdown();
}

BackendInfo DxgiDesktopDuplicationBackend::info() const {
  return {
      BackendKind::Dxgi,
      "DxgiDesktopDuplication",
      // IDXGIOutputDuplication cannot capture across the secure desktop
      // (UAC / Ctrl+Alt+Del / login screen) -- captureNextFrame returns
      // SecureDesktopActive in that state.
      false,
      false,
      "DXGI Desktop Duplication (D3D11) -- preferred unattended path",
  };
}

std::vector<MonitorInfo> DxgiDesktopDuplicationBackend::enumerateMonitors() const {
  // EnumDisplayMonitors gives the same view DXGI will see.
  return enumerateMonitorsCommon();
}

bool DxgiDesktopDuplicationBackend::probe() {
  // Minimal probe: can the DXGI / D3D11 runtime be loaded at all? On
  // Windows 10 + 11 this is essentially always true. Phase 2 will
  // extend this to attempt a transient D3D11CreateDevice / CreateDXGIFactory1
  // so we can detect headless / hardware-disabled cases too.
  HMODULE m = LoadLibraryW(L"d3d11.dll");
  if (m == nullptr) {
    vlog.debug("d3d11.dll not loadable (err=%lu)", GetLastError());
    return false;
  }
  FreeLibrary(m);
  return true;
}

CaptureStatus DxgiDesktopDuplicationBackend::initialize(int /*monitorIndex*/) {
  if (!probe()) {
    vlog.info("DXGI runtime not available on this host; backend disabled");
    return CaptureStatus::NotSupported;
  }
  vlog.info("DXGI runtime detected; real capture path scheduled for "
            "phase 2 -- factory will fall back");
  return CaptureStatus::NotSupported;
}

CaptureStatus DxgiDesktopDuplicationBackend::captureNextFrame(
    uint32_t /*timeoutMs*/, const uint8_t** /*pixels*/,
    int* /*strideBytes*/, FrameMeta* /*meta*/) {
  return CaptureStatus::NotSupported;
}

void DxgiDesktopDuplicationBackend::shutdown() {
  initialized_ = false;
}

}  // namespace capture
}  // namespace win32
}  // namespace rfb
