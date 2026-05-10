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

#include <rfb/H264EncoderContext.h>

#if defined(H264_WIN)
#include <rfb/H264WinEncoderContext.h>
#endif

namespace rfb {

H264EncoderContext::~H264EncoderContext() {}

H264EncoderContext* H264EncoderContext::createContext(const core::Rect& r) {
#if defined(H264_WIN)
  auto ctx = new H264WinEncoderContext(r);
  if (!ctx->initialize()) {
    delete ctx;
    return nullptr;
  }
  return ctx;
#else
  // Linux / macOS server-side H.264 encoding is a known gap. The
  // decoder side has libav, but no encoder is wired in yet.
  // doc/encoding-policy.md tracks this.
  (void)r;
  return nullptr;
#endif
}

}  // namespace rfb
