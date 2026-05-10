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

#include <rfb_win32/CaptureBackend.h>

#include <rfb_win32/CaptureBackendDxgi.h>
#include <rfb_win32/CaptureBackendGdi.h>
#include <rfb_win32/CaptureBackendWgc.h>

#include <core/Configuration.h>
#include <core/LogWriter.h>

#include <windows.h>
// shellscalingapi.h is Win 8.1+; we resolve GetDpiForMonitor via
// GetProcAddress instead so the legacy build profile still compiles.

#include <cstring>
#include <set>
#include <string>

namespace rfb {
namespace win32 {
namespace capture {

static core::LogWriter vlog("CaptureBackend");

BackendKind configuredBackendKind = BackendKind::Auto;

// User-facing knob. winvnc / vncconfig pick this up via the standard
// Configuration plumbing; SDisplay reads it in phase 3 when it actually
// switches to consume CaptureBackend. Until then, setting this changes
// the factory's selection but doesn't affect the running capture path.
namespace {
core::EnumParameter captureBackendParam(
    "CaptureBackend",
    "Windows screen capture backend (auto, dxgi, wgc, gdi). 'auto' "
    "tries dxgi first, then wgc, then falls back to gdi.",
    {"auto", "dxgi", "wgc", "gdi"}, "auto");
}  // namespace

// -=- Naming -----------------------------------------------------------

const char* backendKindName(BackendKind kind) {
  switch (kind) {
    case BackendKind::Auto: return "auto";
    case BackendKind::Dxgi: return "dxgi";
    case BackendKind::Wgc:  return "wgc";
    case BackendKind::Gdi:  return "gdi";
  }
  return "unknown";
}

static int ascii_tolower(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
  return c;
}

static bool ieq(const char* a, const char* b) {
  while (*a && *b) {
    if (ascii_tolower(*a) != ascii_tolower(*b)) return false;
    ++a; ++b;
  }
  return *a == 0 && *b == 0;
}

bool parseBackendKind(const char* str, BackendKind* out) {
  if (!str || !out) return false;
  if (ieq(str, "auto")) { *out = BackendKind::Auto; return true; }
  if (ieq(str, "dxgi")) { *out = BackendKind::Dxgi; return true; }
  if (ieq(str, "wgc"))  { *out = BackendKind::Wgc;  return true; }
  if (ieq(str, "gdi"))  { *out = BackendKind::Gdi;  return true; }
  return false;
}

bool setConfiguredBackendKind(const char* str) {
  BackendKind k;
  if (!parseBackendKind(str, &k)) {
    vlog.error("Unknown CaptureBackend value '%s'; keeping '%s'",
               str ? str : "(null)",
               backendKindName(configuredBackendKind));
    return false;
  }
  configuredBackendKind = k;
  vlog.info("CaptureBackend configured: %s", backendKindName(k));
  return true;
}

const char* captureStatusName(CaptureStatus s) {
  switch (s) {
    case CaptureStatus::Ok:                  return "Ok";
    case CaptureStatus::Timeout:             return "Timeout";
    case CaptureStatus::AccessLost:          return "AccessLost";
    case CaptureStatus::DisplayChanged:      return "DisplayChanged";
    case CaptureStatus::SecureDesktopActive: return "SecureDesktopActive";
    case CaptureStatus::NotSupported:        return "NotSupported";
    case CaptureStatus::Error:               return "Error";
  }
  return "unknown";
}

// -=- Monitor enumeration ----------------------------------------------

namespace {

// MDT_EFFECTIVE_DPI = 0 in shellscalingapi.h; we redeclare locally so we
// don't need that header on legacy builds.
constexpr UINT kMdtEffectiveDpi = 0;
typedef HRESULT (WINAPI *GetDpiForMonitor_t)(HMONITOR, UINT, UINT*, UINT*);

GetDpiForMonitor_t loadGetDpiForMonitor() {
  // Resolved once per process. Returns nullptr on Windows 8 or older,
  // where GetDpiForMonitor doesn't exist; callers fall back to 96 DPI.
  static GetDpiForMonitor_t fn = []() -> GetDpiForMonitor_t {
    HMODULE m = LoadLibraryW(L"shcore.dll");
    if (!m) return nullptr;
    return reinterpret_cast<GetDpiForMonitor_t>(
        GetProcAddress(m, "GetDpiForMonitor"));
  }();
  return fn;
}

int rotationFromDmDisplayOrientation(DWORD orient) {
  switch (orient) {
    case DMDO_DEFAULT: return 0;
    case DMDO_90:      return 90;
    case DMDO_180:     return 180;
    case DMDO_270:     return 270;
    default:           return 0;
  }
}

struct EnumCtx {
  std::vector<MonitorInfo>* out;
  int nextIndex;
};

BOOL CALLBACK enumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
  EnumCtx* ctx = reinterpret_cast<EnumCtx*>(lParam);

  MONITORINFOEXA mi;
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoA(hMon, &mi))
    return TRUE;

  MonitorInfo info;
  info.index = ctx->nextIndex++;
  info.deviceName = mi.szDevice;  // \\.\DISPLAY1 etc.
  info.virtualRect = core::Rect(mi.rcMonitor.left, mi.rcMonitor.top,
                                mi.rcMonitor.right, mi.rcMonitor.bottom);

  // DPI: Win 8.1+ via GetDpiForMonitor. Older builds report 96/96 like
  // GDI's stretch behaviour assumes.
  info.dpiX = 96;
  info.dpiY = 96;
  if (auto getDpi = loadGetDpiForMonitor()) {
    UINT dx = 96, dy = 96;
    if (SUCCEEDED(getDpi(hMon, kMdtEffectiveDpi, &dx, &dy))) {
      info.dpiX = static_cast<int>(dx);
      info.dpiY = static_cast<int>(dy);
    }
  }

  // Rotation: EnumDisplaySettingsExA against the device name.
  info.rotationDegrees = 0;
  DEVMODEA dm = {};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsExA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm,
                             EDS_ROTATEDMODE) &&
      (dm.dmFields & DM_DISPLAYORIENTATION)) {
    info.rotationDegrees =
        rotationFromDmDisplayOrientation(dm.dmDisplayOrientation);
  }

  info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

  ctx->out->push_back(info);
  return TRUE;
}

}  // namespace

std::vector<MonitorInfo> enumerateMonitorsCommon() {
  std::vector<MonitorInfo> out;
  EnumCtx ctx{&out, 0};
  EnumDisplayMonitors(nullptr, nullptr, enumProc,
                      reinterpret_cast<LPARAM>(&ctx));
  return out;
}

// -=- Factory ---------------------------------------------------------

namespace {

// std::make_unique is C++14; the project is on gnu++11, hence the
// explicit `unique_ptr<T>(new T())` form.
std::unique_ptr<CaptureBackend> makeOne(BackendKind k) {
  switch (k) {
    case BackendKind::Dxgi:
      return std::unique_ptr<CaptureBackend>(
          new DxgiDesktopDuplicationBackend());
    case BackendKind::Wgc:
      return std::unique_ptr<CaptureBackend>(
          new WindowsGraphicsCaptureBackend());
    case BackendKind::Gdi:
      return std::unique_ptr<CaptureBackend>(new GdiCaptureBackend());
    case BackendKind::Auto:
    default:
      return nullptr;
  }
}

// Try to construct + initialize a backend. Returns it on Ok; logs and
// returns nullptr on NotSupported; logs the Error and returns nullptr
// otherwise (factory keeps trying so a hard error in one backend doesn't
// strand the user).
std::unique_ptr<CaptureBackend> tryBackend(BackendKind k) {
  auto b = makeOne(k);
  if (!b)
    return nullptr;

  CaptureStatus st = b->initialize(-1);
  if (st == CaptureStatus::Ok) {
    BackendInfo bi = b->info();
    vlog.info("Selected capture backend: %s (%s)",
              bi.displayName, bi.description);
    return b;
  }

  vlog.info("Capture backend %s rejected initialize: %s",
            backendKindName(k), captureStatusName(st));
  return nullptr;
}

}  // namespace

// Read the configured backend kind: factory caller's argument wins, then
// the configuredBackendKind global (set programmatically), then the
// CaptureBackend EnumParameter (set by config / CLI / registry).
BackendKind resolveConfiguredKind(BackendKind callerArg) {
  if (callerArg != BackendKind::Auto)
    return callerArg;
  if (configuredBackendKind != BackendKind::Auto)
    return configuredBackendKind;
  std::string v = captureBackendParam.getValueStr();
  BackendKind k = BackendKind::Auto;
  parseBackendKind(v.c_str(), &k);
  return k;
}

std::unique_ptr<CaptureBackend>
makeCaptureBackend(BackendKind preferred, BackendKind* resolved) {
  BackendKind effective = resolveConfiguredKind(preferred);

  // Caller asked for a specific backend (directly or via config): try
  // it first. If it returns NotSupported, fall through to auto. If it
  // returns Error, surface it by trying auto next anyway -- we'd rather
  // degrade gracefully than strand the server.
  if (effective != BackendKind::Auto) {
    if (auto b = tryBackend(effective)) {
      if (resolved) *resolved = b->info().kind;
      return b;
    }
    vlog.error("Preferred capture backend %s unavailable; falling back",
               backendKindName(effective));
  }

  // Auto: DXGI -> WGC -> GDI.
  for (BackendKind k :
       {BackendKind::Dxgi, BackendKind::Wgc, BackendKind::Gdi}) {
    if (effective != BackendKind::Auto && k == effective)
      continue;  // already tried above
    if (auto b = tryBackend(k)) {
      if (resolved) *resolved = b->info().kind;
      return b;
    }
  }

  // GDI's initialize() always returns Ok in phase 1, so reaching here
  // would mean a logic error in one of the backends. Defensive fallback.
  vlog.error("All capture backends declined initialize; returning bare "
             "GDI marker so caller has a non-null pointer to talk to");
  auto fallback = makeOne(BackendKind::Gdi);
  if (resolved) *resolved = BackendKind::Gdi;
  return fallback;
}

}  // namespace capture
}  // namespace win32
}  // namespace rfb
