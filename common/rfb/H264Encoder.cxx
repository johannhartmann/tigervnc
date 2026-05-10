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

#include <stdexcept>

#include <core/LogWriter.h>

#include <rdr/OutStream.h>
#include <rfb/H264Encoder.h>
#include <rfb/H264EncoderContext.h>
#include <rfb/PixelBuffer.h>
#include <rfb/SConnection.h>
#include <rfb/encodings.h>

namespace rfb {

static core::LogWriter vlog("H264Encoder");

// Same cap as H264Decoder. Stateful per-rect contexts are expensive
// (each one holds an MFT instance + sample buffers) and a runaway
// caller could otherwise leak them indefinitely.
static const size_t kMaxContexts = 64;

// RFB H.264 frame-header flag bits.
namespace {
constexpr uint32_t kFlagResetContext     = 0x1;
constexpr uint32_t kFlagResetAllContexts = 0x2;
}  // namespace

H264Encoder::H264Encoder(SConnection* conn_)
    : Encoder(conn_, encodingH264, EncoderUseNativePF) {}

H264Encoder::~H264Encoder() {
  resetContexts();
}

bool H264Encoder::isSupported() {
  if (!conn->client.supportsEncoding(encodingH264))
    return false;
#if defined(H264_WIN)
  return true;
#else
  return false;
#endif
}

H264EncoderContext* H264Encoder::findContext(const core::Rect& r) {
  for (H264EncoderContext* ctx : contexts)
    if (ctx->isEqualRect(r))
      return ctx;
  return nullptr;
}

void H264Encoder::resetContexts() {
  for (H264EncoderContext* ctx : contexts)
    delete ctx;
  contexts.clear();
}

void H264Encoder::writeRect(const PixelBuffer* pb,
                             const Palette& /*palette*/) {
  core::Rect r = pb->getRect();

  // For now we require 32-bpp pixels with B in byte 0 (BGRA / BGRX) --
  // that's what the Windows framebuffer ships and what the NV12
  // converter assumes. Anything else falls back to an empty payload
  // so the caller surfaces the issue rather than corrupting pixels.
  const PixelFormat& pf = pb->getPF();
  if (pf.bpp != 32) {
    vlog.error("H.264 encoder needs 32-bpp pixels (got %d)", pf.bpp);
    rdr::OutStream* os = conn->getOutStream();
    os->writeU32(0);
    os->writeU32(kFlagResetContext);
    return;
  }

  H264EncoderContext* ctx = findContext(r);
  if (!ctx) {
    if (contexts.size() >= kMaxContexts) {
      // Evict the oldest context. Same policy as H264Decoder.
      H264EncoderContext* old = contexts.front();
      delete old;
      contexts.pop_front();
    }
    ctx = H264EncoderContext::createContext(r);
    if (!ctx) {
      // Platform encoder unavailable. Emit an empty H.264 payload with
      // ResetContext so any prior state for this rect is discarded;
      // EncodeManager has already decided H.264 is the encoder, so we
      // can't change our mind here without rewinding the stream.
      vlog.error("H.264 encoder context unavailable for %dx%d rect",
                 r.width(), r.height());
      rdr::OutStream* os = conn->getOutStream();
      os->writeU32(0);
      os->writeU32(kFlagResetContext);
      return;
    }
    contexts.push_back(ctx);
  }

  int stride;
  const uint8_t* src = pb->getBuffer(r, &stride);

  std::vector<uint8_t> nal;
  bool keyFrame = false;
  if (!ctx->encode(src, stride * (pf.bpp / 8), &nal, &keyFrame)) {
    vlog.error("H.264 encode failed for %dx%d rect; dropping context",
               r.width(), r.height());
    contexts.remove(ctx);
    delete ctx;
    rdr::OutStream* os = conn->getOutStream();
    os->writeU32(0);
    os->writeU32(kFlagResetContext);
    return;
  }

  // Wire format: length, flags, NALs.
  rdr::OutStream* os = conn->getOutStream();
  os->writeU32((uint32_t)nal.size());
  os->writeU32(keyFrame ? kFlagResetContext : 0);
  if (!nal.empty())
    os->writeBytes(nal.data(), nal.size());
}

void H264Encoder::writeSolidRect(int /*width*/, int /*height*/,
                                  const PixelFormat& /*pf*/,
                                  const uint8_t* /*colour*/) {
  // Solid rects are tiny and uniform -- there's nothing for H.264 to
  // gain over Tight's Solid sub-encoder. EncodeManager never routes
  // solid rects to us in practice (the "Solid" type is the first
  // checked in writeSubRect), but the interface still requires the
  // method. Throw so we'd notice if the routing changes.
  throw std::runtime_error(
      "H264Encoder::writeSolidRect should not be reached -- solid rects "
      "go through Tight's Solid sub-encoder");
}

}  // namespace rfb
