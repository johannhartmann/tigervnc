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

// CaptureBackend.h
//
// Windows screen capture backend abstraction. Phase 1 of the capture
// modernization effort: defines the interface, factory, supporting types,
// and the runtime selection mechanism. The full DXGI Desktop Duplication
// and Windows.Graphics.Capture implementations land in follow-up patches;
// the GDI backend remains a marker for the legacy SDisplay path which
// continues to handle real capture for now.
//
// The abstraction is intentionally shaped so that future SDisplay code
// can talk to one CaptureBackend instance per session, query its
// capabilities, and consume frames + dirty-rect metadata + cursor info
// without caring which backend is wired in. See doc/windows-capture-
// backends.md for the design notes and manual test matrix.

#ifndef __RFB_WIN32_CAPTURE_BACKEND_H__
#define __RFB_WIN32_CAPTURE_BACKEND_H__

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <core/Rect.h>  // declares core::Point and core::Rect

namespace rfb {
namespace win32 {
namespace capture {

// -=- Backend identity --------------------------------------------------

enum class BackendKind {
  Auto = 0,   // factory chooses; never reported as the resolved kind
  Dxgi,       // DXGI Desktop Duplication (D3D11 / IDXGIOutputDuplication)
  Wgc,        // Windows.Graphics.Capture (WinRT, user-consent)
  Gdi,        // BitBlt-based legacy path
};

// Stable string names ("auto", "dxgi", "wgc", "gdi") -- safe to use in
// config files and logs.
const char* backendKindName(BackendKind kind);

// Parse a case-insensitive name. Returns false if the string doesn't
// match a known backend.
bool parseBackendKind(const char* str, BackendKind* out);

struct BackendInfo {
  BackendKind kind;
  const char* displayName;       // e.g. "DxgiDesktopDuplication"
  bool worksOnSecureDesktop;     // can capture across UAC / login screen
  bool requiresUserConsent;      // OS shows a "share this..." prompt
  const char* description;       // human-readable one-liner for logs
};

// -=- Monitor enumeration -----------------------------------------------

struct MonitorInfo {
  int index;                    // stable index within an enumerate() call
  std::string deviceName;       // \\.\DISPLAY1 form, ASCII
  core::Rect virtualRect;       // virtual-desktop coords, pixels
  int dpiX;                     // 96 if unknown / not queried
  int dpiY;                     // 96 if unknown / not queried
  int rotationDegrees;          // 0, 90, 180, 270
  bool isPrimary;
};

// Enumerate currently-attached monitors using EnumDisplayMonitors. Always
// safe to call regardless of which backend has been selected -- backends
// may share this implementation via the helper in CaptureBackend.cxx.
std::vector<MonitorInfo> enumerateMonitorsCommon();

// -=- Capture status ----------------------------------------------------

enum class CaptureStatus {
  Ok = 0,
  Timeout,             // captureNextFrame waited longer than timeoutMs
  AccessLost,          // backend must be re-initialised (e.g. mode switch)
  DisplayChanged,      // monitor topology changed (hotplug, rotation, ...)
  SecureDesktopActive, // UAC / Ctrl+Alt+Del visible; backend can't capture
  NotSupported,        // backend cannot run on this host or is not yet
                       // implemented; caller should fall back
  Error,               // unrecoverable
};

const char* captureStatusName(CaptureStatus s);

// -=- Frame metadata ----------------------------------------------------

struct DirtyRects {
  // Areas of the captured frame that changed since the previous one.
  // Coordinates are in the captured surface's coordinate system (i.e.
  // pixel-aligned, 0,0 at the top-left of the captured monitor or
  // virtual desktop).
  std::vector<core::Rect> dirty;

  // Move rectangles: contents at `srcOrigin` were copied to `destRect`.
  // Backends that don't expose move information leave this empty and
  // describe the same change as entries in `dirty`.
  std::vector<std::pair<core::Rect, core::Point>> moves;

  // Set if the backend cannot describe the change incrementally and the
  // entire frame must be re-encoded. `dirty` and `moves` may be empty.
  bool fullFrame = false;
};

struct PointerShape {
  bool valid = false;            // false = no shape data this frame
  core::Point hotspot;           // relative to the rect's top-left
  int width = 0;
  int height = 0;
  // BGRA premultiplied. size() == width*height*4 when valid.
  std::vector<uint8_t> bgra;
};

struct FrameMeta {
  DirtyRects rects;
  bool pointerVisible = false;   // pointerPos is meaningful iff true
  core::Point pointerPos;        // virtual-desktop coords
  bool pointerShapeChanged = false;
  PointerShape pointerShape;     // valid only when pointerShapeChanged
};

// -=- The interface itself ----------------------------------------------

class CaptureBackend {
public:
  virtual ~CaptureBackend() = default;

  // Identity. Stable across the object's lifetime.
  virtual BackendInfo info() const = 0;

  // Enumerate currently-attached monitors. Safe to call before / after /
  // independent of initialize().
  virtual std::vector<MonitorInfo> enumerateMonitors() const = 0;

  // Initialise capture for a specific monitor.
  //
  //   monitorIndex == -1   primary monitor
  //   monitorIndex == -2   entire virtual desktop
  //   monitorIndex >= 0    matches MonitorInfo::index from enumerate()
  //
  // Returns Ok if the backend is ready to capture, NotSupported if the
  // backend cannot run on this host (caller should fall back), or Error
  // for genuine failures.
  virtual CaptureStatus initialize(int monitorIndex) = 0;

  // Block up to `timeoutMs` for the next frame. On Ok:
  //   *pixels      points at BGRA pixels valid until the next call;
  //                ownership stays with the backend, the caller copies
  //                what it needs.
  //   *strideBytes is the byte stride between rows.
  //   *meta        is populated with dirty rects, cursor data, etc.
  //
  // On Timeout / DisplayChanged / AccessLost the *pixels output is
  // unspecified and *meta is left untouched.
  virtual CaptureStatus captureNextFrame(uint32_t timeoutMs,
                                         const uint8_t** pixels,
                                         int* strideBytes,
                                         FrameMeta* meta) = 0;

  // Lifecycle hooks. Called by SDisplay (in phase 2) when the
  // corresponding system event fires. Default no-op so simple backends
  // don't have to implement them.
  virtual void onDisplayChanged() {}
  virtual void onSessionChanged() {}  // sleep / resume / lock / unlock

  // Tear down. Idempotent. The backend may be reinitialised afterwards.
  virtual void shutdown() = 0;
};

// -=- Factory -----------------------------------------------------------

// Construct a CaptureBackend.
//
//   preferred == Auto: try Dxgi, then Wgc, then Gdi -- the first whose
//                      initialize(-1) returns Ok wins.
//   otherwise:         try the requested kind first; if its initialize
//                      returns NotSupported, fall back through the auto
//                      sequence. If it returns Error, surface the error
//                      directly (no fallback for hard failures).
//
// `*resolved` (if non-null) is set to the kind that was actually selected
// so the caller can log it. Always returns a non-null backend; the worst
// case is a Gdi marker backend whose captureNextFrame returns
// NotSupported.
std::unique_ptr<CaptureBackend>
makeCaptureBackend(BackendKind preferred,
                   BackendKind* resolved = nullptr);

// -=- Configuration -----------------------------------------------------

// "auto" / "dxgi" / "wgc" / "gdi". Phase 2 wires this into SDisplay.
extern BackendKind configuredBackendKind;

// Update configuredBackendKind from a string. Returns false on bad input
// without modifying the existing value.
bool setConfiguredBackendKind(const char* str);

}  // namespace capture
}  // namespace win32
}  // namespace rfb

#endif  // __RFB_WIN32_CAPTURE_BACKEND_H__
