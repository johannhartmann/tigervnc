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

#include <rfb_win32/CaptureBackendGdi.h>

#include <core/LogWriter.h>

namespace rfb {
namespace win32 {
namespace capture {

static core::LogWriter vlog("GdiCapture");

GdiCaptureBackend::GdiCaptureBackend()
    : initialized_(false), monitorIndex_(-1) {}

GdiCaptureBackend::~GdiCaptureBackend() {
  shutdown();
}

BackendInfo GdiCaptureBackend::info() const {
  return {
      BackendKind::Gdi,
      "Gdi",
      // GDI BitBlt cannot capture the secure desktop (UAC / login) at all,
      // and inside a service it cannot capture the interactive session
      // without session-0 trickery. Both behaviors are unchanged from the
      // shipping codebase.
      false,
      false,
      "BitBlt-based legacy capture (handled by SDisplay in phase 1)",
  };
}

std::vector<MonitorInfo> GdiCaptureBackend::enumerateMonitors() const {
  return enumerateMonitorsCommon();
}

CaptureStatus GdiCaptureBackend::initialize(int monitorIndex) {
  monitorIndex_ = monitorIndex;
  initialized_ = true;
  // The legacy BitBlt path lives in SDisplay / DeviceFrameBuffer and is
  // unaffected by this object existing. Phase 2 moves it here.
  vlog.debug("GDI backend marker initialized for monitor %d; pixel "
             "acquisition still flows through SDisplay (phase 1).",
             monitorIndex);
  return CaptureStatus::Ok;
}

CaptureStatus GdiCaptureBackend::captureNextFrame(uint32_t /*timeoutMs*/,
                                                   const uint8_t** /*pixels*/,
                                                   int* /*strideBytes*/,
                                                   FrameMeta* /*meta*/) {
  // Phase 1: the new interface has no caller yet. Return NotSupported so
  // any accidental caller falls back cleanly rather than reading garbage.
  return CaptureStatus::NotSupported;
}

void GdiCaptureBackend::shutdown() {
  initialized_ = false;
}

}  // namespace capture
}  // namespace win32
}  // namespace rfb
