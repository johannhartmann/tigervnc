/* Copyright (C) 2000-2003 Constantin Kaplinsky.  All Rights Reserved.
 * Copyright (C) 2011 D. R. Commander.  All Rights Reserved.
 * Copyright 2014-2022 Pierre Ossman for Cendio AB
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
#ifndef __RFB_ENCODEMANAGER_H__
#define __RFB_ENCODEMANAGER_H__

#include <chrono>
#include <deque>
#include <vector>

#include <stdint.h>

#include <core/Configuration.h>
#include <core/Region.h>
#include <core/Timer.h>

#include <rfb/EncodingPolicy.h>
#include <rfb/PixelBuffer.h>

namespace rfb {

  class SConnection;
  class Encoder;
  class UpdateInfo;
  class PixelBuffer;
  class RenderedCursor;

  struct RectInfo;

  class EncodeManager : public core::Timer::Callback {
  public:
    EncodeManager(SConnection* conn);
    ~EncodeManager();

    void logStats();

    // Operator-facing knob. Selects an adaptive encoding preset that
    // tunes JPEG quality / compression level / (future) H.264 enable.
    // "Custom" (default) leaves existing behaviour untouched. See
    // doc/encoding-policy.md.
    static core::EnumParameter encodingPreset;

    // Log the adaptive-policy diagnostics summary every N frames (0
    // disables mid-connection logging; the summary still appears once
    // at connection close via logStats()).
    static core::IntParameter policyLogInterval;

    // Hack to let ConnParams calculate the client's preferred encoding
    static bool supported(int encoding);

    bool needsLosslessRefresh(const core::Region& req);
    int getNextLosslessRefresh(const core::Region& req);

    void pruneLosslessRefresh(const core::Region& limits);

    void forceRefresh(const core::Region& req);

    void writeUpdate(const UpdateInfo& ui, const PixelBuffer* pb,
                     const RenderedCursor* renderedCursor);

    void writeLosslessRefresh(const core::Region& req,
                              const PixelBuffer* pb,
                              const RenderedCursor* renderedCursor,
                              size_t maxUpdateSize);

  protected:
    void handleTimeout(core::Timer* t) override;

    void doUpdate(bool allowLossy, const core::Region& changed,
                  const core::Region& copied,
                  const core::Point& copy_delta,
                  const PixelBuffer* pb,
                  const RenderedCursor* renderedCursor);
    void prepareEncoders(bool allowLossy);

    core::Region getLosslessRefresh(const core::Region& req,
                                    size_t maxUpdateSize);

    int computeNumRects(const core::Region& changed);

    Encoder* startRect(const core::Rect& rect, int type);
    void endRect();

    void writeCopyRects(const core::Region& copied,
                        const core::Point& delta);
    void writeSolidRects(core::Region* changed, const PixelBuffer* pb);
    void findSolidRect(const core::Rect& rect, core::Region* changed,
                       const PixelBuffer* pb);
    void writeRects(const core::Region& changed, const PixelBuffer* pb);

    void writeSubRect(const core::Rect& rect, const PixelBuffer* pb);

    bool checkSolidTile(const core::Rect& r, const uint8_t* colourValue,
                        const PixelBuffer *pb);
    void extendSolidAreaByBlock(const core::Rect& r,
                                const uint8_t* colourValue,
                                const PixelBuffer* pb, core::Rect* er);
    void extendSolidAreaByPixel(const core::Rect& r,
                                const core::Rect& sr,
                                const uint8_t* colourValue,
                                const PixelBuffer* pb, core::Rect* er);

    PixelBuffer* preparePixelBuffer(const core::Rect& rect,
                                    const PixelBuffer* pb, bool convert);

    bool analyseRect(const PixelBuffer *pb,
                     struct RectInfo *info, int maxColours);

  protected:
    // Templated, optimised methods
    template<class T>
    inline bool checkSolidTile(int width, int height,
                               const T* buffer, int stride,
                               const T colourValue);
    template<class T>
    inline bool analyseRect(int width, int height,
                            const T* buffer, int stride,
                            struct RectInfo *info, int maxColours);

  protected:
    SConnection *conn;

    std::vector<Encoder*> encoders;
    std::vector<int> activeEncoders;

    core::Region lossyRegion;
    core::Region recentlyChangedRegion;
    core::Region pendingRefreshRegion;

    core::Timer recentChangeTimer;

    struct EncoderStats {
      unsigned rects;
      unsigned long long bytes;
      unsigned long long pixels;
      unsigned long long equivalent;
    };
    typedef std::vector< std::vector<struct EncoderStats> > StatsVector;

    unsigned updates;
    EncoderStats copyStats;
    StatsVector stats;
    int activeType;
    int beforeLength;

    // Per-rectangle accounting populated by startRect()/endRect() and
    // consumed by writeSubRect()'s diagnostic block. Decoupled from
    // the existing stats[][] aggregation because Diagnostics records
    // per-encoder-kind, not per-(class,type).
    std::chrono::steady_clock::time_point rectStartTime;
    int lastRectBytes;
    uint64_t lastRectEncodeUs;

    // Connection-wide recent change rate. Updated once per writeUpdate()
    // -- one frame batch == one "tick", windowed over the last second.
    // Cheap proxy for "is this a busy / video connection?" that the
    // EncodingPolicy uses to gate the H.264 recommendation.
    std::deque<uint64_t> recentUpdateTimesMs;
    int currentChangeFps;

    // Frames seen since the last mid-connection policy summary. Reset
    // when the summary is logged.
    unsigned framesSinceLastPolicyLog;

    // Adaptive-policy diagnostics. Populated per rectangle in
    // writeSubRect(); summarised by logStats(). Per-instance, not
    // thread-safe (matches the per-connection EncodeManager model).
    encoding::Diagnostics encodingDiag;

    class OffsetPixelBuffer : public FullFramePixelBuffer {
    public:
      OffsetPixelBuffer() {}
      virtual ~OffsetPixelBuffer() {}

      void update(const PixelFormat& pf, int width, int height,
                  const uint8_t* data_, int stride);

    private:
      uint8_t* getBufferRW(const core::Rect& r, int* stride) override;
    };

    OffsetPixelBuffer offsetPixelBuffer;
    ManagedPixelBuffer convertedPixelBuffer;
  };

}

#endif
