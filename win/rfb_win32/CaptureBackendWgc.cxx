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

#include <rfb_win32/CaptureBackendWgc.h>

#include <core/LogWriter.h>

#include <windows.h>

namespace rfb {
namespace win32 {
namespace capture {

static core::LogWriter vlog("WgcCapture");

WindowsGraphicsCaptureBackend::WindowsGraphicsCaptureBackend()
    : initialized_(false) {}

WindowsGraphicsCaptureBackend::~WindowsGraphicsCaptureBackend() {
  shutdown();
}

BackendInfo WindowsGraphicsCaptureBackend::info() const {
  return {
      BackendKind::Wgc,
      "WindowsGraphicsCapture",
      // WGC cannot capture the secure desktop. It is also not appropriate
      // for unattended service capture: the OS shows the user a
      // "yellow border" sharing indicator on Win 10, and capture must be
      // initiated from a user session.
      false,
      true,
      "Windows.Graphics.Capture (WinRT) -- consent-based, user session "
      "only",
  };
}

std::vector<MonitorInfo> WindowsGraphicsCaptureBackend::enumerateMonitors() const {
  return enumerateMonitorsCommon();
}

bool WindowsGraphicsCaptureBackend::probe() {
  // The official check is winrt::Windows::Graphics::Capture::
  // GraphicsCaptureSession::IsSupported(). Pulling in C++/WinRT just to
  // probe is heavyweight, so phase 1 uses a build-number heuristic.
  // Phase 2 (which actually uses WinRT) will replace this with the real
  // IsSupported() call.
  //
  // RtlGetVersion lives in ntdll.dll and is the only way to get the
  // unspoofed Windows build number from user mode (GetVersionEx is
  // subject to manifest-based version lies).
  typedef LONG (WINAPI *RtlGetVersion_t)(POSVERSIONINFOEXW);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return false;
  RtlGetVersion_t rtl = reinterpret_cast<RtlGetVersion_t>(
      GetProcAddress(ntdll, "RtlGetVersion"));
  if (!rtl) return false;
  RTL_OSVERSIONINFOEXW info = {};
  info.dwOSVersionInfoSize = sizeof(info);
  if (rtl(&info) != 0) return false;

  // Windows 10 1903 == build 18362.
  if (info.dwMajorVersion < 10) return false;
  if (info.dwMajorVersion == 10 && info.dwBuildNumber < 18362) return false;
  return true;
}

CaptureStatus WindowsGraphicsCaptureBackend::initialize(int /*monitorIndex*/) {
  if (!probe()) {
    vlog.info("Windows.Graphics.Capture not available on this host "
              "(needs Win 10 1903 or later); backend disabled");
    return CaptureStatus::NotSupported;
  }
  vlog.info("Windows.Graphics.Capture runtime detected; real capture "
            "path scheduled for phase 2 -- factory will fall back");
  return CaptureStatus::NotSupported;
}

CaptureStatus WindowsGraphicsCaptureBackend::captureNextFrame(
    uint32_t /*timeoutMs*/, const uint8_t** /*pixels*/,
    int* /*strideBytes*/, FrameMeta* /*meta*/) {
  return CaptureStatus::NotSupported;
}

void WindowsGraphicsCaptureBackend::shutdown() {
  initialized_ = false;
}

}  // namespace capture
}  // namespace win32
}  // namespace rfb
