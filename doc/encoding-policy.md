# Encoding policy and presets

TigerVNC's server selects a per-rectangle encoding (Tight, TightJPEG,
ZRLE, Hextile, RRE, Raw, JPEG) based on the rectangle's content and
the client's advertised capabilities. This document describes the
adaptive **encoding policy** module that's been added to centralise
those decisions, the **operator-facing presets** that tune them, and
the **diagnostics** that surface what the server actually picked.

## Status

| Item | Status |
|---|---|
| `rfb::encoding::pickEncoder()` policy function | ✓ |
| `Preset` enum + `tuningFor()` (LANCrisp / Balanced / LowBandwidth / VideoOptimized / Custom) | ✓ |
| `Diagnostics` counters and `diagnosticsSummary()` | ✓ |
| GTest coverage of the decision logic | ✓ (17 cases) |
| `core::EnumParameter "EncodingPreset"` + `PolicyLogInterval` | ✓ |
| `EncodeManager` consumes the policy per rectangle | ✓ |
| `EncodeManager` applies preset's quality / compression knobs | ✓ |
| `Diagnostics::bytes` and `encodeTimeUs` populated per rect | ✓ |
| Connection-wide `recentChangeFps` derivation feeds the H.264 gate | ✓ |
| Periodic mid-connection summary via `PolicyLogInterval` | ✓ |
| Closing summary in `EncodeManager::logStats()` | ✓ |
| **Server-side H.264 encoder on Windows (Media Foundation)** | **✓** |
| Server-side H.264 encoder on Linux | ⚠ gap (libav-based encoder not wired) |
| Server-side H.264 encoder on macOS | ⚠ gap (no libav build by default + no VideoToolbox encoder) |

## H.264 status by platform

**Windows: shipping.** `H264WinEncoderContext` wraps the Media
Foundation H.264 encoder MFT (`CLSID_CMSH264EncoderMFT`). One stateful
encoder context per rect coordinates, NV12 input, Annex-B byte-stream
output, ICodecAPI low-latency + CBR rate-control configuration. Force-
keyframe every 60 frames for resilience. Both x64 and ARM64 build the
encoder and ship it. Picked when:

- `EncodingPreset` resolves to `LowBandwidth` or `VideoOptimized`
  (`tuningFor(preset).h264Enabled == true`), AND
- the client advertised `encodingH264 (50)` in `SetEncodings`.

When both gates pass, `prepareEncoders()` routes the FullColour slot
to the H.264 encoder; indexed/bitmap rects keep going to Tight (which
amortises better at small palettes). Cursor + small UI updates flow
through the existing classic encoders, unchanged.

**Linux: gap.** The decoder side has libav (`H264LibavDecoderContext`)
but no encoder context exists. Adding one would mean: open libav with
`AV_CODEC_ID_H264`, bind a `libx264` or `libopenh264` encoder,
allocate `AVFrame` for NV12 input, encode + drain `AVPacket`s. Roughly
the same shape as `H264LibavDecoderContext.cxx` but in the encode
direction. `H264EncoderContext::createContext` returns nullptr today,
so any H.264 recommendation on a Linux server falls back to TightJPEG
and ticks the `Diagnostics::fallbacks` counter.

**macOS: gap.** Upstream TigerVNC doesn't build libav for macOS by
default (the CMake `find_package(AVCodec)` is best-effort). Adding a
VideoToolbox encoder context (the native Apple H.264 encoder) is the
right path long-term but is out of scope for this fork's
Windows-focused modernization.

## Presets

`Preset::Balanced` is the intended default. Each preset is a coherent
set of tunings that the existing classic encoders + the future H.264
encoder both consume.

| | LANCrisp | Balanced | LowBandwidth | VideoOptimized |
|---|---|---|---|---|
| JPEG quality (TightJPEG) | 92 | 75 | 45 | 70 |
| Tight compression | 2 | 6 | 9 | 4 |
| ZRLE compression | 2 | 6 | 9 | 6 |
| H.264 enabled | no | no¹ | yes | yes |
| H.264 bitrate | — | — | 2 Mbps | 8 Mbps |
| H.264 keyframe interval | — | — | 120 fr | 60 fr |
| Max frame rate | 60 Hz | 30 Hz | 15 Hz | 30 Hz |
| Max latency target | 20 ms | 80 ms | 300 ms | 50 ms |

¹ Balanced flips H.264 on once the server-side encoder lands; until
then leaving it off avoids the policy producing recommendations that
will always fall back.

`Preset::Custom` returns sentinel "don't override" tunings (negative
quality / compression numbers, zero rates) so callers can detect that
the operator hasn't picked a preset and leave the existing encoder
defaults alone.

## When H.264 helps, when classic wins

| Workload | Best classic encoder | H.264 win? |
|---|---|---|
| Static UI, terminals, code editors | Tight | no — loss is unacceptable, indexed compression already excellent |
| Documents and reading | TightJPEG (low quality fine) | no — H.264's setup cost dominates for sparse updates |
| Photos, slides, gradients | TightJPEG | maybe — H.264's keyframe + P-frames help only for repeated updates of the same region |
| Full-screen video, animation, games | TightJPEG (acceptable) | **yes** — H.264 is dramatically smaller per frame and tracks motion |
| Data visualisations changing every frame | TightJPEG | yes if the change region is large; classic if it's a small chart |

The policy's heuristic captures this via three thresholds:

- **Tiny** (`area < 256` pixels²): per-rect protocol overhead beats
  any compression. Always Raw / Tight.
- **JPEG-worthwhile** (`area >= 64×64`, smooth content): TightJPEG.
- **H.264-worthwhile** (`area >= 256×256`, `recentChangeFps >= 5`,
  preset enables H.264, client supports it): H.264.

## Compatibility

- **Clients without `encodingH264`** (most non-TigerVNC viewers, older
  TigerVNC viewers): policy never recommends H.264 for them. The
  fallback is automatic — `ClientCaps::supportsH264 == false` short-
  circuits the H.264 branch.
- **Clients with H.264**: policy recommends H.264 only when both the
  preset enables it *and* the rect's area + motion characteristics
  meet the thresholds. Otherwise classic encoders.
- **The policy never makes H.264 mandatory**: a preset with
  `h264Enabled = true` is a *permission*, not a *requirement*. Even
  with VideoOptimized + a capable client + a large rect, a still
  frame falls back to TightJPEG.

## Configuration

```
-EncodingPreset Balanced     # one of LANCrisp Balanced LowBandwidth
                             # VideoOptimized Custom (default)
-PolicyLogInterval 500       # emit mid-connection diagnostics every N
                             # frames; 0 disables (default 500 frames)
```

The preset is a `core::EnumParameter`, picked up via the standard
Configuration plumbing (CLI, config file, Windows registry). `Custom`
is the default and the escape hatch: operators who want to set the
underlying knobs directly (`-CompressionLevel`, `-JpegQuality`, etc.)
get exactly the historical behaviour with no preset interference.

Setting any other value causes `prepareEncoders()` to override the
per-connection `qualityLevel` and `compressLevel` from the preset's
tuning. The override is recomputed on every prepareEncoders() call so
runtime preset changes take effect without restarting the server.

JPEG quality in `tuningFor()` is on a 0..100 scale (the natural one
for `TightJPEG` / `JPEG`). `ClientParams::qualityLevel` is on a 0..9
scale; `prepareEncoders()` rescales by floor(q/10) and clamps to
[0, 9]. So `LANCrisp`'s 92 maps to `qualityLevel = 9`, `Balanced`'s
75 → 7, `LowBandwidth`'s 45 → 4, `VideoOptimized`'s 70 → 7.

## Diagnostics

`rfb::encoding::Diagnostics` is a tiny POD struct that EncodeManager
will populate as it dispatches each rectangle. `diagnosticsSummary()`
renders it as a one-line string suitable for periodic info-level
logging:

```
frames=1234 (Tight=900 TightJPEG=300 ZRLE=34 H264=0) fallbacks=0
```

The `fallbacks` counter ticks every time the policy recommended
H.264 but the encoder produced something else. When the server-side
H.264 encoder ships, `fallbacks` should drop to zero on workloads
where the policy chose H.264 — that's the regression check the
follow-up patch will use.

Per-frame logging is intentionally *not* added; the diagnostics are
designed to be sampled (every N frames or every M seconds) to keep
log volume low.

## Architecture decisions / non-goals

- The policy is in `common/rfb/`, not `win/`. It has no platform-
  specific code and is exercised by the `tests/unit/encodingpolicy`
  GTest on Linux, macOS, and Windows.
- The policy is a **pure function**: same inputs always produce the
  same output. This keeps it cheap to call per-rectangle and trivial
  to unit-test.
- The policy does **not** measure bandwidth or latency itself.
  `EncodeManager` already maintains a `Congestion` estimator; the
  policy will read those numbers via `RectStats` (extended in the
  follow-up).
- The policy does **not** cache decisions. `EncodeManager` already
  caches by region; adding a second cache layer would not help.

## Follow-up work

All four originally-listed gaps are now closed in this PR. Remaining
follow-ups are platform expansions and optimisations:

1. **Linux server-side H.264 encoder** (libav-based). Mirror
   `H264LibavDecoderContext` in the encode direction:
   `avcodec_find_encoder(AV_CODEC_ID_H264)` + NV12 input + drain
   `AVPacket`s. Wire from `H264EncoderContext::createContext` when
   `H264_LIBS=LIBAV`. Pre-existing libav build infrastructure makes
   this a moderate effort.
2. **macOS server-side H.264 encoder.** VideoToolbox encoder (native
   Apple HW path) + a CMake gating block.
3. **Hardware-accelerated D3D11 NV12 path on Windows.** The encoder
   today converts BGRA → NV12 on the CPU. A D3D11 shader-based
   converter would land it in GPU memory; worth doing only if
   profiling shows the CPU path is the bottleneck.
4. **Adaptive bitrate.** The encoder is pinned at 8 Mbps. Tying the
   bitrate to the connection's `Congestion` estimator would make the
   `LowBandwidth` preset deliver on its bitrate cap.
