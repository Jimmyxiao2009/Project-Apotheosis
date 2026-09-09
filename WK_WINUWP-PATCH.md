# WK_WINUWP patch set

This repository includes `wk-winuwp.patch`, the complete source diff for the
WebKit tree used by Project Apotheosis, against the upstream tag.

## Scope

- Base: `webkitgtk-2.52.4`
- Base commit: `e4ab5336695fe76b623682915737c3ee88f2e0ed`
- Files changed: 77 under `Source/` (4 new files)
- Diff: 1597 insertions and 82 deletions

The patch contains the ARM32 Windows 10 Mobile / UWP adaptations used by the
`gpu-path1` development line. It is intentionally a source diff rather than a
copy of the multi-gigabyte WebKit checkout.

### What is in it

1. The original `WK_WINUWP` patch set (65 files, +852/−72): WTF core, JSC
   ARM32 assembler/offlineasm, curl networking, FreeType/HarfBuzz font path,
   CMake plumbing. Every change is guarded with `#if defined(WK_WINUWP)` and
   carries an `Apotheosis:` comment.
2. The port definition files that the first release of the patch was missing
   (they had never been `git add`-ed, so `git diff` skipped them). They were
   reconstructed from `port/draft-PlatformWinUWP.cmake`,
   `port/draft-OptionsWinUWP-webcore-block.cmake` and the upstream Win port
   files, and verified by building the engine and the harness from scratch on
   a second machine:
   - `Source/cmake/OptionsWinUWP.cmake`
   - `Source/WTF/wtf/PlatformWinUWP.cmake`
   - `Source/JavaScriptCore/PlatformWinUWP.cmake`
   - `Source/WebCore/PlatformWinUWP.cmake`
3. Small follow-up fixes that a clean build from the tag needed, all under
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

## Apply

From the root of a WebKit checkout at the base tag:

```powershell
git checkout webkitgtk-2.52.4
git apply --3way path\to\wk-winuwp.patch
```

The patch is generated with paths relative to the WebKit root, so it should be
applied from that root. Keep the tree LF-only (the patch is LF; with
`core.autocrlf=true` normalise the patch file before `git apply`). Review the
resulting diff before building.

## Regenerate

On a branch carrying the changes on top of the tag:

```powershell
git diff webkitgtk-2.52.4 HEAD > wk-winuwp.patch
```

## Current state of the Project Apotheosis `winuwp` branch

The patch above is the author's original publication and is left as it is. The
fork's own branch has moved on; `git diff webkitgtk-2.52.4` there is currently
**140 files, 12971 insertions and 115 deletions**, and PATCHLOG.md has one row
per commit with the reason for it.

Latest structural change, TileGrid package 4 (`5e052e57f8`), in
`Source/WebCore/platform/graphics/texmap/`:

- new since the tag: `TextureMapperTiledStore.h`,
  `TextureMapperTileGridStore.{h,cpp}`, `TextureMapperTileGridCore.{h,cpp}`,
  `TextureMapperTileGridModel.{h,cpp}`, `TextureMapperTileGridMachines.h`
  (plus `Source/ThirdParty/sml/`);
- back to the tag byte for byte: `TextureMapperTile.{h,cpp}`;
- `TextureMapperTiledBackingStore.{h,cpp}` reduced to upstream plus the store
  interface and viewport-limited tiling (see BLOCKERS.md in the docs repo).

The texmap directory's share of the diff went from 8656 added lines in 21 files
to 6109 in 19.

This is the source artifact requested in
[Issue #3](https://github.com/MoonlightLabCN/Project-Apotheosis/issues/3).
