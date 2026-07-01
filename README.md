# Project Apotheosis — EdgeHTML Reborn

> **A modern WebKit/WebCore browser engine, brought back to life on Windows 10 Mobile.**
> 把现代 **WebKit / WebCore** 渲染引擎移植回 **Windows 10 Mobile**,让被微软放弃的 Windows Phone 重新跑上真实的现代网页。

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: Windows 10 Mobile](https://img.shields.io/badge/platform-Windows%2010%20Mobile-0078D6.svg)](#-platform--目标平台)
[![Build: 14393](https://img.shields.io/badge/target%20build-14393-0078D6.svg)](#-platform--目标平台)
[![Arch: ARM](https://img.shields.io/badge/arch-ARM-5E5E5E.svg)](#-platform--目标平台)
[![Latest release](https://img.shields.io/github/v/release/Jimmyxiao2009/Project-Apotheosis?label=latest%20release&color=success)](https://github.com/Jimmyxiao2009/Project-Apotheosis/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Jimmyxiao2009/Project-Apotheosis/total.svg?color=blueviolet)](https://github.com/Jimmyxiao2009/Project-Apotheosis/releases)

---

Most of the web stopped working on Windows Phone years ago — its old EdgeHTML engine froze in time while the web moved on. **Apotheosis** ports the modern **WebKit/WebCore** engine (webkitgtk-2.52.4) to **Windows 10 Mobile on ARM**, complete with a **JIT-accelerated** JavaScript engine and **GPU compositing**, and proves it on real hardware — a **Lumia 950**.

Windows Phone 的老 EdgeHTML 引擎多年前就跟不上现代 Web 了。本项目把现代 WebKit/WebCore 引擎移植到 **Windows 10 Mobile (ARM)**,带 **JSC JIT** 与 **GPU 合成**,并在真机 **Lumia 950** 上验证跑通。

## ✨ What works today / 现状

Verified on a real device (Lumia 950 · ARM · Windows 10 Mobile):

| | Milestone | 说明 |
|---|---|---|
| ✅ | **WTF + JavaScriptCore (CLoop)** | 引擎核心真机运行(Phase 0) |
| ✅ | **WebCore layout + Cairo software rendering** | Bing / GitHub / Apple / 微软 等真实站点正确渲染 |
| ✅ | **Live interactive session** | 真实鼠标事件转发(点击 / 表单 / 链接导航)、滚动触发懒加载、屏幕键盘输入 |
| ✅ | **JIT** | 加 `codeGeneration` capability 后 App Container 放行可执行内存,JSC JIT 真机生效(~5–50×) |
| ✅ | **GPU compositing** | ANGLE (D3D11 FL9_3) + WebCore **TextureMapper**,合成直呈现到 `SwapChainPanel` |
| ✅ | **Smooth scroll / pinch-zoom (M3/M4)** | 直呈现快滚 + 实时缩放变换 + 按新尺度重栅格 |
| ✅ | **Browser shell (0.1.8)** | 标签页、智能地址栏、设置、页内查找、动作菜单、检测更新、LBrowser 风底栏 UI |
| 🚧 | **UI polish** | 向 Safari / Edge 形态持续演进中 |

## 📥 Download / 下载

Grab the latest signed test package from the **[Releases page](https://github.com/Jimmyxiao2009/Project-Apotheosis/releases/latest)**:

- **`Harness_<ver>_ARM.appx`** — the browser, for ARM Windows 10 Mobile.
- **`Harness_<ver>_ARM.cer`** — sideloading certificate (install first if the package isn't trusted).

### Install on device / 真机安装 (Lumia 950 etc.)

1. Phone → **Settings → Update & security → For developers → enable Device Portal**, join the same Wi-Fi.
2. Open the device's **Device Portal** in a browser → **Apps** → upload and install `Harness_<ver>_ARM.appx`.
3. If prompted, install the bundled **`.cer`** first so the test signature is trusted.

> Depends on **`Microsoft.VCLibs.140.00 (ARM)`** — most devices already have it from other *-Reborn* apps.
> HTTPS works in App Container via a bundled `cacert.pem` injected at startup (the OS Schannel only reaches TLS 1.2; the engine's own curl/OpenSSL does TLS 1.3).

## 🏗 Architecture / 架构

Three layers decoupled through a stable **C ABI**:

```
┌──────────────────────────────────────────────────────────┐
│  Harness — UWP host app  (C++/CX, MSVC v143, ARM)         │  harness/
│   · Toolbar / address bar / tabs / find-in-page / settings │
│   · Touch gestures → engine scroll / tap / pinch-zoom      │
│   · GpuPanel (SwapChainPanel) ← GPU direct present         │
│   · RenderImage (WriteableBitmap) ← software fallback      │
└───────────────────────────┬──────────────────────────────┘
                            │  C ABI  ·  WebCoreDriver.h
┌───────────────────────────▼──────────────────────────────┐
│  WebCoreDriver  (clang-cl → WebCoreDriver-gpu.lib)         │  port/
│   · Resident Page/Frame session, real event dispatch       │
│   · Two paint paths:  Cairo paintToRGBA  |  TextureMapper   │
│   · PortChromeClient / LoadingFrameLoaderClient / Strategies│
└───────────────────────────┬──────────────────────────────┘
                            │
┌───────────────────────────▼──────────────────────────────┐
│  WebCore / JavaScriptCore / WTF                            │  (upstream)
│   clang-cl · thumbv7-windows-msvc · App Container          │  not in repo
│   ARM32 / App-Container patches all guarded by WK_WINUWP    │
└──────────────────────────────────────────────────────────┘
```

**Threading rule:** *present* runs **only on the engine thread**; the UI thread never synchronously waits on the engine (mutual wait = deadlock). All C ABI calls are serialized on a single engine thread.

The engine has three build configurations from one patched WebKit source tree:

| Build dir | Config | Purpose |
|---|---|---|
| `build-clang-webcore` | Cairo software render | Phase 1b baseline |
| `build-clang-jit` | + JSC JIT | JIT line |
| `build-clang-gpu` | + GPU (TextureMapper + ANGLE) + JIT | **current dev line** |

Branches: **`gpu-path1`** = active dev line (`build-clang-gpu`) · **`master`** = JIT-only archive line.

## 📂 What's tracked / 仓库内容

This repo tracks **only the port layer and the host** — not the gigabytes of upstream WebKit, nor re-downloadable binaries:

- **`port/`** — WebCore driver, port-layer clients, symbol stubs, build/link scripts.
  Core sources: `WebCoreDriver.{cpp,h}` · `PortChromeClient.{h,cpp}` · `LoadingFrameLoaderClient.{h,cpp}` · `PortPlatformStrategies` · `stubs-*.cpp` · `Toolchain-ARM32-UWP-clang.cmake` · `arm32-uwp-env.ps1`.
- **`harness/`** — UWP host app (C++/CX), XAML UI, `Package.appxmanifest`, signing cert.
- **`tools/`** — Device Portal (WDP) remote deploy / crash-dump capture / auto-diagnostics.
- **`angle/include`** — ANGLE headers (binaries are gitignored, re-downloadable).

> **Not in repo:** upstream `WebKit/` (sparse webkitgtk-2.52.4, GB-scale), all `build-*/` outputs, fonts, `*.pfx`, `*.log`.

## 🔧 Build / 构建

This is a **Windows-only, x64-build-machine** project — the produced binary is ARM32 and can only actually *run* on a real Windows 10 Mobile device ("Verify on device" below). Everything up to packaging the appx builds fine on an x64 dev box; nothing ARM32 runs locally.

### Prerequisites / 前置环境

- **Repo path must be pure ASCII** — the Ruby/meson code generators choke on non-ASCII paths. This repo assumes `E:\Apotheosis` and vcpkg at `C:\vcpkg`.
- **Three toolchains, don't mix them up:**
  - WTF/JSC/WebCore + `port/*.cpp` → **clang-cl**, `--target=thumbv7-unknown-windows-msvc`, linked with **lld-link** (WebKit has dropped pure-MSVC ARM32 support).
  - `harness/` (C++/CX UWP) → **MSVC v143 (toolset 14.44.35207) ARM**. Newer Visual Studio removed ARM32 `vcvars` and SDK 26100 dropped the ARM32 libs, so this repo hand-rolls the ARM32 environment (`INCLUDE`/`LIB`/`PATH`) from the **Windows SDK 22621** ARM libs via `port\arm32-uwp-env.ps1`, instead of relying on a (now-missing) vcvars script.
- Prebuilt ARM `.lib`s for the engine's dependencies (Cairo, ICU, libcurl, FreeType, HarfBuzz, ANGLE.WindowsStore …).
- A full checkout of **webkitgtk-2.52.4** with this project's ARM32/App-Container patches applied at `WebKit/` (not in this repo — GB-scale upstream source; the patch list lives in the project's own notes, not tracked here). All patch sites are guarded with `#if defined(WK_WINUWP)` + an `Apotheosis:` comment so they never change upstream semantics when the flag is off.

### Engine + driver / 引擎与驱动

```powershell
# First time only: configure the GPU engine build (CMake, out-of-tree)
pwsh -File port\configure-gpu.ps1

# After changing upstream WebCore sources (WK_WINUWP-guarded patches):
# incrementally rebuild the engine, then relink the driver
. port\arm32-uwp-env.ps1
& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -C build-clang-gpu WebCore
pwsh -File port\link-driver-gpu.ps1

# After changing only port/*.cpp (driver layer): just relink
pwsh -File port\link-driver-gpu.ps1
# → produces WebCoreDriver-gpu.lib

# Fast single-file syntax check while iterating (see port\driver-compile-gpu.log)
pwsh -File port\compile-driver-gpu.ps1 port\WebCoreDriver.cpp port\WebCoreDriver.gpu.obj
```

There's a parallel non-GPU **JIT-only** line (`master` branch, `build-clang-jit`) using `port\configure-jit.ps1` / `link-driver-jit.ps1` / `compile-driver-jit.ps1` — same idea, no TextureMapper/ANGLE.

### Host app (harness) / 宿主 App

```powershell
# Only needed after editing XAML (MainPage.xaml / App.xaml) — see note below
pwsh -File port\gen-xaml-codebehind.ps1

# Full build + package (MSBuild v143 ARM) → appx under harness\AppPackages\Harness\
pwsh -File port\build-harness.ps1
# see harness-build.log for details
```

> **XAML is not markup-compiled.** A system update broke the deprecated C++/CX `XamlCompiler` (null-ref crash on *every* installed SDK). Instead, `port\gen-xaml-codebehind.ps1` parses `MainPage.xaml` and generates `harness\xamlgen\MainPage.g.hpp`: at runtime it loads the XAML as an embedded string via `XamlReader::Load`, binds every `x:Name` field via `FindName`, and wires every event handler — all by hand, no compiler involved. If you add a control with a new `x:Name`, the generator won't auto-declare its field; add one line yourself to `harness\xamlgen\MainPage.g.h` (`private: <Type>^ <Name>;`).

### Verify on device / 真机验证

There is no ARM32 emulator here — the **only** way to confirm a change actually works is a real device (Lumia 950 or similar) over **Device Portal** (Settings → Update & security → For developers → enable Device Portal, same Wi-Fi as the build machine).

```powershell
# Install + launch on the device
pwsh -File tools\deploy-launch.ps1 -Ip <device-ip> -Ver <version>

# Flaky Wi-Fi (phones sleep and drop the connection) → retry-hardened variant
pwsh -File tools\Deploy-Robust.ps1 -Ip <device-ip> -Ver <version>
```

Bump `Version` in `harness\Package.appxmanifest` before each release build so the app's in-app "check for update" can tell versions apart (`-Ver` in the deploy scripts must match).

### Key constraints / 关键约束 (踩坑前必读)

- **C++ exceptions OFF** — clang's `thumbv7-windows-msvc` backend can't lower `cleanupret`; build with `_HAS_EXCEPTIONS=0` + `/EHs-c-`.
- **All upstream edits are guarded** with `#if defined(WK_WINUWP)` + an `Apotheosis:` comment, so the port never pollutes upstream semantics.
- **Software rendering is the universal base; GPU is a runtime switch** gated strictly on `g_gpuActive` (defaults to Cairo + EmptyChromeClient → zero regression when GPU isn't up).
- **Threading rule:** *present* only ever happens on the engine thread; the UI thread never synchronously waits on the engine (mutual wait = deadlock, and a timed-out `RunOnUIThread` calls `std::terminate`). All C ABI calls are serialized on one engine thread.
- **The C ABI has two copies** — `port\WebCoreDriver.h` and `harness\WebCoreDriver.h`. Adding or changing an exported function means editing both, or the two sides silently disagree.

See **[`CLAUDE.md`](CLAUDE.md)**, **[`HANDOFF.md`](HANDOFF.md)** and **[`M2-HANDOFF.md`](M2-HANDOFF.md)** for the deep details.

## 🎯 Platform / 目标平台

- **OS:** Windows 10 Mobile, build **14393**+
- **Architecture:** **ARM** (ARM32)
- **Devices:** Lumia 950 / 950 XL and other ARM Windows 10 Mobile hardware.

## 📜 License / 授权

This repository's own code (`port/`, `harness/`, `tools/`) is licensed under the **[MIT License](LICENSE)**.

It builds against upstream **WebKit / WebCore / JavaScriptCore / WTF**, which is *not* included here and remains under its own licenses (primarily **LGPL-2.1** and **BSD**). Bundled third-party runtimes (Cairo, ICU, libcurl, FreeType, HarfBuzz, ANGLE …) keep their respective upstream licenses.

本仓库自有代码以 **MIT** 授权;所链接的上游 WebKit/WebCore/JSC/WTF 及第三方运行库不在本仓库内,各自保留其原始授权(主要 LGPL-2.1 / BSD)。

---

*Reviving the Windows Phone web for the people who never let it die.* 📱
