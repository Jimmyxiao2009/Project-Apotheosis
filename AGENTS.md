# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## 这是什么

**Project Apotheosis / EdgeHTML Reborn** —— 把现代 WebKit/WebCore（webkitgtk-2.52.4）移植到 **Windows 10 Mobile / Lumia 950（ARM32, UWP, App Container）**，让被微软放弃的 Windows Phone 跑真实现代网页，带 JIT 与 GPU 合成。它是"复活 Win10M 生态"大计划的浏览器引擎组件。

真机（Lumia 950, Win10M 15254）已验证跑通：WTF+JSC CLoop → WebCore+Cairo 软渲染（Bing/GitHub/Apple 等真实站点）→ 真实事件交互 → JSC JIT → ANGLE(D3D11 FL9_3)+TextureMapper GPU 合成直呈现到 SwapChainPanel → 平滑滚动 + 捏合缩放。当前在演进 UI 向 Safari/Edge 形态。

## 关键约束（先读，违反必踩坑）

- **只能用 ASCII 路径**：构建链（Ruby 代码生成器 / meson）对非 ASCII 路径敏感 → 仓库必须在 `E:\Apotheosis`，vcpkg 在 `C:\vcpkg`。原中文路径 `E:\Desktop\项目\…` 炸过生成器。脚本里大量 `E:\Apotheosis\…` 是硬编码绝对路径。
- **三套工具链并存，别混**：
  - 引擎 WTF/JSC/WebCore = **clang-cl**（`--target=thumbv7-unknown-windows-msvc`，WebKit 已弃纯 MSVC）。
  - 移植驱动 `port/*.cpp` = clang-cl，链 **lld-link**。
  - harness（C++/CX UWP）= **MSVC v143（工具集 14.44.35207）ARM**。因为 VS18 砍了 arm32 vcvars、SDK 26100 删了 arm32 库 → 用 **SDK 22621** 的 arm 库，并用 `port\arm32-uwp-env.ps1` 手搓 ARM32 环境（INCLUDE/LIB/PATH）绕过被移除的 vcvars。
- **C++ 异常必须关**：clang 的 thumbv7-windows-msvc 后端无法 lower `cleanupret`（Windows 异常展开）→ `_HAS_EXCEPTIONS=0` + `/EHs-c-`。
- **所有上游 WebKit 改动用 `#if defined(WK_WINUWP)` 守卫 + `Apotheosis:` 注释**，只影响本 port，不污染上游语义。
- **软件渲染是通用底座，GPU 运行时切换**：合成开关严格 gate 在 `g_gpuActive`（默认 false，仅 `WebCoreGpuInit` 成功后置 true）。GPU 未起时回 Cairo 软渲染 + EmptyChromeClient（零回归）。曾经无条件开合成导致真机静默闪退（`__fastfail`，无 dump）。

## 仓库布局 / 什么被跟踪

仓库**只跟踪移植层与宿主**，不含 GB 级上游与可重下二进制：

- `port/` —— WebCore 驱动 + Port 层客户端 + 各 stub + 构建/链接脚本。⚠️ 真源码混在**大量一次性调试残留**里（`repro_*.cpp`、`mangle-repro*`、`*.obj/*.lib/*.dll`、`*.log`、`undef-*.txt`、`_*.bat`）——这些是趟编译墙时的实验件，可忽略。核心源码见下。
- `harness/` —— UWP 宿主 App（C++/CX、XAML、`Package.appxmanifest`、签名证书 `.cer`/`.pfx`）。
- `tools/` —— Device Portal（WDP）远程部署 / 抓崩溃 dump / 自动诊断脚本。
- `angle/include` —— ANGLE 头（跟踪）；`angle/arm`、`angle-windowsstore` 二进制 gitignore（可重下）。
- **不在仓库**：`WebKit/`（sparse webkitgtk-2.52.4，GB 级，gitignore；上游 ARM32/App-Container 补丁清单记在**项目记忆**而非仓库）、`build-clang-*/` `build-release/` `deps-build/`（构建输出）、字体、`*.pfx`、`*.log`。

**核心移植层源码**（在 `port/` 一堆实验件中）：

- `WebCoreDriver.cpp` / `WebCoreDriver.h` —— 引擎对外的 C ABI + 常驻 Page 会话（导航、真实事件派发、链接提取、软/硬呈现）。
- `PortChromeClient.{h,cpp}` —— 非 final 的 `ChromeClient` 子类（EmptyChromeClient 合成钩子是 `final` 不能覆写），开 GPU 合成、捕获根 `GraphicsLayer`、`triggerRenderingUpdate` 置 needsPresent。
- `LoadingFrameLoaderClient.{h,cpp}` —— 真策略回调的 `FrameLoaderClient`（非 Empty，`PolicyAction::Use`）。
- `PortPlatformStrategies` / `PortNetworkStorageSession` —— 装 LoaderStrategy、网络存储会话。
- `stubs-*.cpp` —— 平台未实现符号 stub（crypto/network/pasteboard/ax/loader/other）。
- `Toolchain-ARM32-UWP-clang.cmake` / `arm32-uwp-env.ps1` / `clang-cl-arm-shim.h` —— 工具链与环境。

## 架构（大图，要读多文件才看得清）

三层经 C ABI 解耦：

```
Harness (C++/CX UWP, MSVC v143)         harness/
  · MainPage: 地址栏/工具栏 + 触摸手势 → 引擎滚动/点击/缩放/选择
  · GpuPanel (SwapChainPanel) ← GPU 直呈现 | RenderImage (WriteableBitmap) ← 软件回退
        │  C ABI = WebCoreDriver.h  (extern "C")
WebCoreDriver (port/, clang-cl → WebCoreDriver-gpu.lib)
  · 常驻 Page/Frame/FrameView 会话、真实事件派发、链接提取
  · 两条呈现路：Cairo paintToRGBA（软件） | TextureMapper→ANGLE swapchain（GPU）
  · PortChromeClient / LoadingFrameLoaderClient / Port*Strategies
        │
WebCore / JavaScriptCore / WTF (clang-cl, thumbv7-windows-msvc, App Container)
  · ARM32 / App Container 补丁全部 WK_WINUWP 守卫；上游源不在本仓库
```

- **线程铁律**：present **只在引擎线程**；UI 线程**绝不同步 wait 引擎**（ANGLE 把 surface create/resize marshal 回 panel dispatcher，互等 = 死锁，`RunOnUIThread` 超时会 `std::terminate`）。所有 C ABI 调用在**单一引擎线程**串行化。
- **C ABI 有两份副本**：`port/WebCoreDriver.h` 与 `harness/WebCoreDriver.h`。加/改导出时**两份必须同步**，否则 ABI 不一致。
- **GPU 合成 recipe**（`WebCoreComposite`，镜像 WebKit 的 `WCScene::update`）：`flushCompositingStateIncludingSubframes` → `updateBackingStoreIncludingSubLayers` → `applyAnimationsRecursively` → `beginPainting`/`paint`/`endPainting` → `eglSwapBuffers`（直呈现）或 `glReadPixels`（离屏 readback 验证）。根层 = `PortChromeClient::rootLayer()`，实为同步 `GraphicsLayerTextureMapper`（`USE_COORDINATED_GRAPHICS=0`）。

引擎有**三个构建配置**（同一份带 WK_WINUWP 补丁的 WebKit 源，不同 CMake 开关）：

| 构建目录 | 配置 | 用途 |
|---|---|---|
| `build-clang-webcore` | Cairo 软渲染 | Phase 1b 基线 |
| `build-clang-jit` | + JSC JIT | JIT 线 |
| `build-clang-gpu` | + GPU（TextureMapper+ANGLE）+ JIT | **当前开发线** |

分支：`gpu-path1` = 当前开发线（对应 `build-clang-gpu`）；`master` = 纯 JIT 封存线。

## 常用命令（PowerShell 7 / pwsh）

改 `port/*.cpp` 驱动后，重编 + 重链 GPU 驱动（产出 `WebCoreDriver-gpu.lib`）：

```powershell
pwsh -File E:\Apotheosis\port\link-driver-gpu.ps1
```

单文件编译验证（快，定位编译错，看 `port\driver-compile-gpu.log`）：

```powershell
pwsh -File E:\Apotheosis\port\compile-driver-gpu.ps1 E:\Apotheosis\port\WebCoreDriver.cpp E:\Apotheosis\port\WebCoreDriver.gpu.obj
```

改了上游 WebCore 源（WK_WINUWP 补丁）后，增量重编引擎，再重链驱动：

```powershell
. E:\Apotheosis\port\arm32-uwp-env.ps1
& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -C E:\Apotheosis\build-clang-gpu WebCore
pwsh -File E:\Apotheosis\port\link-driver-gpu.ps1
```

构建 harness appx（默认 VS2017 官方 C++/CX XAML codegen，再由 VS18/v143 ARM 全量编译、链接和打包）：

```powershell
pwsh -File E:\Apotheosis\port\build-harness.ps1
# 完整干净验证：pwsh -File E:\Apotheosis\port\build-harness.ps1 -Clean
# 应急 fallback：pwsh -File E:\Apotheosis\port\build-harness.ps1 -XamlMode Fallback
# 看 harness-build.log；appx 在 harness\AppPackages\Harness\Harness_<ver>_ARM_Test\
```

部署到真机并启动（交互测，不轮询）：

```powershell
pwsh -File E:\Apotheosis\tools\deploy-launch.ps1 -Ip <设备IP> -Ver <版本号>
```

全自动诊断回路（卸→装→启→轮询拉 `LocalState` 的 dump/BMP 截图；仅当设备里有 `autodiag.txt` 时触发）：

```powershell
pwsh -File E:\Apotheosis\tools\auto-diag2.ps1
```

- **JIT（非 GPU）线**对应：`port\configure-jit.ps1` / `link-driver-jit.ps1` / `compile-driver-jit.ps1`；首次配引擎用 `port\configure-gpu.ps1` 等。
- 升版本号改 `harness\Package.appxmanifest`，deploy 脚本 `-Ver` 要对上。
- 量 appx 大小用 PowerShell `.Length`（**别用 `ls -la`**，Windows 属主名带空格会把列读偏）。
- 这是 **x64 构建机，ARM32 appx 跑不了**——引擎验证唯一靠真机。设备常因省电掉 WiFi，部署易传一半断，用 `tools\Deploy-Robust.ps1` 容错重试；远程时只产出 appx 交用户部署。

## 真机部署前置

设备：开机、同一 WiFi、设置→面向开发人员→开 **Device Portal**。appx 依赖 `Microsoft.VCLibs.140.00 (ARM)`（设备多半已由其他 -Reborn 应用装上）。**HTTPS 在 App Container 无系统证书库** → 打包 `cacert.pem`，启动时 `WebCoreSetCACertPath` 注入（curl/OpenSSL 自带 TLS 1.3，不靠 OS 的只到 1.2 的 Schannel）。

## 项目记忆（深层背景在这）

每个里程碑的**根因 / 试错 / 真机数据点**、以及**上游 WebKit 补丁清单**都在 Codex 项目记忆（`MEMORY.md` 索引 + 各 `.md`），不在仓库里。动手前先扫 `MEMORY.md`。仓库现状与架构概述见 `PROJECT-OVERVIEW.md`。
