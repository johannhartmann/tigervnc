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
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <vector>

namespace rfb {
namespace win32 {
namespace capture {

static core::LogWriter vlog("DxgiCapture");

namespace {

template <typename T>
inline void safeRelease(T*& obj) {
  if (obj) {
    obj->Release();
    obj = nullptr;
  }
}

// Translate selectMonitorIndex (-1 primary, -2 virtual desktop, >=0
// matching MonitorInfo::index) into the DXGI adapter+output indices.
struct OutputSelection {
  IDXGIAdapter1* adapter;
  IDXGIOutput*   output;
  DXGI_OUTPUT_DESC desc;
};

bool pickOutput(int monitorIndex, OutputSelection* out) {
  out->adapter = nullptr;
  out->output  = nullptr;

  IDXGIFactory1* factory = nullptr;
  HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory));
  if (FAILED(hr) || !factory) {
    vlog.error("CreateDXGIFactory1 failed (hr=0x%08lx)", hr);
    return false;
  }

  // We enumerate monitors in the same order EnumDisplayMonitors does --
  // i.e. across all adapters concatenated. monitorIndex == -1 picks the
  // primary monitor's adapter+output. monitorIndex == -2 (virtual
  // desktop) is approximated as the primary monitor for now; phase-3
  // multi-monitor work can extend this.
  bool found = false;
  int globalIdx = 0;

  for (UINT a = 0; ; ++a) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND)
      break;
    if (!adapter)
      continue;

    for (UINT o = 0; ; ++o) {
      IDXGIOutput* output = nullptr;
      if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND)
        break;
      if (!output)
        continue;

      DXGI_OUTPUT_DESC desc = {};
      if (FAILED(output->GetDesc(&desc))) {
        output->Release();
        continue;
      }

      bool match = false;
      if (monitorIndex == -1 || monitorIndex == -2) {
        // Primary heuristic: the output whose monitor info has the
        // PRIMARY flag.
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (desc.Monitor && GetMonitorInfoW(desc.Monitor, &mi) &&
            (mi.dwFlags & MONITORINFOF_PRIMARY)) {
          match = true;
        }
      } else if (globalIdx == monitorIndex) {
        match = true;
      }
      ++globalIdx;

      if (match) {
        out->adapter = adapter;
        out->output  = output;
        out->desc    = desc;
        found        = true;
        // Don't release adapter/output -- we hand ownership to caller.
        // Drop everything else.
        // Reset adapter pointer so the outer release loop skips it.
        adapter = nullptr;
        output  = nullptr;
        break;
      }

      output->Release();
    }

    if (adapter)
      adapter->Release();
    if (found)
      break;
  }

  factory->Release();
  return found;
}

// DXGI_OUTDUPL_MOVE_RECT.SourcePoint -> core::Point;
// DXGI_OUTDUPL_MOVE_RECT.DestinationRect -> core::Rect.
inline core::Point asPoint(const POINT& p) {
  return core::Point(p.x, p.y);
}
inline core::Rect asRect(const RECT& r) {
  return core::Rect(r.left, r.top, r.right, r.bottom);
}

}  // namespace

DxgiDesktopDuplicationBackend::DxgiDesktopDuplicationBackend()
    : monitorIndex_(-1),
      initialized_(false),
      device_(nullptr),
      context_(nullptr),
      duplication_(nullptr),
      staging_(nullptr),
      frameAcquired_(false),
      lastPixels_(nullptr),
      lastStride_(0),
      stagingMapped_(false) {}

DxgiDesktopDuplicationBackend::~DxgiDesktopDuplicationBackend() {
  shutdown();
}

BackendInfo DxgiDesktopDuplicationBackend::info() const {
  return {
      BackendKind::Dxgi,
      "DxgiDesktopDuplication",
      false,  // can't capture across the secure desktop (UAC, login)
      false,  // no user-consent prompt
      "DXGI Desktop Duplication (D3D11) -- preferred unattended path",
  };
}

std::vector<MonitorInfo> DxgiDesktopDuplicationBackend::enumerateMonitors() const {
  return enumerateMonitorsCommon();
}

bool DxgiDesktopDuplicationBackend::probe() {
  HMODULE m = LoadLibraryW(L"d3d11.dll");
  if (m == nullptr) {
    vlog.debug("d3d11.dll not loadable (err=%lu)", GetLastError());
    return false;
  }
  FreeLibrary(m);
  return true;
}

CaptureStatus DxgiDesktopDuplicationBackend::initialize(int monitorIndex) {
  if (initialized_) {
    vlog.debug("Re-initializing DXGI for monitor %d", monitorIndex);
    destroyResources();
  }
  monitorIndex_ = monitorIndex;

  if (!probe())
    return CaptureStatus::NotSupported;

  // 1. Create a D3D11 device. We don't need any specific feature level
  // for plain BGRA capture; D3D_FEATURE_LEVEL_9_1 is the floor and any
  // adapter that implements DXGI 1.2 supports at least 10_0.
  static const D3D_FEATURE_LEVEL kLevels[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
  HRESULT hr = D3D11CreateDevice(
      nullptr,                          // default adapter
      D3D_DRIVER_TYPE_HARDWARE, nullptr,
      0,                                // no debug layer in production
      kLevels, ARRAYSIZE(kLevels), D3D11_SDK_VERSION,
      &device_, &got, &context_);
  if (FAILED(hr)) {
    vlog.error("D3D11CreateDevice failed (hr=0x%08lx)", hr);
    destroyResources();
    return CaptureStatus::Error;
  }
  vlog.debug("D3D11 device created at feature level 0x%04x", got);

  // 2. Pick the adapter+output for the requested monitor. Note the
  // adapter we pick may not be the same one the device was created on,
  // since EnumAdapters / DuplicateOutput don't require the device's
  // adapter -- AcquireNextFrame will route correctly either way.
  OutputSelection sel = {};
  if (!pickOutput(monitorIndex, &sel)) {
    vlog.error("No DXGI output matches monitor index %d", monitorIndex);
    destroyResources();
    return CaptureStatus::Error;
  }
  outputRect_ = core::Rect(sel.desc.DesktopCoordinates.left,
                           sel.desc.DesktopCoordinates.top,
                           sel.desc.DesktopCoordinates.right,
                           sel.desc.DesktopCoordinates.bottom);

  // 3. Get IDXGIOutput1 and start duplication.
  IDXGIOutput1* output1 = nullptr;
  hr = sel.output->QueryInterface(__uuidof(IDXGIOutput1),
                                  reinterpret_cast<void**>(&output1));
  sel.output->Release();
  sel.adapter->Release();
  if (FAILED(hr) || !output1) {
    vlog.error("IDXGIOutput1 not available (hr=0x%08lx)", hr);
    destroyResources();
    return CaptureStatus::Error;
  }
  hr = output1->DuplicateOutput(device_, &duplication_);
  output1->Release();
  if (FAILED(hr) || !duplication_) {
    vlog.error("IDXGIOutput1::DuplicateOutput failed (hr=0x%08lx)", hr);
    destroyResources();
    // Common cause: another process already holds the duplication, or
    // we're on the secure desktop. Both surface as E_ACCESSDENIED /
    // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE.
    return CaptureStatus::AccessLost;
  }

  // 4. Create a staging texture sized to the output. Phase-2 doesn't
  // attempt incremental partial copies; the staging is reused for every
  // frame and we copy the whole captured texture into it. Phase-3 can
  // extend this to copy only the dirty rectangles.
  D3D11_TEXTURE2D_DESC staging = {};
  staging.Width = outputRect_.width();
  staging.Height = outputRect_.height();
  staging.MipLevels = 1;
  staging.ArraySize = 1;
  staging.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  staging.SampleDesc.Count = 1;
  staging.Usage = D3D11_USAGE_STAGING;
  staging.BindFlags = 0;
  staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging.MiscFlags = 0;
  hr = device_->CreateTexture2D(&staging, nullptr, &staging_);
  if (FAILED(hr) || !staging_) {
    vlog.error("CreateTexture2D(staging) failed (hr=0x%08lx)", hr);
    destroyResources();
    return CaptureStatus::Error;
  }

  initialized_ = true;
  vlog.info("DXGI Desktop Duplication initialized for monitor %d "
            "(%dx%d at %d,%d)",
            monitorIndex, outputRect_.width(), outputRect_.height(),
            outputRect_.tl.x, outputRect_.tl.y);
  return CaptureStatus::Ok;
}

CaptureStatus DxgiDesktopDuplicationBackend::captureNextFrame(
    uint32_t timeoutMs, const uint8_t** pixels, int* strideBytes,
    FrameMeta* meta) {
  if (!initialized_ || !duplication_)
    return CaptureStatus::Error;

  // Unmap any previous frame's staging texture before requesting the
  // next one -- D3D11 disallows concurrent maps.
  if (stagingMapped_) {
    context_->Unmap(staging_, 0);
    stagingMapped_ = false;
    lastPixels_ = nullptr;
    lastStride_ = 0;
  }
  releaseFrameIfHeld();

  DXGI_OUTDUPL_FRAME_INFO info = {};
  IDXGIResource* desktopRes = nullptr;
  HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &info, &desktopRes);

  if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    return CaptureStatus::Timeout;

  if (hr == DXGI_ERROR_ACCESS_LOST) {
    vlog.info("AcquireNextFrame returned ACCESS_LOST; caller should "
              "re-initialize");
    safeRelease(desktopRes);
    return CaptureStatus::AccessLost;
  }

  if (FAILED(hr) || !desktopRes) {
    vlog.error("AcquireNextFrame failed (hr=0x%08lx)", hr);
    safeRelease(desktopRes);
    return CaptureStatus::Error;
  }

  frameAcquired_ = true;

  // Copy the desktop image into the staging texture. AccumulatedFrames
  // == 0 with no dirty rects means a metadata-only update (e.g. cursor
  // moved without screen content changing); we still produce a frame
  // for the pointer-update path but skip the costly readback.
  ID3D11Texture2D* desktopTex = nullptr;
  hr = desktopRes->QueryInterface(__uuidof(ID3D11Texture2D),
                                  reinterpret_cast<void**>(&desktopTex));
  desktopRes->Release();
  if (FAILED(hr) || !desktopTex) {
    vlog.error("Desktop resource is not a Texture2D (hr=0x%08lx)", hr);
    releaseFrameIfHeld();
    return CaptureStatus::Error;
  }

  bool hasContent = info.LastPresentTime.QuadPart != 0 ||
                    info.AccumulatedFrames > 0 ||
                    info.TotalMetadataBufferSize > 0;
  if (hasContent) {
    context_->CopyResource(staging_, desktopTex);
  }
  desktopTex->Release();

  // 5. Pull dirty rects + move rects out of the duplication metadata.
  // The buffer sizes are reported by AcquireNextFrame in
  // info.TotalMetadataBufferSize.
  meta->rects.dirty.clear();
  meta->rects.moves.clear();
  meta->rects.fullFrame = false;

  if (info.TotalMetadataBufferSize > 0) {
    std::vector<uint8_t> metaBuf(info.TotalMetadataBufferSize);

    UINT moveBytes = 0;
    if (SUCCEEDED(duplication_->GetFrameMoveRects(
            (UINT)metaBuf.size(),
            reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metaBuf.data()),
            &moveBytes))) {
      auto* moves =
          reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metaBuf.data());
      size_t n = moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
      meta->rects.moves.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        meta->rects.moves.emplace_back(asRect(moves[i].DestinationRect),
                                       asPoint(moves[i].SourcePoint));
      }
    }

    UINT dirtyBytes = 0;
    if (SUCCEEDED(duplication_->GetFrameDirtyRects(
            (UINT)metaBuf.size(),
            reinterpret_cast<RECT*>(metaBuf.data()),
            &dirtyBytes))) {
      auto* dirty = reinterpret_cast<RECT*>(metaBuf.data());
      size_t n = dirtyBytes / sizeof(RECT);
      meta->rects.dirty.reserve(n);
      for (size_t i = 0; i < n; ++i)
        meta->rects.dirty.push_back(asRect(dirty[i]));
    }
  } else if (hasContent) {
    // The OS is telling us "something changed" without describing what.
    // Force a full-frame update.
    meta->rects.fullFrame = true;
  }

  // 6. Pointer position + (optionally) shape.
  meta->pointerVisible = info.PointerPosition.Visible != 0;
  if (info.PointerPosition.Visible) {
    meta->pointerPos = core::Point(
        outputRect_.tl.x + info.PointerPosition.Position.x,
        outputRect_.tl.y + info.PointerPosition.Position.y);
  }

  meta->pointerShapeChanged = false;
  if (info.PointerShapeBufferSize > 0) {
    pointerShapeBuf_.resize(info.PointerShapeBufferSize);
    DXGI_OUTDUPL_POINTER_SHAPE_INFO psi = {};
    UINT used = 0;
    if (SUCCEEDED(duplication_->GetFramePointerShape(
            (UINT)pointerShapeBuf_.size(), pointerShapeBuf_.data(),
            &used, &psi))) {
      meta->pointerShape.valid = true;
      meta->pointerShape.hotspot =
          core::Point(psi.HotSpot.x, psi.HotSpot.y);
      meta->pointerShape.width = psi.Width;
      // For COLOR cursors, height is the bitmap height; for MONOCHROME
      // the buffer encodes AND+XOR masks stacked vertically (height/2
      // pixels each). Phase-2 surfaces COLOR shapes faithfully and
      // leaves monochrome / masked-color decoding to phase-3.
      meta->pointerShape.height =
          (psi.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME)
              ? psi.Height / 2
              : psi.Height;
      meta->pointerShape.bgra.assign(pointerShapeBuf_.begin(),
                                     pointerShapeBuf_.begin() + used);
      meta->pointerShapeChanged = true;
    }
  }

  // 7. Map the staging texture so the caller can read pixels.
  if (hasContent) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
      vlog.error("Map(staging) failed (hr=0x%08lx)", hr);
      releaseFrameIfHeld();
      return CaptureStatus::Error;
    }
    stagingMapped_ = true;
    lastPixels_ = static_cast<const uint8_t*>(mapped.pData);
    lastStride_ = static_cast<int>(mapped.RowPitch);
  }
  // else: cursor-only update, no pixel map. lastPixels_ stays whatever
  // it was; caller is expected to consult meta->rects.dirty before
  // reading from *pixels.

  *pixels = lastPixels_;
  *strideBytes = lastStride_;
  return CaptureStatus::Ok;
}

void DxgiDesktopDuplicationBackend::onDisplayChanged() {
  // Topology change forces a re-init -- the duplication is bound to a
  // specific output that may no longer exist.
  if (initialized_) {
    vlog.info("Display topology changed, tearing down DXGI duplication");
    destroyResources();
    initialized_ = false;
  }
}

void DxgiDesktopDuplicationBackend::onSessionChanged() {
  // Sleep / resume / lock: AcquireNextFrame will start returning
  // ACCESS_LOST. Tear down so the next captureNextFrame() prompts the
  // caller to re-initialize cleanly.
  if (initialized_) {
    vlog.info("Session changed, tearing down DXGI duplication");
    destroyResources();
    initialized_ = false;
  }
}

void DxgiDesktopDuplicationBackend::releaseFrameIfHeld() {
  if (frameAcquired_ && duplication_) {
    duplication_->ReleaseFrame();
    frameAcquired_ = false;
  }
}

void DxgiDesktopDuplicationBackend::destroyResources() {
  if (stagingMapped_ && context_ && staging_) {
    context_->Unmap(staging_, 0);
  }
  stagingMapped_ = false;
  lastPixels_ = nullptr;
  lastStride_ = 0;

  releaseFrameIfHeld();

  safeRelease(staging_);
  safeRelease(duplication_);
  safeRelease(context_);
  safeRelease(device_);
}

CaptureStatus DxgiDesktopDuplicationBackend::recreateAfterAccessLost() {
  destroyResources();
  initialized_ = false;
  return initialize(monitorIndex_);
}

void DxgiDesktopDuplicationBackend::shutdown() {
  destroyResources();
  initialized_ = false;
}

}  // namespace capture
}  // namespace win32
}  // namespace rfb
