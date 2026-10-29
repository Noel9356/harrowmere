# screenshot_png module

Frame capture to an image file (PNG by default, JPEG and BMP selectable). The module exposes the `ScreenshotCapability` (capability family `screenshot`, published in `screenshot_capability.hpp` next to this module; the plugin descriptor and manifest carry the implementation-bound member name `screenshot.png`, the PNG-codec capture implementation) — a single `requestScreenshot()` method — and owns the whole path from the request to a file on disk: queue, copy, readback, encode, write. Encoding runs on the module's own encode pool (sized by `encode_workers`, parallel by default): the frame handler copies the pixels out of the renderer's staging memory and the encode+write happens off the frame path, with `ScreenshotSavedEvent` published on the main thread at the next frame event for a file that is already on disk — a capture never stalls the frame it interrupted.

## Request paths

All of them end in the same queue, so captures are serialized and never interleave:

- the configured hotkey (`[screenshot].key`, default `F12`) — the module subscribes `KeyPressedEvent` on the event bus, which is why the key names it accepts are the same names the input_glfw module publishes;
- script mods calling `engine.request_screenshot()` through the Lua bridge;
- C++ consumers calling `requestFrameCapture()` on the `RendererCapability` (the screenshot_png module forwards it into the same queue);
- the app composition root, which may queue a capture at any point to exercise the readback and PNG write.

## Correctness model

The capture is recorded inside the same command buffer as the frame — the swapchain image is copied before present, synchronized with the frame's fence — then swizzled to RGBA8 on the CPU and written once the GPU signals. The result is never torn and exactly one frame old; a capture recorded after present would race the compositor and produce mixed frames, which is why the copy lives inside the frame's submission. The readback waits on its own fence with a dedicated budget, independent of the presentation fence. Writing the file requires the `filesystem.write` permission, which the module declares; denying it fails startup precisely instead of losing captures silently.

## Wiring

| Manifest field | Value |
|---|---|
| provided capabilities | `screenshot.png` (the `screenshot` family's implementation member) |
| required capabilities | `logging`, `renderer` |
| required permissions | `capability.logging`, `capability.renderer`, `filesystem.write` |

`input` is deliberately NOT required: the module reacts to key presses through the event bus (the input_glfw module publishes `KeyPressedEvent`), so a build without the input module loses only the hotkey, not the capability API — headless setups queue captures through the renderer capability or the Lua bridge.

## Configuration

| Key | Type | Default | Meaning |
|---|---|---|---|
| `key` | string | `"F12"` | key that queues a capture (same names the event bus reports) |
| `output_directory` | string | `""` (empty) | where capture files are written; the empty default resolves to `screenshots` beside the working directory (see below) |
| `format` | string | `"png"` | file format: `png` (lossless), `jpg` (quality 1-100), or `bmp` |
| `jpeg_quality` | integer | `90` | JPEG quality (only consulted by the `jpg` encoder) |
| `filename_prefix` | string | `"screenshot"` | file name stem: `<prefix>_<timestamp>_<counter>.<ext>` |
| `encode_workers` | integer | `0` (auto) | encoder pool size. `0` sizes the pool from the machine's hardware threads (the engine-wide parallel-by-default worker count), `1`-`8` pins an explicit size. Encodes are CPU-bound and independent, so a burst of captures drains in parallel; completions still drain on the frame event, so the bus stays single threaded. The startup log reports the resolved pool size |

Relative paths (including the default) resolve against the process working directory — the project root for the documented quick start (`./build/runtime/engine` from the engine tree), so captures land in `<project root>/screenshots` where the user actually looks; absolute paths are used as-is. An unknown `format` spelling fails startup precisely (the extension and the encoder must agree); captures still queued when the run stops are dropped with a warning.

Live reload: `output_directory`, `format`, `jpeg_quality` and `filename_prefix` re-read through the kernel's configuration watch utility (`ConfigurationWatchHub`, `kernel/include/engine/kernel/configuration_watch.hpp`) — one watch per key, fired exactly when the kernel's reload diff names that key (see `docs/configuration.md`, "Live configuration reload") — so a capture pipeline can retune its output mid-run and the module never hand-writes a changedKeys scan. Keep-last-good on invalid values; the trigger `key` is deliberately not reloadable (the startup binding stands for the run).

## Files

- `screenshot_capability.hpp` — the published capability surface every consumer includes
- `screenshot.hpp` / `screenshot.cpp` — the module, the queue, the readback and PNG writing
- `module_plugin.cpp` — the static plugin descriptor (name, factory, provided capabilities)
- `CMakeLists.txt` — target and option wiring (`ENGINE_BUILD_MODULE_SCREENSHOT_PNG`)
