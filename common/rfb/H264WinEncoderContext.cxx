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

#include <rfb/H264WinEncoderContext.h>

#include <core/LogWriter.h>

#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#include <cstring>
#include <cstdint>
#include <vector>

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

namespace rfb {

static core::LogWriter vlog("H264WinEncoder");

namespace {

// Bitrate / framerate / GOP defaults. EncodingPolicy will override
// these via tuning in a follow-up; for now they're conservative
// values that match the VideoOptimized preset's intent.
constexpr UINT32 kDefaultBitrate    = 8000000;   // 8 Mbps
constexpr UINT32 kFrameRateNum      = 30;
constexpr UINT32 kFrameRateDen      = 1;
// 100-ns ticks per frame at 30 fps = ~333333. The MFT consumes media
// time in this unit.
constexpr long long kFrameDuration100ns = 10000000LL / kFrameRateNum;

// Force a keyframe every N frames. Provides resilience against packet
// loss / decoder reset and bounds the worst-case time-to-recover.
constexpr long long kKeyFrameInterval = 60;

// Round up to even number; H.264 requires even dimensions for 4:2:0
// chroma subsampling (NV12).
inline int evenUp(int v) { return (v + 1) & ~1; }

// 8.16 fixed-point BT.601 coefficients for BGRA -> Y/U/V.
//   Y =  0.257*R + 0.504*G + 0.098*B + 16
//   U = -0.148*R - 0.291*G + 0.439*B + 128
//   V =  0.439*R - 0.368*G - 0.071*B + 128
inline uint8_t clampU8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)v;
}

}  // namespace

H264WinEncoderContext::H264WinEncoderContext(const core::Rect& r)
    : H264EncoderContext(r),
      encoder_(nullptr),
      inputSample_(nullptr),
      inputBuffer_(nullptr),
      outputSample_(nullptr),
      outputBuffer_(nullptr),
      width_(evenUp(r.width())),
      height_(evenUp(r.height())),
      frameCount_(0),
      needsKeyFrame_(true),
      streamStarted_(false) {}

H264WinEncoderContext::~H264WinEncoderContext() {
  if (encoder_ && streamStarted_) {
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
  }
  SAFE_RELEASE(outputBuffer_);
  SAFE_RELEASE(outputSample_);
  SAFE_RELEASE(inputBuffer_);
  SAFE_RELEASE(inputSample_);
  SAFE_RELEASE(encoder_);
}

bool H264WinEncoderContext::initialize() {
  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) {
    vlog.error("MFStartup failed (hr=0x%08lx)", hr);
    return false;
  }

  // Create the H.264 encoder MFT directly via CoCreateInstance. We
  // could enumerate MFTs via MFTEnumEx and pick the best one, but the
  // built-in CLSID_CMSH264EncoderMFT is sufficient for everything we
  // need at this scope.
  hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr,
                        CLSCTX_INPROC_SERVER, IID_IMFTransform,
                        reinterpret_cast<void**>(&encoder_));
  if (FAILED(hr) || !encoder_) {
    vlog.info("CLSID_CMSH264EncoderMFT not available (hr=0x%08lx); "
              "Windows H.264 encoder disabled for this connection",
              hr);
    return false;
  }

  // ----- output type: H.264 -----
  IMFMediaType* outputType = nullptr;
  hr = MFCreateMediaType(&outputType);
  if (FAILED(hr) || !outputType) goto fail;

  outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  outputType->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_H264);
  outputType->SetUINT32(MF_MT_AVG_BITRATE, kDefaultBitrate);
  MFSetAttributeSize (outputType, MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(outputType, MF_MT_FRAME_RATE,
                      kFrameRateNum, kFrameRateDen);
  MFSetAttributeRatio(outputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  outputType->SetUINT32(MF_MT_INTERLACE_MODE,
                        MFVideoInterlace_Progressive);
  // Baseline profile is the broadest-compatible. Main is fine too;
  // High requires more decoder support and we don't need 8x8 etc.
  outputType->SetUINT32(MF_MT_MPEG2_PROFILE,
                        eAVEncH264VProfile_Base);

  hr = encoder_->SetOutputType(0, outputType, 0);
  outputType->Release();
  if (FAILED(hr)) {
    vlog.error("SetOutputType failed (hr=0x%08lx)", hr);
    goto fail;
  }

  // ----- input type: NV12 -----
  IMFMediaType* inputType;
  inputType = nullptr;
  hr = MFCreateMediaType(&inputType);
  if (FAILED(hr) || !inputType) goto fail;

  inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  inputType->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_NV12);
  MFSetAttributeSize (inputType, MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(inputType, MF_MT_FRAME_RATE,
                      kFrameRateNum, kFrameRateDen);
  MFSetAttributeRatio(inputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  inputType->SetUINT32(MF_MT_INTERLACE_MODE,
                       MFVideoInterlace_Progressive);

  hr = encoder_->SetInputType(0, inputType, 0);
  inputType->Release();
  if (FAILED(hr)) {
    vlog.error("SetInputType failed (hr=0x%08lx)", hr);
    goto fail;
  }

  // ----- low-latency knob via ICodecAPI -----
  {
    ICodecAPI* codec = nullptr;
    if (SUCCEEDED(encoder_->QueryInterface(IID_ICodecAPI,
                                           reinterpret_cast<void**>(&codec)))
        && codec) {
      VARIANT v;
      VariantInit(&v);
      v.vt = VT_BOOL;
      v.boolVal = VARIANT_TRUE;
      codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
      // CBR rate control. VBR would be better for static content but
      // CBR has predictable bitrate -- the right tradeoff for VNC
      // since the receiving side is wire-bound.
      v.vt = VT_UI4;
      v.ulVal = eAVEncCommonRateControlMode_CBR;
      codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
      v.ulVal = kDefaultBitrate;
      codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
      codec->Release();
    }
  }

  // ----- input + output sample / buffer plumbing -----
  {
    MFT_OUTPUT_STREAM_INFO outInfo{};
    encoder_->GetOutputStreamInfo(0, &outInfo);
    UINT outBufSize = outInfo.cbSize > 0
                          ? outInfo.cbSize
                          : (UINT)(width_ * height_ * 2);

    if (FAILED(MFCreateSample(&inputSample_)) ||
        FAILED(MFCreateSample(&outputSample_)) ||
        FAILED(MFCreateMemoryBuffer(width_ * height_ * 3 / 2,
                                    &inputBuffer_)) ||
        FAILED(MFCreateMemoryBuffer(outBufSize, &outputBuffer_))) {
      vlog.error("Sample / buffer allocation failed");
      goto fail;
    }
    inputSample_->AddBuffer(inputBuffer_);
    outputSample_->AddBuffer(outputBuffer_);
  }

  encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  streamStarted_ = true;

  vlog.debug("H.264 encoder initialised for %dx%d", width_, height_);
  return true;

fail:
  SAFE_RELEASE(outputBuffer_);
  SAFE_RELEASE(outputSample_);
  SAFE_RELEASE(inputBuffer_);
  SAFE_RELEASE(inputSample_);
  SAFE_RELEASE(encoder_);
  return false;
}

void H264WinEncoderContext::convertBgraToNv12(const uint8_t* bgra,
                                              int strideBytes,
                                              uint8_t* nv12) {
  uint8_t* yPlane  = nv12;
  uint8_t* uvPlane = nv12 + (size_t)width_ * height_;

  // Y plane: per-pixel.
  for (int y = 0; y < height_; ++y) {
    const uint8_t* srcRow = bgra + (size_t)y * strideBytes;
    uint8_t* dstY = yPlane + (size_t)y * width_;
    for (int x = 0; x < width_; ++x) {
      int b = srcRow[4 * x + 0];
      int g = srcRow[4 * x + 1];
      int r = srcRow[4 * x + 2];
      int Y = (66 * r + 129 * g + 25 * b + 128) >> 8;
      dstY[x] = clampU8(Y + 16);
    }
  }

  // UV plane: subsampled by 2 in both axes. Average a 2x2 block to
  // produce one (U, V) pair.
  for (int y = 0; y < height_; y += 2) {
    const uint8_t* row0 = bgra + (size_t)y       * strideBytes;
    const uint8_t* row1 = bgra + (size_t)(y + 1) * strideBytes;
    uint8_t* dstUV = uvPlane + (size_t)(y / 2) * width_;
    for (int x = 0; x < width_; x += 2) {
      // Sum of four BGR samples. The /4 is folded into the coefficient.
      int sumB = row0[4 * x + 0] + row0[4 * (x + 1) + 0]
               + row1[4 * x + 0] + row1[4 * (x + 1) + 0];
      int sumG = row0[4 * x + 1] + row0[4 * (x + 1) + 1]
               + row1[4 * x + 1] + row1[4 * (x + 1) + 1];
      int sumR = row0[4 * x + 2] + row0[4 * (x + 1) + 2]
               + row1[4 * x + 2] + row1[4 * (x + 1) + 2];
      // Effective per-pixel via /4 in the divisor of the shifts below.
      int U = (-38 * sumR - 74 * sumG + 112 * sumB + 512 + (128 << 10)) >> 10;
      int V = (112 * sumR - 94 * sumG -  18 * sumB + 512 + (128 << 10)) >> 10;
      dstUV[x]     = clampU8(U);
      dstUV[x + 1] = clampU8(V);
    }
  }
}

bool H264WinEncoderContext::drainOneFrame(std::vector<uint8_t>* out,
                                           bool* keyFrame) {
  out->clear();

  // Loop ProcessOutput until the MFT says it needs more input. Hardware
  // encoders often produce 0 or 1 sample per ProcessInput; software
  // produces exactly one. Either way, accumulate the bytes.
  for (;;) {
    DWORD status = 0;
    MFT_OUTPUT_DATA_BUFFER outBuf{};
    outBuf.dwStreamID = 0;
    outBuf.pSample    = outputSample_;
    outBuf.dwStatus   = 0;
    outBuf.pEvents    = nullptr;

    HRESULT hr = encoder_->ProcessOutput(0, 1, &outBuf, &status);
    SAFE_RELEASE(outBuf.pEvents);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      return true;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      // MFT renegotiating its output type. Re-set output type and try
      // again. For our fixed input geometry this should not happen, but
      // guard against it so we don't loop forever.
      vlog.debug("ProcessOutput returned STREAM_CHANGE; ignoring");
      return out->size() > 0;
    }
    if (FAILED(hr)) {
      vlog.error("ProcessOutput failed (hr=0x%08lx)", hr);
      return false;
    }

    // Was this an I-frame? The MFT marks keyframes with the
    // MF_NALU_KEYFRAME / MFSampleExtension_CleanPoint attribute.
    UINT32 cleanPoint = 0;
    outputSample_->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);
    if (cleanPoint)
      *keyFrame = true;

    BYTE* ptr = nullptr;
    DWORD curLen = 0;
    if (FAILED(outputBuffer_->Lock(&ptr, nullptr, &curLen)))
      return false;
    if (ptr && curLen > 0) {
      size_t prev = out->size();
      out->resize(prev + curLen);
      std::memcpy(out->data() + prev, ptr, curLen);
    }
    outputBuffer_->Unlock();
    outputBuffer_->SetCurrentLength(0);
  }
}

bool H264WinEncoderContext::encode(const uint8_t* bgra, int strideBytes,
                                    std::vector<uint8_t>* out,
                                    bool* keyFrame) {
  if (!encoder_)
    return false;
  out->clear();
  *keyFrame = false;

  // Convert source pixels to NV12 directly into the input buffer.
  BYTE* nv12 = nullptr;
  DWORD maxLen = 0;
  if (FAILED(inputBuffer_->Lock(&nv12, &maxLen, nullptr)) || !nv12) {
    vlog.error("inputBuffer_->Lock failed");
    return false;
  }
  convertBgraToNv12(bgra, strideBytes, nv12);
  size_t nv12Bytes = (size_t)width_ * height_ * 3 / 2;
  inputBuffer_->Unlock();
  inputBuffer_->SetCurrentLength((DWORD)nv12Bytes);

  inputSample_->SetSampleTime(frameCount_ * kFrameDuration100ns);
  inputSample_->SetSampleDuration(kFrameDuration100ns);

  // Periodic keyframes for resilience. Setting CleanPoint on the input
  // sample asks the encoder to emit an I-frame.
  bool requestKey = needsKeyFrame_ ||
                    (frameCount_ % kKeyFrameInterval == 0);
  if (requestKey) {
    inputSample_->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
  } else {
    inputSample_->DeleteItem(MFSampleExtension_CleanPoint);
  }
  needsKeyFrame_ = false;

  HRESULT hr = encoder_->ProcessInput(0, inputSample_, 0);
  if (FAILED(hr)) {
    vlog.error("ProcessInput failed (hr=0x%08lx)", hr);
    return false;
  }

  if (!drainOneFrame(out, keyFrame))
    return false;

  ++frameCount_;
  return true;
}

}  // namespace rfb
