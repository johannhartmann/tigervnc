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

// H264WinEncoderContext.h
//
// Windows Media Foundation H.264 encoder context. Wraps a synchronous
// IMFTransform configured with NV12 input + H.264 (Annex-B) output.
// One context per dirty rectangle; calling code reuses the context
// across frames so the encoder can emit P-frames.
//
// Compiled in only when H264_WIN is defined (Windows + ENABLE_H264).

#ifndef __RFB_H264WINENCODERCONTEXT_H__
#define __RFB_H264WINENCODERCONTEXT_H__

#include <rfb/H264EncoderContext.h>

struct IMFTransform;
struct IMFSample;
struct IMFMediaBuffer;

namespace rfb {

class H264WinEncoderContext : public H264EncoderContext {
public:
  H264WinEncoderContext(const core::Rect& r);
  ~H264WinEncoderContext() override;

  // Set up the MFT, input/output media types, sample buffers. Returns
  // false if the encoder MFT couldn't be created (driver missing,
  // unsupported geometry, ...). Caller falls back.
  bool initialize();

  bool encode(const uint8_t* bgra, int strideBytes,
              std::vector<uint8_t>* out, bool* keyFrame) override;

private:
  // Hand-rolled BGRA -> NV12. width_/height_ control the output
  // geometry (must be even -- the constructor rounds rect up). Source
  // pixel order is B, G, R, X in bytes 0..3. Output Y plane size is
  // width*height; UV plane is width * height/2 (interleaved U,V at
  // half resolution in both axes).
  void convertBgraToNv12(const uint8_t* bgra, int strideBytes,
                         uint8_t* nv12);

  bool drainOneFrame(std::vector<uint8_t>* out, bool* keyFrame);

  IMFTransform*   encoder_;
  IMFSample*      inputSample_;
  IMFMediaBuffer* inputBuffer_;
  IMFSample*      outputSample_;
  IMFMediaBuffer* outputBuffer_;

  // Frame dimensions rounded up to even (H.264 requires it for NV12).
  int width_;
  int height_;

  // Monotonic frame counter; doubles as the input sample's media-time
  // (in 100-ns units; 33333 == ~30 fps).
  long long frameCount_;

  // Set to true while we're about to submit the very first frame, so
  // we can mark it as a keyframe.
  bool needsKeyFrame_;

  // True iff the encoder's been streamed a START_OF_STREAM message.
  // Necessary because Media Foundation requires the synchronous
  // Start-of-Stream / End-of-Stream bracketing.
  bool streamStarted_;
};

}

#endif  // __RFB_H264WINENCODERCONTEXT_H__
