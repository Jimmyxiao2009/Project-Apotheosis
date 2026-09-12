# WK_WINUWP patch set

This repository includes `wk-winuwp.patch`, the complete source diff for the
WebKit tree used by Project Apotheosis, against the upstream tag.

## Scope

- Base: `webkitgtk-2.52.4`
- Base commit: `e4ab5336695fe76b623682915737c3ee88f2e0ed`
- Patched tree: `05a728a8e0` (branch `winuwp`, 111 commits over the tag)
- Files changed: 141 under `Source/` (17 new files)
- Diff: 12802 insertions and 105 deletions

The patch contains the ARM32 Windows 10 Mobile / UWP adaptations used by the
`gpu-path1` development line. It is intentionally a source diff rather than a
copy of the multi-gigabyte WebKit checkout. Every change is guarded with
`#if defined(WK_WINUWP)` (or confined to the WinUWP cmake files) and carries an
`Apotheosis:` comment.

### What is in it

**1. The original `WK_WINUWP` patch set** (65 files, +852/−72): WTF core, JSC
ARM32 assembler/offlineasm, curl networking, FreeType/HarfBuzz font path, CMake
plumbing.

**2. The port definition files** that the first release of the patch was missing
(they had never been `git add`-ed, so `git diff` skipped them). Reconstructed
from `port/draft-PlatformWinUWP.cmake`,
`port/draft-OptionsWinUWP-webcore-block.cmake` and the upstream Win port files,
and verified by building the engine and the harness from scratch on a second
machine:

- `Source/cmake/OptionsWinUWP.cmake`
- `Source/WTF/wtf/PlatformWinUWP.cmake`
- `Source/JavaScriptCore/PlatformWinUWP.cmake`
- `Source/WebCore/PlatformWinUWP.cmake`

**3. Small follow-up fixes that a clean build from the tag needed**, all under
`WK_WINUWP` guards or confined to the WinUWP cmake files:

- no `libpsl` dependency (PublicSuffixStore is stubbed in `port/stubs-other.cpp`);
- generic WTF event loop (`RunLoopWin` needs an HWND, impossible in an App Container);
- `JSStringRefBSTR` dropped from the JSC build (no `BSTR` in the App Container partition);
- desktop-only keyboard APIs (`GetKeyState`, `GetKeyboardLayout`, `ToUnicodeEx`)
  guarded in `KeyEventWin.cpp` / `WindowsKeyNames.cpp` – the harness supplies
  modifiers and text;
- GLFence sources left out (the prebuilt ANGLE 2.1.13 `eglext.h` predates
  `EGL_KHR_fence_sync`), `EGL_EGLEXT_PROTOTYPES` defined for that header;
- OpenSSL 3: `const_cast` for `EVP_PKEY_get0_*`, `HKDF` through
  `EVP_PKEY_HKDF` (the one-shot `HKDF()` is BoringSSL-only);
- GDI cairo interop files (`GraphicsContextWinCairo`, `ImageAdapterWinCairo`)
  left out – the App Container cairo has no win32 backend;
- `accessibility/win`, `platform/graphics/egl` and `platform/video-codecs`
  on the WebCore include path (headers only).

Because `Source/WebCore/PlatformWinUWP.cmake` includes
`platform/OpenSSL.cmake`, WebCore now provides the WebCrypto backend itself;
the corresponding stubs in `port/stubs-crypto.cpp` and the two
`PlatformKeyboardEvent` stubs in `port/stubs-other.cpp` were removed in the
same change set (they produced LNK2005 duplicates against `WebCore.lib`).

Items 1–3 are the state of the patch as of 2026-09-06. Everything below was
added afterwards and is what this revision of the patch adds.

**4. `APOTHEOSIS_JIT`: the baseline JIT actually enabled on ARMv7.**
`PlatformEnable.h` forces `ENABLE_JIT=0` on 32-bit non-Linux, so the shipping
engine was LLInt-only – `JavaScriptCore.lib` contained no `JSC::JIT` symbol at
all. The opt-in option lets `ENABLE_JIT` (with DFG; JIT without DFG is no longer
buildable upstream) take effect for this port. Three walls had to be fixed with
it: a `Wasm::WasmOrigin` guard for JIT-without-WebAssembly, `getpid()` (absent
from the App Container CRT) replaced with WTF's `getCurrentProcessID()`, and –
the one that mattered – `ARMv7Assembler::replaceWithJump()` computing its T4
branch displacement modulo 32 MB on non-Linux, which corrupted the JIT pool and
crashed while scrolling. JSC compiler threads get a 4 MB stack. On the device
the JIT is worth 20–30 % on animation-heavy pages.

**5. Crash and assertion diagnostics.** W10M writes no WER dump for an App
Container fast-fail, so a crash used to be a silent disappearance. A WTF crash
hook, a vectored exception handler and a `SIGABRT` handler in the driver write
`LocalState\crash.txt`; a plain `RELEASE_ASSERT` now reports its location; a
refused executable `VirtualProtect` crashes with the error instead of writing
into a read-only page; and a `CanMakeCheckedPtrBase` destroyed with an
outstanding `CheckedPtr` reports the class and survives instead of aborting.
Every one of these found a real bug on the device.

**6. Thread stacks and texture memory.** Every WTF thread used to inherit the
harness' 16 MB PE stack reservation, so a page with a few dozen workers
exhausted the 32-bit address space and died in `RELEASE_ASSERT` inside
`WTF::Thread::create` (a news site, reproducible). Stack reservations are now
per thread type (JavaScript 8 MB, everything else 1 MB). TextureMapper accounts
its texture bytes and the pool is bounded, so a pinch cannot allocate the app
out of its memory budget.

**7. Networking.** The port ships its own curl build; this raises the total
connection cap from 17 (an HTTP/1.1-era per-browser figure) to 32 and states
`CURLPIPE_MULTIPLEX` explicitly, allows 8 parallel loads per host, reports
connect/TLS/TTFB timings for every response (gated, off by default), pauses a
deferred transfer instead of cancelling it, prefetches DNS for
`preconnect`/`dns-prefetch` hints, and never sends `<a ping>` hyperlink
auditing. Three real defects came out of the same work: `ResourceHandle::cancel()`
left a dangling client (null+0x14 crash in redirect completion), every finished
load leaked handle, delegate, request and response, and a paused 3xx transfer
was never cancelled on the terminal paths. On the device time-to-commit on a news
site went from 14.8 s to 0.4 s.

**8. Load-time instrumentation.** The driver's pump cannot see main-thread work
it did not start, and a cold news-site load had ~5 s of unattributed time. Style,
layout and timer passes are counted per load, a pending resource is dated, and
DOM timers are aligned onto a coarse grid while a load pump owns the engine
thread. Opt-in (armed together with `LocalState\perf.txt`), no cost otherwise.

**9. Images and text.** A shared decode work queue; large images decode
asynchronously while visible, animated frames stay synchronous (the icon jump on a
code-hosting site); a failed asynchronous decode notifies its waiting clients instead of
leaving a white box; an already decoded image is drawn synchronously rather than
re-decoded at a new scale. Glyphs are no longer rasterised with subpixel
antialiasing (the "doubled text" after a pinch was colour fringing), and the
synthetic-bold offset walks in device-pixel steps – the actual "doubled text"
was Cairo drawing the run twice at a fractional offset because only the regular
font cuts were packaged.
A character outside the Basic Multilingual Plane no longer draws the primary
font's `.notdef` box after itself: `WidthIterator` appends a placeholder glyph
after every surrogate pair for a shaping routine to delete again, and this port
has none (`Font::applyTransforms()` is the `!USE(CORE_TEXT)` no-op, and simple
text is not routed through `ComplexTextController` as it is on GTK/WPE).

**10. Compositing.** `AcceleratedCompositingForFixedPosition` was never enabled,
so every sticky or fixed element was repainted into the scrolling layer on every
frame – the sticky-header flicker chased through four packages.

**11. The Cairo record/replay painting engine and off-thread rasterisation.**
A tile paint is recorded into a display list on the engine thread and replayed by
a worker pool. Measured on the device: 2.9 ms versus 25 ms of engine-thread time
per scroll tick. The recorder can be cancelled, records the tiles nearest the
viewport first, and drops the replays of tiles that are being destroyed. It ran
behind a runtime switch during bring-up; the switch is gone now that it is
proven.

`Cairo::OperationRecorder::applyDeviceScaleFactor()` is an **empty override**
while `GraphicsContext` implements it as `scale(factor)` — which is what the
synchronous `BitmapTexture::updateContents()` / ImageBuffer path gets. Both
recording call sites (`TextureMapperTile::wkBeginThreadedPaint` and the tile
grid's `record()`) used it, so from the moment threaded raster became the
default every zoomed tile was translated but never scaled: each tile held a
source-rect-sized corner of the *unscaled* page and white everywhere else. That
is the whole "white while zoomed, old tiles spread out with gaps" picture, and
it was the ground under a long chase of tile-level workarounds. The fix is
`context.scale(scale)` at both call sites; the recorder itself is upstream code
and is left alone.

**12. TileGrid v2 – the tile bookkeeping rebuilt.**
`TextureMapperTiledBackingStore` had accumulated a cover rect, a keep rect, a
stale-tile stash, a progressive cover, a lattice backstop, four per-tile flags,
three traces and a driver-side repair loop that force-dirtied the whole layer
tree when the answers did not add up. The mechanisms fought each other; the
device showed the fights as holes, wrong-scale tiles and one-to-two-second
stalls.

The replacement is a **pure, host-testable grid model** with a thin WebCore
adapter, modelled on WebKit's Cocoa `TileController`/`TileGrid` and reduced to
what this port needs:

- `TextureMapperTileGridModel.{h,cpp}` + `TextureMapperTileGridMachines.h` – the
  model: index-addressed lattice, one primary grid and one frozen previous-scale
  grid, five lifecycles (tile, dirty patch, grid, replay job, store phase)
  written as Boost.SML transition tables. No allocation, no RTTI, no
  exceptions – and an unhandled event is *reported*, never silently ignored.
- `TextureMapperTileGridCore.{h,cpp}` – reconciliation against two backend
  interfaces (raster, texture), no WebCore types.
- `TextureMapperTileGridStore.{h,cpp}`, `TextureMapperTiledStore.h` – the
  WebCore adapter and the store interface; `TextureMapperTiledBackingStore` is
  back to upstream plus ~40 lines of viewport-limited 1024-pixel tiling (pure
  upstream tiles a long page into a dozen 16 MB textures, which is an
  out-of-memory kill on a 32-bit address space).
- `Source/ThirdParty/sml/` – Boost.SML 1.2.0, vendored verbatim with its Boost
  Software License 1.0 and a README recording origin, version and the rule that
  it may only be included from `TextureMapperTileGridMachines.h` and the two
  `.cpp` files, so it never leaks into a WebCore header.

Every pass derives the desired tile set from (layer bounds, scale, visible rect,
budget) and reconciles the grid to it; nothing survives by accident, there are no
timeouts or grace counters, and a tile draws either nothing or pixels rasterised
for exactly its index and scale. Holes are counted over the *desired* cells, so
the driver can ask for one more composite instead of dirtying the tree.

The design is written down in `docs/TILEGRID-DESIGN.md`; the sources and the
host tests cite its section numbers (§2 the model, §2.4 rules R1-R12, §3 the
driver contract, §4 the invariants, §5 the code layout and the trace format).

The model and the core are compiled and tested on the PC:
`tests/tilegrid/` (CMake + MSVC, no WebKit build, no device) runs eighteen
invariants, fourteen scripted scenarios plus five distilled from device rounds,
a fuzzer, and a replay of two captured device traces in
`tests/tilegrid/replays/` (1.9 MB and 1.3 MB of `tg` lines) - 2 525 111
checks in well under a minute.

`TextureMapperTile.{h,cpp}` is byte-identical to the tag again and
`TextureMapperTiledBackingStore.cpp` went from 2 355 to 320 lines; the texmap
share of this patch dropped from 8 656 to 6 142 added lines when v1 was deleted.

## Apply

From the root of a WebKit checkout at the base tag:

```powershell
git checkout webkitgtk-2.52.4
git apply --3way path\to\wk-winuwp.patch
```

The patch is generated with paths relative to the WebKit root, so it should be
applied from that root. Keep the tree LF-only. The patch itself is LF and stays
LF on checkout: `.gitattributes` in this repository marks it `-text`, so
`core.autocrlf=true` does not rewrite it.  Review the resulting diff before
building.

## Regenerate

On a branch carrying the changes on top of the tag:

```powershell
git diff --output=wk-winuwp.patch webkitgtk-2.52.4 HEAD -- Source/
```

(`--output=` instead of a shell redirect: PowerShell would rewrite the line
endings and the patch would stop applying.)

## Changes in this patch, commit by commit

Grouped by area; the subject line is the reason in short form.

### Base: the author's published patch set (1)

| Commit | Change |
|---|---|
| `c72fe20eec` | Apply upstream WK_WINUWP patch set (Apotheosis 2026-09-01) |

### Build and port definition (12)

| Commit | Change |
|---|---|
| `7f360973f2` | Add OptionsWinUWP.cmake for the WinUWP ARM32 port |
| `9bab70e789` | Turn USE_JPEGXL and USE_LCMS off for WinUWP |
| `f168f619c2` | Disable the jsc shell for WinUWP |
| `bd97751e80` | drop LibPSL on WinUWP (PublicSuffixStore stubbed in the driver) |
| `338472dd07` | select the generic event loop for WinUWP |
| `ba7837ad59` | fix remaining WebCore compile walls for the App Container |
| `ac322810b8` | Normalize KeyEventWin/WindowsKeyNames back to LF line endings |
| `494988c5d1` | add platform/graphics/egl to WebCore include dirs |
| `dfe9674e71` | accessibility, EGL ext prototypes, GLFence off for WinUWP |
| `ed4fedef34` | keep the Win accessibility wrapper header on the include path |
| `6c2e00cb51` | HKDF via EVP_PKEY, drop GDI cairo interop, video-codecs headers |
| `1e7bda4a49` | Normalize four files back to LF line endings |

### JIT on ARMv7 (5)

| Commit | Change |
|---|---|
| `e1956f74c4` | Add WTF, JavaScriptCore and WebCore PlatformWinUWP.cmake |
| `d5de2c44a5` | drop JSStringRefBSTR from the WinUWP JSC build |
| `d6fbd77538` | guard Wasm::WasmOrigin in PCToCodeOriginMap.h for JIT-without-WebAssembly |
| `2839627d94` | use WTF getCurrentProcessID() in ExecutableAllocator's verbose log (no getpid in the App Container CRT) |
| `9b1d994edb` | range-checked replaceWithJump on ARMv7 WinUWP (T4 branch wrapped modulo 32 MB) |

### WTF: threads, crashes, diagnostics (7)

| Commit | Change |
|---|---|
| `e072a82874` | APOTHEOSIS_JIT option lets ENABLE_JIT take effect on ARMv7 WinUWP |
| `d640b0b358` | crash hook in WTF Assertions.cpp for the WinUWP port |
| `1b7cd2ce69` | crash loudly when the App Container executable VirtualProtect fails |
| `c2f1036377` | report the location of plain RELEASE_ASSERT crashes on WinUWP |
| `9e19b7821f` | explicit per-type thread stack reservations on WinUWP |
| `07addeb841` | 4 MB stack for the JSC compiler threads |
| `b1cde9e0d5` | name the class that leaves a dangling CheckedPtr, and survive it |

### Networking and loading (12)

| Commit | Change |
|---|---|
| `710c5a57b5` | 8 parallel loads per host on WinUWP (curl is HTTP/1.1 only) |
| `4a193bb7e8` | main-resource connect timings to the driver; connect stall guard; IPv4-only switch |
| `c4e1fa89dc` | deferred loading pauses the curl transfer instead of cancelling it |
| `f51c4a62e3` | DNS prefetch for preconnect/dns-prefetch hints (WebResourceLoadScheduler) |
| `c43cb044e0` | never send <a ping> hyperlink-auditing requests |
| `399037ae69` | fix null+0x14 crash in redirect completion (dangling client) |
| `5917b69286` | clear the client in ResourceHandle::cancel() (dangling client on every async hop) |
| `24da386f96` | release the curl delegate on terminal paths (handle/delegate/CurlRequest leaked per load) |
| `5b8c67ca7c` | cancel the paused 3xx transfer on the terminal redirect paths |
| `9f046d0284` | Raise the curl total-connection cap and state HTTP/2 multiplexing |
| `a48c7c6be7` | Report every curl response to the driver, not just the main document |
| `4367730d13` | Count the style, layout and timer passes of a load, and date a pending resource |

### Images (8)

| Commit | Change |
|---|---|
| `a7bc06ddc5` | decode image frames on one shared work queue |
| `72deac3129` | a failed asynchronous image decode must repaint its waiting clients |
| `4a3003d2f1` | BitmapImageSource back to LF (tool wrote CRLF in ca61eaead9) |
| `5a60b606c4` | draw an already decoded image synchronously |
| `d2a0bbaa3d` | stop tagging cached image frames with a size for drawing |
| `a6077851a9` | clear the async-decode-failed flag before the caching early-outs |
| `bf47da9deb` | Attribute the main-thread load time the driver's pump cannot see |
| `f36836c878` | an ImageBackingStore that could not allocate must not look valid |

### Fonts and text (3)

| Commit | Change |
|---|---|
| `eb3bc29a3b` | stop rasterising glyphs with subpixel antialiasing |
| `a6aca5b304` | walk the synthetic-bold offset in device-pixel steps |
| `31b03a10b0` | build the TileGrid v2 model, core and store for WinUWP |

### Rendering and compositing (4)

| Commit | Change |
|---|---|
| `3f75b01938` | asynchronous decoding for large images while they are visible |
| `1ae389e015` | keep animated image frames on the synchronous decode path |
| `b3aa6628ac` | Align DOM timers onto a coarse grid while a load pump owns the engine thread |
| `fe0f870dea` | give sticky layers a backing and never lose a scroll compositing update |

### Cairo record/replay and off-thread raster (8)

| Commit | Change |
|---|---|
| `efae69091c` | build the Cairo record/replay painting engine for WinUWP |
| `f02ee934ac` | off-thread tile rasterisation behind a runtime switch |
| `059a4f41de` | let the engine thread cancel a tile replay |
| `f081b36bc8` | rasterise a tile's first paint off-thread as well |
| `1ad4ffa3ee` | record the tiles nearest the viewport first |
| `52b99efdaa` | cancel the replays of tiles that are being dropped |
| `58a0d86d2c` | hand the cairo recording to every raster job, never upload silently |
| `e63efe5711` | scale a recorded tile paint with scale(), not applyDeviceScaleFactor() |

### Tiling - texmap (v1 era) (33)

| Commit | Change |
|---|---|
| `f8bbb6517d` | paint newly created / resized TextureMapper backing stores in full; dirty-stats accessor |
| `0024624f92` | retry the image upload when the native image is not available yet |
| `17238de945` | count texmap backing-store repaints so images decode synchronously |
| `b02ec30010` | tile layer content at 1024 and only rasterise around the visible rect |
| `d6d4c3dcc1` | account texmap texture bytes, bound the texture pool, tile images at 1024 |
| `22c2ea4286` | keep visible-rect tiling under page scale, cap tiles per store |
| `76b4ec4bf0` | keep the previous scale's tiles as placeholders across a pinch |
| `bba9c69412` | snap tile quads to the device pixel grid to kill zoom seams |
| `59555d0659` | paint the visible tiles synchronously across a scale change |
| `1b6fc2f1dc` | place tile content on a continuous sub-pixel grid |
| `274d278e14` | repaint visible tiles that lost their replay |
| `c9f5b7ed92` | draw the stale zoom tiles only where the new ones are still empty |
| `32c6911ba2` | keep visible first paints synchronous for as long as the page is zoomed |
| `692d5e29f4` | drop the old grid's tiles when the contents scale changes |
| `c48b7b2761` | pre-render the tiles around the viewport while the page is idle |
| `b22d481732` | never leave a tile inside the viewport transparent while zoomed |
| `509ea911a5` | export a texmap layer/tile dump for the driver |
| `eff7149b3e` | keep the tiles the viewport swept through synchronous |
| `daca74cbfb` | round the scaled dirty rect outwards (1-px stale band while zoomed) |
| `f76795b115` | keep the zoom placeholders a device pixel clear of painted tiles |
| `fc1b21b442` | force a stuck visible tile back on the synchronous path with >= |
| `39915ee26f` | bound the tile uploads one composite may hand to GL |
| `9c2dd52b40` | never show another rect's or scale's pixels while panning zoomed |
| `cbcab943aa` | stop hiding a recycled tile at scale 1 and never defer a small layer |
| `2ff05cd7ac` | never draw a tile over the placeholder that stands in for it |
| `5339355214` | never let the upload budget hold a wrong tile on screen |
| `32c23f2dc6` | never draw a tile with a texture that is not its own |
| `abccd8c3ca` | never let a placeholder outlive the tile grid it stands in for |
| `52f31062cf` | do not delete a placeholder set that is doing its job |
| `8b08871ec2` | tell every backing store what the page scale is |
| `7b882c6d8c` | keep the tile grid on the tile lattice while zoomed |
| `fe469e688f` | per-cause full-repaint counters for the tiling-rewrite diagnostic |
| `69dd649d0a` | drop v1 tile erase threshold on WinUWP |

### Tiling - TileGrid v2 (17)

| Commit | Change |
|---|---|
| `f2234c63d9` | vendor Boost.SML 1.2.0 for the tile grid state machines |
| `d64e03a265` | add the pure TileGrid v2 model and its state machines |
| `0bd4107c14` | paintedRect semantics and a gesture-free pan margin in the tile model |
| `6abcec7eaa` | add the TileGrid v2 store core and its two backend interfaces |
| `32390ebf51` | give the tiled backing store an interface and pick it at creation |
| `730221b40e` | add the TileGrid v2 store, its backends, its switch and its trace |
| `be8e31d4e9` | let the tile model hear about refused requests and failed uploads |
| `25de2a3bae` | report a refused raster job and an impossible upload to the model |
| `f750add4c3` | queue raster jobs over the in-flight cap instead of refusing them |
| `756da71476` | TileGrid v2 by default; delete the v1 tile mechanics of packages 11-28 |
| `41ae9e9f5e` | invalidate an image store once per frame, and never budget it |
| `49fac8bdd7` | delete the TileGrid and threaded-raster runtime switches |
| `d745824a6b` | a store that shows nothing must not raster (TileGrid v2) |
| `7a5afd1c0f` | anonymise test-site references in comments |
| `1e0265f3e4` | point design references at the shipped document |
| `ea8744130c` | do not name our log directories in the comments |
| `b2d15873fb` | reword the comments that still cite unpublished documents |

### Unclassified - file a rule in local\make-pr-branch.ps1 (1)

| Commit | Change |
|---|---|
| `05a728a8e0` | stop drawing a .notdef box after every non-BMP character |


This is the source artifact requested in
[Issue #3](https://github.com/MoonlightLabCN/Project-Apotheosis/issues/3).
