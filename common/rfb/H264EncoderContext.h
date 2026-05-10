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

// H264EncoderContext.h
//
// Abstract per-region H.264 encoder. Mirrors the H264DecoderContext
// pattern: createContext() picks the platform-specific implementation
// at runtime; on Windows builds with HAVE_H264 a Media Foundation H.264
// encoder MFT-backed context is returned. Linux / macOS currently
// return nullptr -- documented gap, see doc/encoding-policy.md.

#ifndef __RFB_H264ENCODERCONTEXT_H__
#define __RFB_H264ENCODERCONTEXT_H__

#include <stdint.h>

#include <vector>

#include <core/Rect.h>

namespace rfb {

  class H264EncoderContext {
  public:
    // Platform factory. Returns nullptr if H.264 encoding isn't built
    // for this platform, or if the platform encoder failed to initialise
    // (driver missing, MFT not registered, etc.). Caller falls back to
    // a classic encoder.
    static H264EncoderContext* createContext(const core::Rect& r);

    virtual ~H264EncoderContext() = 0;

    // Encode a single frame.
    //
    //   bgra        pointer to source pixels in BGRA / BGRX byte order
    //               (the native Windows framebuffer layout). The caller
    //               passes one row of (rect.width()*4) bytes per stride.
    //   strideBytes byte stride between rows.
    //   out         appended NAL units of the produced H.264 stream
    //               (Annex-B byte-stream form). Cleared by the callee
    //               first; the caller wraps the buffer in the RFB H.264
    //               header (length + flags) when emitting on the wire.
    //   keyFrame    set to true on output for an I-frame; false for a
    //               P-frame. The caller uses this to set the
    //               ResetContext flag in the RFB H.264 frame header so
    //               the decoder knows it can drop prior state.
    //
    // Returns true on success. False signals a permanent failure --
    // caller should drop the context and fall back.
    virtual bool encode(const uint8_t* bgra, int strideBytes,
                        std::vector<uint8_t>* out, bool* keyFrame) = 0;

    inline bool isEqualRect(const core::Rect& r) const { return r == rect; }

  protected:
    core::Rect rect;
    H264EncoderContext(const core::Rect& r) : rect(r) {}
  };

}

#endif  // __RFB_H264ENCODERCONTEXT_H__
