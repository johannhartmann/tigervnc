# Encoding policy and presets

TigerVNC's server selects a per-rectangle encoding (Tight, TightJPEG,
ZRLE, Hextile, RRE, Raw, JPEG) based on the rectangle's content and
the client's advertised capabilities. This document describes the
adaptive **encoding policy** module that's been added to centralise
those decisions, the **operator-facing presets** that tune them, and
the **diagnostics** that surface what the server actually picked.

## Status (this PR)

| Item | Status |
|---|---|
| `rfb::encoding::pickEncoder()` policy function | ✓ |
| `Preset` enum + `tuningFor()` (LANCrisp / Balanced / LowBandwidth / VideoOptimized / Custom) | ✓ |
| `Diagnostics` counters and `diagnosticsSummary()` | ✓ |
| GTest coverage of the decision logic | ✓ (17 cases) |
| Server-side **H.264 encoder** | not yet implemented |
| `EncodeManager` consumes the policy | follow-up |
| `core::EnumParameter` wired into the global config so operators can pick a preset | follow-up |

The policy is a strictly additive module: it computes a recommendation
but `EncodeManager` does not yet route through it. That's the
follow-up that turns the framework into runtime behaviour.

## H.264, honestly

Upstream TigerVNC's H.264 code is **decoder-only**. The server has no
H.264 encoder, on any platform. The `Decoder.cxx` path that
instantiates `H264Decoder` covers viewers connecting to non-TigerVNC
servers that happen to send `encodingH264 (50)`. The
`HAVE_H264 / H264_LIBS=WIN` build flags wire up the Media Foundation
**decoder** for the Windows viewer.

So as of this PR:

- The policy can **return** `RecommendedEncoder::H264` when the preset
  enables it, the client supports it, and the rect is large + moving.
- `EncodeManager` has no H.264 path to call. The future server-side
  encoder (Media Foundation H.264 on Windows, libav on Linux/Mac) is a
  separate, substantial effort — comparable to the DXGI capture work.
- When the policy says "H.264" and `EncodeManager` falls back to
  TightJPEG (or whatever it would have picked), the diagnostics
  helper records a `fallback`. When the server-side encoder ships,
  `fallbacks` should drop to zero on the relevant connections.

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

## Configuration (planned)

Until the follow-up that wires `EncodeManager`, none of these knobs
take effect. Listed here so the eventual surface is documented:

```
-EncodingPreset Balanced     # one of LANCrisp Balanced LowBandwidth
                             # VideoOptimized Custom
```

The preset selection will be a `core::EnumParameter`, picked up via
the standard Configuration plumbing (CLI, config file, registry on
Windows). `Custom` is the escape hatch for operators who want to set
the underlying knobs directly (`-CompressionLevel`, `-JpegQuality`,
etc.) without the preset overriding them.

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

## Follow-up work (not in this PR)

1. Wire `EncodeManager` to call `pickEncoder()` per rectangle and
   record the recommendation in a per-connection `Diagnostics`. Until
   the H.264 encoder lands, the recommendation is observational; if
   the policy says "H.264", a `recordFallback()` is logged and the
   existing heuristic is used.
2. Wire a `core::EnumParameter "EncodingPreset"` to `tuningFor()`,
   and have `EncodeManager` apply the preset's quality / compression
   numbers to its existing knobs.
3. Implement the **server-side H.264 encoder**: `H264Encoder` +
   `H264WinEncoderContext` (Media Foundation) + libav encoder for
   Linux/Mac, BGRA → NV12, frame submission, GOP / keyframe handling,
   bitrate control.
4. Surface `Diagnostics` via a per-connection log line every N frames,
   and (optionally) via a small admin-facing JSON dump.
