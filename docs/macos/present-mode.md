# Present mode on macOS

## What the device offers

MoltenVK presents through `CAMetalLayer`. On the M4 Max (MoltenVK 1.4.2, macOS 27.2) the
surface reports exactly two present modes (`out/m4-moltenvk-full.txt`, Sol-1's device report):

| Vulkan mode | Metal behaviour |
|---|---|
| `FIFO` | `displaySyncEnabled = YES`: a drawable is shown at the next display refresh. |
| `IMMEDIATE` | `displaySyncEnabled = NO`: shown as soon as the compositor takes it; can tear when the window is presented directly to the display (full screen). |

There is no `MAILBOX` and no `FIFO_RELAXED` (MoltenVK `MVKPhysicalDevice::getSurfacePresentModes`).

## Choice

`FIFO` is the Apple default (`BB_PRESENT_MODE` unset). `BB_PRESENT_MODE=Immediate` is an explicit
opt-in for measurements that must run faster than the display (`BB_FPS_LIMIT=0`). Any other value,
including `Mailbox`, the shadPS4 default on other platforms, is a hard error. So is a mode the
surface does not report. There is no fallback between modes (`vk_swapchain.cpp`,
`FindPresentMode`).

## Why FIFO fits every frame-rate preset

The guest is paced by the emulated vblank on the present thread
(`VideoOutDriver::PresentThread`), and that thread also calls `Flip`, so anything that blocks in
`vkAcquireNextImageKHR` delays vblank events. With FIFO, acquire blocks only when every
swapchain image is queued, which means the game is presenting faster than the display refreshes.
No shipping preset does that:

| Preset (`run.sh` / `run.py`) | Pacing | FIFO effect |
|---|---|---|
| 30 / 60 locked (`BB_VBLANK_HZ=60`) | vblank timer at 60 Hz, one flip per vblank at most | display ≥ 60 Hz: the queue never fills, no blocking |
| 90 (`BB_VBLANK_HZ=90`) | 90 Hz vblank | needs a display ≥ 90 Hz (the 120 Hz panel); on a 60 Hz display FIFO caps it at 60 |
| uncapped (`BB_VBLANK_HZ=0`) | 480 Hz vblank, frame limit = min(display refresh, 120) | at or below the display rate: no blocking |

Every Mac run before this change already presented with FIFO: the old code requested Mailbox, logged
"falling back to Fifo" and continued. The title-screen 60 FPS / 16.7 ms worst-frame result was
measured that way, so making FIFO deliberate changes no runtime behaviour. It only removes the
warn-and-continue path.

## Known limit

The emulated vblank is a free-running timer that is not locked to the display. A 60 Hz game on a
60 Hz display therefore drifts against refresh, and about once per drift period one frame is shown
for two refreshes. Mailbox would drop a frame instead, which is the same kind of judder. The real fix
for steady 60 FPS is to drive the emulated vblank from the display link (`CAMetalDisplayLink`); it
is tracked as renderer performance work, not as part of present-mode selection.
