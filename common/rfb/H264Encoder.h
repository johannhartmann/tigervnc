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

// H264Encoder.h
//
// Server-side H.264 encoder. Mirrors the H264Decoder structure:
// keeps a list of per-region H264EncoderContexts (one stateful encoder
// instance per dirty-rect coordinates) so consecutive updates of the
// same region can share an H.264 stream and emit P-frames.
//
// On the wire each rectangle is the standard RFB H.264 framing:
//
//     uint32 length            -- byte length of the NAL units
//     uint32 flags             -- bit 0 = ResetContext,
//                                 bit 1 = ResetAllContexts
//     <length> bytes           -- Annex-B byte-stream H.264 NAL units
//
// An I-frame sets ResetContext so the viewer can safely drop any
// previously-buffered state for that region.
//
// Currently only the Windows Media Foundation backend is implemented.
// On Linux / macOS isSupported() returns false and EncodeManager falls
// back to TightJPEG. See doc/encoding-policy.md for the gap status.

#ifndef __RFB_H264ENCODER_H__
#define __RFB_H264ENCODER_H__

#include <list>

#include <core/Rect.h>

#include <rfb/Encoder.h>

namespace rfb {

  class H264EncoderContext;

  class H264Encoder : public Encoder {
  public:
    H264Encoder(SConnection* conn);
    ~H264Encoder() override;

    bool isSupported() override;

    void writeRect(const PixelBuffer* pb,
                   const Palette& palette) override;
    void writeSolidRect(int width, int height,
                        const PixelFormat& pf,
                        const uint8_t* colour) override;

  private:
    H264EncoderContext* findContext(const core::Rect& r);
    void resetContexts();

    std::list<H264EncoderContext*> contexts;
  };

}

#endif  // __RFB_H264ENCODER_H__
