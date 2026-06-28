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

## 🔧 Build (summary) / 构建概要

ARM32 UWP toolchain — **clang-cl** (`--target=thumbv7-unknown-windows-msvc`) + **lld-link** for the engine/driver, **MSVC v143 (ARM)** for the harness. Dependencies (Cairo / ICU / libcurl / FreeType / HarfBuzz / ANGLE …) are pre-built as ARM `.lib`s, then:

```powershell
# 1. compile + link the WebCore driver  →  WebCoreDriver-gpu.lib
pwsh -File port\link-driver-gpu.ps1

# 2. build the host appx  (MSBuild v143 ARM, two-pass XAML markup compile)
pwsh -File port\build-harness.ps1

# 3. deploy to a real device over Device Portal and launch
pwsh -File tools\deploy-launch.ps1 -Ip <device-ip> -Ver <version>
```

### Key constraints / 关键约束 (踩坑前必读)

- **ASCII-only paths** — the Ruby/meson code generators choke on non-ASCII paths; the repo lives at `E:\Apotheosis`, vcpkg at `C:\vcpkg`.
- **C++ exceptions OFF** — clang's `thumbv7-windows-msvc` backend can't lower `cleanupret`; build with `_HAS_EXCEPTIONS=0` + `/EHs-c-`.
- **All upstream edits are guarded** with `#if defined(WK_WINUWP)` + an `Apotheosis:` comment, so the port never pollutes upstream semantics.
- **Software rendering is the universal base; GPU is a runtime switch** gated strictly on `g_gpuActive` (defaults to Cairo + EmptyChromeClient → zero regression when GPU isn't up).

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
