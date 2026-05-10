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
// On modern Windows builds (TIGERVNC_WINDOWS_TARGET=modern, i.e.
// _WIN32_WINNT >= 0x0602) this backend has a working IDXGIOutputDuplication
// implementation: D3D11 device, output enumeration, AcquireNextFrame,
// dirty / move rectangle parsing, separate pointer-shape channel, CPU
// readback through a staging texture.
//
// On the legacy build profile the DXGI 1.2 APIs aren't declared, so
// initialize() reports NotSupported and the factory falls back to GDI.

#ifndef __RFB_WIN32_CAPTURE_BACKEND_DXGI_H__
#define __RFB_WIN32_CAPTURE_BACKEND_DXGI_H__

#include <rfb_win32/CaptureBackend.h>

// Forward declarations to keep d3d11.h / dxgi1_2.h out of public headers.
// Defined in <Unknwn.h> as `struct` aliases for the COM interfaces.
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGIOutputDuplication;

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
  void onDisplayChanged() override;
  void onSessionChanged() override;
  void shutdown() override;

  // Probe whether the DXGI Desktop Duplication runtime appears available
  // on this host without initialising any D3D11 resources. Returns true
  // if d3d11.dll is loadable.
  static bool probe();

private:
  void releaseFrameIfHeld();
  void destroyResources();
  CaptureStatus recreateAfterAccessLost();

  // -1 means "not yet initialised"; otherwise the requested monitor
  // index (or -1 = primary, -2 = virtual desktop).
  int monitorIndex_;
  bool initialized_;

  // Selected monitor's virtual-desktop rect (matches what
  // enumerateMonitors() reports for this index).
  core::Rect outputRect_;

  // Backing D3D11 / DXGI objects. Raw pointers because the COM lifetime
  // is hand-managed via destroyResources(); ComPtr / WRL would be
  // cleaner but adds another header dependency.
  ID3D11Device* device_;
  ID3D11DeviceContext* context_;
  IDXGIOutputDuplication* duplication_;
  ID3D11Texture2D* staging_;

  // True between AcquireNextFrame Ok and ReleaseFrame -- used so
  // shutdown / error recovery can balance Acquire with a Release.
  bool frameAcquired_;

  // The last successfully mapped staging-texture pointer + stride.
  // Pointer is non-null only between successful captureNextFrame() and
  // the next call (each call unmaps the previous one).
  const uint8_t* lastPixels_;
  int lastStride_;
  bool stagingMapped_;

  // Pointer-shape buffer reused across frames.
  std::vector<uint8_t> pointerShapeBuf_;
};

}  // namespace capture
}  // namespace win32
}  // namespace rfb

#endif  // __RFB_WIN32_CAPTURE_BACKEND_DXGI_H__
