# Windows screen capture backends

TigerVNC's Windows server is moving from a single GDI-only capture path
to a pluggable backend abstraction (`rfb::win32::capture::CaptureBackend`).
This document is the design rationale, the operator-facing description
of what each backend does, and the manual test matrix that gates
shipping a non-GDI default.

## Status (phase 1)

The interface, factory, and three backend classes are in place. The
runtime probe and logging are functional. **No backend's
`captureNextFrame()` is wired into `SDisplay` yet** — the existing
GDI path in `SDisplay` / `DeviceFrameBuffer` continues to drive real
capture. The `auto` factory walk currently lands on the GDI marker and
SDisplay is unchanged.

Phase 2 will land:

1. The actual `IDXGIOutputDuplication` body inside `DxgiDesktopDuplicationBackend`.
2. The `Windows.Graphics.Capture` (WinRT) body inside `WindowsGraphicsCaptureBackend`.
3. Migration of the BitBlt code from `DeviceFrameBuffer` into `GdiCaptureBackend`.
4. `SDisplay` consuming `CaptureBackend::captureNextFrame()` instead of
   directly calling `DeviceFrameBuffer::grabRect()`.

## Backends

| Backend | Min Windows | Service / unattended | Secure desktop | User consent prompt | Status |
|---|---|---|---|---|---|
| **`gdi`** | 7+ | yes (interactive session only) | no — black frame on UAC | no | shipping (legacy `SDisplay` path) |
| **`dxgi`** | 8+ (Win10+ recommended) | yes | no — frame freezes | no | scaffolded; `initialize()` returns `NotSupported` |
| **`wgc`** | 10 1903+ | **no** — user session only | no | yes (yellow-border indicator) | scaffolded |

### `gdi` — BitBlt fallback

The historical capture path. Walks the desktop with `BitBlt` from a
`DeviceContext` into a `DIBSectionBuffer`, change-detected via
`SDisplayCorePolling` or `SDisplayCoreWMHooks`. Cursor is captured
separately via `WMCursor`. Always available, no DirectX dependency,
known-working on every supported Windows host.

Limitations:
- High CPU on busy screens (per-frame full-screen `BitBlt` even with
  WM-hook narrowing).
- Cannot capture across the secure desktop (UAC, Ctrl+Alt+Del,
  login screen): the captured frame freezes with whatever was visible
  before the secure desktop appeared.
- Loses HW-accelerated DirectX game / video frames in some
  configurations.

### `dxgi` — DXGI Desktop Duplication

The native Windows API for unattended remote-control screen capture. A
`D3D11` device shares a desktop image with the application via
`IDXGIOutputDuplication::AcquireNextFrame()`. The OS gives us:

- changed-rectangle metadata (multiple small dirty rects per frame),
- move-rectangle metadata (e.g. window scrolls),
- the pointer shape and position in a separate channel,

so the backend can avoid full-frame updates and let the encoder send
much less data. Phase 2 will plumb all of that through `FrameMeta`.

Cannot capture the secure desktop. When UAC pops, `AcquireNextFrame`
returns `DXGI_ERROR_ACCESS_LOST` and the backend reports
`SecureDesktopActive`; the legacy GDI fallback would have shown the
same black frame, just by a different mechanism.

### `wgc` — Windows.Graphics.Capture

The user-consent path. `GraphicsCaptureSession` requires the user (or
the app's manifest) to nominate a monitor or window to share. Win 10
shows a yellow border around the captured surface; Win 11 22H2+ allows
suppression but the consent gesture is still required.

This is the **right** API for "share my screen" interactive sessions
and the **wrong** API for unattended VNC server capture. The scaffold
exists so a future "user-mode interactive sharing" mode can use it
without re-litigating the abstraction.

## Selection

The `CaptureBackend` configuration parameter accepts:

- `auto` *(default)* — try `dxgi`, then `wgc`, then `gdi`.
- `dxgi`, `wgc`, `gdi` — pin to a specific backend; if unavailable, fall
  back through the auto sequence rather than fail outright.

In phase 1 the parameter is read by `setConfiguredBackendKind()` but
not yet by `SDisplay` — the BitBlt path runs unconditionally regardless
of the value. Phase 2 wires it.

The factory logs the selection at INFO and the reason any backend was
rejected. Per-frame logging is intentionally absent.

## Logging

- `CaptureBackend`: factory-level decisions (`Selected capture backend:
  ...`, `... rejected initialize: NotSupported`, etc.).
- `DxgiCapture`, `WgcCapture`, `GdiCapture`: per-backend init / probe /
  shutdown.
- No backend logs per frame.

## Testability

`rfb::capture::coalesceRects` (in `common/rfb/CaptureRectCoalesce.h`) is
the unit-testable helper for the dirty-rect coalescing the DXGI backend
will need. It's pure, has no platform dependencies, and is exercised
by the cross-platform `tests/unit/capturerectcoalesce` GTest. Run on
any host:

```
cmake --build build --target capturerectcoalesce
ctest --test-dir build/tests/unit -R capturerectcoalesce
```

The expected coalescing budget is `maxGrowthRatio = 1.5` (allow up to
50% wasted pixels in exchange for one fewer rectangle), but callers
can tune up or down.

## Manual test matrix

Phase 1 only changes `gdi` behaviour by routing the marker initialize
through the new factory; no regression should be observable. Phase 2
tests will live below, gated by which backend is being exercised.

| Scenario | gdi (legacy) | dxgi (phase 2) | wgc (phase 2) |
|---|---|---|---|
| Single monitor, x64, latest Win 11 | shipping | TBD | TBD |
| Single monitor, ARM64, Win 11 | verified post-PR-#2 | TBD | TBD |
| Multiple monitors, mixed resolution | shipping | TBD | n/a (per-monitor only) |
| Mixed DPI (100% / 150%) | partial — no DPI awareness in `MonitorInfo` yet | TBD | TBD |
| Rotated monitor (90 / 180 / 270) | partial — rotation field is 0 in phase 1 | TBD | TBD |
| Display hotplug during session | tracked via `WMMonitor`; capture survives | TBD via `onDisplayChanged` | TBD |
| Sleep / resume | tracked via `WMMonitor`; capture survives | TBD via `onSessionChanged` | TBD |
| Lock / unlock | screen freezes during secure desktop | `SecureDesktopActive` reported | n/a (user session ends) |
| RDP-into-server session | works if running in RDP session | DXGI works in RDP sessions on Server SKUs only | n/a |
| Windows ARM64 (Snapdragon) | verified | TBD — ARM64 D3D11 supported since Win 10 1709 | TBD |

## Out of scope for phase 1

- Encoder-side optimisation for many small dirty rects (will benefit
  from `coalesceRects()` once DXGI lands).
- Hardware-accelerated H.264 encode of the DXGI texture.
- Audio capture.
- Per-window capture via WGC.

## References

- DXGI Desktop Duplication API:
  <https://learn.microsoft.com/windows/win32/direct3ddxgi/desktop-dup-api>
- Windows.Graphics.Capture:
  <https://learn.microsoft.com/windows/uwp/audio-video-camera/screen-capture>
- `GraphicsCaptureSession::IsSupported`:
  <https://learn.microsoft.com/uwp/api/windows.graphics.capture.graphicscapturesession.issupported>
