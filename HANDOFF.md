# Project Apotheosis — 交接文档

> 把 **2026 年的 WebKit** 移植到 **Windows 10 Mobile / Lumia 950（ARM32 UWP，App Container）**。
> 复活 Windows Phone 生态的核心基础设施（浏览器引擎)。代号 **Apotheosis**。
> 最后更新：2026-06-14。

---

## 0. 一句话现状

- **Phase 0 = 完成 ✅**：modern WebKit 的 **JavaScriptCore**（CLoop 解释器，无 JIT）编译成 ARM32，
  打包成 UWP appx，**已在真机 Lumia 950 上运行**，屏幕显示 `1+1=2` / `Σ1..100=5050`。
- **Phase 1 = 进行中 🔨（2026-06-15 大跃进）**：WebCore + 渲染。
  - **字体栈定为 FreeType+Fontconfig+HarfBuzz(Cairo 软渲染)**(多智能体测真实源码;推翻早先 DirectWrite 设想,见 §7 与 NIGHT-LOG)。
  - **依赖层全部为 arm-uwp 编出**:fontconfig / cairo(重编带 ft+fc) / harfbuzz / sqlite3(WinRT VFS) 手工交叉编;libjpeg-turbo / libwebp / libxml2 由 **vcpkg 直接编**(vcpkg 能编 CMake 类 arm-uwp 端口,只 autotools/meson 不行)。
  - **WebCore 已 `cmake configure` 通过**(`build-clang-webcore`,脚本 `port\configure-phase1.ps1`);WTF+JSC **编译通过**;WebCore 代码生成全过;**WebCore 源码编译进行中**(逐个 App Container 补丁,规律同 §5 的 ~10× 量)。
  - **网络栈(curl+openssl+psl)延后到 Phase 1b**:静态 HTML 渲染不需其功能,但 WebCore 最终链接需符号。
  - 详细夜间作业记录:`E:\Apotheosis\NIGHT-LOG-2026-06-15.md`。

---

## 1. 机器与工具链（关键，先读这节）

| 组件 | 位置 / 版本 | 用途 |
|---|---|---|
| 构建机 | Windows 11 x64，本机 IP `192.168.3.114` | 编译 |
| 目标设备 | Lumia 950「JimmyXiao」，Win10M `15254.603 armfre`(1709)，IP 动态(曾 .51) | 运行 |
| VS2026 | `C:\Program Files\Microsoft Visual Studio\18\Community`（VS"18"，18.6.3） | — |
| **MSVC v143 ARM cl** | 工具集 **14.44.35207**，`...\VC\Tools\MSVC\14.44.35207\bin\Hostx64\arm\cl.exe` | 编 harness（C++/CX） |
| **clang-cl** | **LLVM 22.1.7** `C:\Program Files\LLVM\bin\clang-cl.exe` | 编 WebKit（WTF/JSC/WebCore） |
| VS2017 v141 | `C:\Program Files (x86)\Microsoft Visual Studio\2017\Community`，工具集 14.16 | 给 vcpkg 提供 arm32 vcvars |
| Win10 SDK | 用 **10.0.22621.0**（含 arm32 库；26100 已删 arm32！） | — |
| Ruby/Perl/Python | Ruby `C:\Ruby33-x64`、Perl(Git) `C:\Program Files\Git\usr\bin`、Python(hermes venv) | WebKit 代码生成 |
| vcpkg | `C:\vcpkg`（ASCII 路径！），overlay triplet `C:\vcpkg-overlay\triplets\arm-uwp.cmake` | 依赖 |
| 代理 | `HTTP_PROXY=HTTPS_PROXY=http://127.0.0.1:7897`（v2ray） | 下载 |

### 三大结构性工具链坑（务必理解）
1. **VS2026 的 vcvarsall 移除了 arm32 target**（只剩 arm64）。但 14.44 的 arm cl 二进制还在、能跑 →
   **手搓 ARM32 环境**绕过 vcvars：`port\arm32-uwp-env.ps1`（设 INCLUDE/LIB/PATH 指向 14.44 arm + SDK 22621 arm）。
2. **新 SDK 26100 删光 arm32 库**（WindowsApp.lib/ucrt 的 arm 都没了）→ 一律用 **22621** 的 arm 库（8.3 短路径
   `C:\PROGRA~2\WI3CF2~1\10\Lib\100226~1.0\{um,ucrt}\arm`，含空格的长路径会被命令行拆断）。
3. **WebKit 已弃纯 MSVC**（`OptionsMSVC.cmake` 第一行："only for clang-cl"），代码里 `__attribute__`/`__PRETTY_FUNCTION__`
   遍地 → WebKit 必须用 **clang-cl**（`--target=thumbv7-unknown-windows-msvc`）。但 harness 只调 JSC 的 C API（纯 C），
   可以用 MSVC cl 编，二者 COFF-ARM + MSVC ABI 兼容。

---

## 2. 目录结构

```
E:\Apotheosis\                       ← ASCII 路径!（原 E:\Desktop\项目\... 的中文 "项目" 炸了 Ruby 生成器）
├─ WebKit\                           ← webkitgtk-2.52.4，sparse checkout(只 Source\WTF/JavaScriptCore/cmake/bmalloc/ThirdParty\capstone)
│  └─ Source\cmake\OptionsWinUWP.cmake        ← 我们的新 port
│  └─ Source\WTF\wtf\PlatformWinUWP.cmake     ← WTF 平台源码选择
│  └─ Source\JavaScriptCore\PlatformWinUWP.cmake
│  └─（多处 WTF/win 及个别 JSC 源码改动，全用 WK_WINUWP 守卫，见 §5）
├─ port\                             ← 所有构建脚本(版本控制这个)
│  ├─ arm32-uwp-env.ps1              ← 注入手搓 ARM32 环境(INCLUDE/LIB/PATH)
│  ├─ Toolchain-ARM32-UWP.cmake      ← Path A: 纯 MSVC(已弃)
│  ├─ Toolchain-ARM32-UWP-clang.cmake← Path B: clang-cl(在用)
│  ├─ clang-cl-arm-shim.h            ← /FI 强制包含,补 _CountLeadingZeros 等
│  ├─ configure-phase0.ps1           ← 配 WTF+JSC(加 -Clang 走 clang 工具链)
│  ├─ vcpkg-triplets\arm-uwp.cmake   ← overlay triplet(记录副本)
│  ├─ build-cairo.ps1 / cairo-cross-clang.txt ← Phase 1 cairo 手动构建
│  ├─ deploy-phase0.ps1 / watch-and-deploy.ps1 ← 部署
├─ build-clang-release\              ← WTF/JSC 构建输出 → lib\JavaScriptCore.lib, WTF.lib
├─ harness\                          ← Phase 0 验证 app(C++/CX UWP)
│  └─ AppPackages\Harness\Harness_0.1.0.0_ARM_Test\Harness_0.1.0.0_ARM.appx  ← 成品(18.3MB,已签名)
│  └─ EdgeHTMLReborn.cer             ← 自签名证书(设备需信任)
├─ tools\Wdp-Deploy.ps1             ← WDP 部署工具(有 CSRF 403 bug,见 §6)
└─ deps-build\                       ← Phase 1 依赖的 meson 构建目录

C:\vcpkg\installed\arm-uwp\          ← 所有第三方依赖装这(ICU/pixman/zlib/png/expat/cairo...)
C:\icu-arm-uwp\                      ← ICU 78(手动组装,Phase 0 用)
```

---

## 3. 如何构建 Phase 0（JSC + harness）

```powershell
# 1) 配置 WTF + JavaScriptCore(clang-cl)
E:\Apotheosis\port\configure-phase0.ps1 -Clang -Config Release -IcuRoot C:\icu-arm-uwp

# 2) 编 JSC 静态库(在 arm 环境里跑 ninja)
. E:\Apotheosis\port\arm32-uwp-env.ps1
& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" `
   -C E:\Apotheosis\build-clang-release JavaScriptCore
#   → build-clang-release\lib\JavaScriptCore.lib (40MB, coff-arm) + WTF.lib

# 3) 编 harness UWP app + 打包 appx(MSBuild,v143 ARM 平台)
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
   E:\Apotheosis\harness\Harness.vcxproj /p:Configuration=Release /p:Platform=ARM
#   → harness\AppPackages\Harness\Harness_0.1.0.0_ARM_Test\Harness_0.1.0.0_ARM.appx (18.3MB)
```

要点：harness 用 **MSBuild v170 集成的 ARM 平台**（v180 砍了 → vcxproj 设 `PlatformToolset=v143`）；`/utf-8`；
pch.h include App/MainPage 头；.xaml.cpp include 对应 `.g.hpp`；定义 `STATICALLY_LINKED_WITH_JavaScriptCore`+`_WTF`；
ICU 的 3 个 dll 用 `<DeploymentContent>` 打进 appx（否则设备闪退）。

---

## 4. 如何部署到真机

设备需：开机、连同一 WiFi、设置→更新和安全→面向开发人员→开 **Device Portal**，记下 IP/端口。

**首选：Device Portal 网页**（避开下面的 CSRF bug）：
浏览器 `https://<设备IP>` → Apps → Install app 选 `Harness_0.1.0.0_ARM.appx` + Certificate 选 `Harness_0.1.0.0_ARM.cer` → Install。
装好点 **"JSC Reborn"** 图标 → 看绿字结果。

**脚本（设备无密码时）**：`E:\Apotheosis\port\deploy-phase0.ps1 -DeviceIp <IP>`
**自动守望**：`E:\Apotheosis\port\watch-and-deploy.ps1`（扫子网，发现 Win10M 自动部署）。

---

## 5. Phase 0 全部源码改动（理解"为什么是这样")

所有改动用 `#if defined(WK_WINUWP)` 守卫（toolchain 定义 `WK_WINUWP=1`），只影响本 port。

**CMake/port:**
- `OptionsWinUWP.cmake`：基于 JSCOnly；`ENABLE_C_LOOP=ON/JIT=OFF/USE_SYSTEM_MALLOC=ON`(ARM32-Win 默认即如此)；
  `USE_CAPSTONE=FALSE`(无 JIT)；clang-cl 分支设 `/utf-8 /GS`、**strip `/EHsc` + 加 `/EHs-c-` + `_HAS_EXCEPTIONS=0`**(关异常)；
  clang_rt 非 REQUIRED(LLVM 无 arm compiler-rt，靠 MSVC CRT)。注册进 `WebKitCommon.cmake` 的 ALL_PORTS。
- `WebKitCompilerFlags.cmake`:原子测试 `--std=c++17` → MSVC 时改 `/std:c++17`(clang-cl 忽略 `--std`)。
- `JavaScriptCore/CMakeLists.txt`:`LLIntOffsets/SettingsExtractor` 的 `bmalloc_CopyHeaders` 依赖按 `USE_SYSTEM_MALLOC` 条件化。
- toolchain 补 GCC 内建宏(纯 MSVC 路径用,clang-cl 路径**不要**):`__SIZEOF_POINTER__=4`、`__BYTE_ORDER__` 等。

**WTF/JSC 源码(App Container / clang-arm 适配):**
- `RandomDevice.cpp`:CryptGenRandom → **BCryptGenRandom**(`BCRYPT_USE_SYSTEM_PREFERRED_RNG`)。
- `win/OSAllocatorWin.cpp`:VirtualAlloc/VirtualAlloc2 → **VirtualAllocFromApp/VirtualAlloc2FromApp**；去 VirtualUnlock。
- `win/FileSystemWin.cpp`:CreateFileW → **CreateFile2**；SHGetFolderPath/CSIDL → stub(返回空)。
- `CurrentTime.cpp`:去 timeBeginPeriod/timeEndPeriod(winmm)。
- `StackTrace.h` + `win/DbgHelperWin.{h,cpp}`:去 dbghelp(SYMBOL_INFO),DbgHelperWin.cpp 从 PlatformWinUWP 排除。
- `WindowsExtras.h`:SHGetValueW / GetWindowLongPtr/SetWindowLongPtr → stub(无 shlwapi/HWND)。
- `win/ThreadingWin.cpp`:去 SEH `__try`(clang ARM32 不支持)。
- `win/SignalsWin.cpp`:去 AddVectoredExceptionHandler(VEH,App Container 无)。
- `win/MemoryPressureHandlerWin.cpp`:去 CreateMemoryResourceNotification/QueryMemoryResourceNotification。
- `PlatformWinUWP.cmake`:RunLoop/MainThread 用 **generic 版**(非 HWND 消息窗口);`generic/MainThreadGeneric.cpp` 补 Win32 线程 ID(原用 pthread)。
- `runtime/MathCommon.cpp`:`roundeven`/`roundevenf` Windows 走 WebKit 自带 fallback(UCRT 无 C23 函数)。
- `JavaScriptCore/PlatformWinUWP.cmake`:排除 JSStringRefBSTR(BSTR 需 oleauto,被 WIN32_LEAN_AND_MEAN 排除)。
- `wtf/simde/arm/neon.h`:`SIMDE_ARM64_BARRIER_SY` 按 `_M_ARM64` 分流。
- `CodePtr.h`/`FunctionPtr.h`/`FunctionTraits.h`:`SYSV_ABI` 变体守卫加 `&& CPU(X86_64)`(SYSV_ABI 仅 x86_64 才是不同调用约定)。

**最关键的两条**：① clang 的 thumbv7-windows-msvc 后端**无法 lower `cleanupret`**(Windows 异常展开)→ **必须关 C++ 异常**(WebKit 本就不用)。② 几乎所有坑的本质都是 **"WebKit/工具假设 Windows=x86_64-desktop"**,ARM32+App Container 上 x64/桌面专属的东西全崩,逐个换 App Container 允许的等价物。

---

## 6. 已知问题 / 坑

- **`tools\Wdp-Deploy.ps1` 有 CSRF 403 bug**：自动部署连上设备、上传后栽在 `CSRF Token Invalid`。
  规避：用 Device Portal 网页装。待修：CSRF token 流程（GET 拿 cookie → POST 带 `X-CSRF-Token`）对这台固件不对。
- **ARM32 VCLibs**：appx 依赖 `Microsoft.VCLibs.140.00 (ARM)`，本机无该 redist；设备多半已装(其他 -Reborn 应用带来)。
  若装报缺依赖，需另找 ARM32 VCLibs appx。
- **中文路径**：构建链对非 ASCII 路径敏感(Ruby/meson)，一切放 ASCII 路径(`E:\Apotheosis`、`C:\vcpkg`)。
- **harness 用 14.44 工具集**：VS 默认 14.51 无 arm；configure/build 脚本已固定 14.44。

---

## 7. Phase 1 计划与进度（WebCore + 渲染）

**目标**：ENABLE_WEBCORE，把静态 HTML 布局+绘制到离屏位图 → UWP WriteableBitmap 显示。

**架构决策（多智能体测绘得出，见记忆 `phase1-plan`）：**
- 图形后端 = **Cairo 软渲染**(`USE_SKIA=OFF`;Skia 要 GPU/gn,ARM32 UWP 不可行)。
- 字体 = **DirectWrite(系统,UWP 允许) + HarfBuzz 整形**,砍 fontconfig/freetype(最不 UWP);避开 GLib(用 generic)。
- 要造 `Source\WebCore\PlatformWinUWP.cmake`。
- 最小配置：关 media/webgl/wasm/webrtc/webxr/service-workers/indexeddb/... 一切非渲染必需。
- WebCore 18 个 App Container 雷区多数可 stub/排除（Uniscribe/Pasteboard/AX/全屏/MediaFoundation…）。

**依赖层（手动 clang-cl cross-build，配方见 §8）：**
```
pixman / zlib / libpng / expat / brotli / freetype  ✅ (vcpkg 早先编出)
cairo (image-only 软件后端)                          ✅ 已装 C:\vcpkg\installed\arm-uwp
harfbuzz (整形)                                      ⬜ 下一个,同配方
libxml2 (WebCore XML/HTML)                           ⬜
sqlite3 (存储)                                       ⬜
icu 78                                               ✅ (C:\icu-arm-uwp,Phase 0 已用)
```

**之后**：WebCore configure（最小配置）→ 数千文件的 clang-cl 编译-修复循环（同 §5 的套路，量 ~10×）→
造最小 "WebView"（HTML 字符串 → Page/Frame/FrameView → layout → paint 到 cairo image surface）→
位图 → WriteableBitmap → 复用 harness/部署链上真机。

**诚实预期**：Phase 1 是多周工程。但工具链/部署链/适配套路全部复用，无"未知死结"，只有"已知的大量活儿"。

---

## 8. Phase 1 依赖手动 cross-build 配方（cairo 已验证，reusable）

meson 依赖（cairo/harfbuzz/libxml2…）vcpkg 编不了（v141 太老无 C11）→ 手动 clang-cl 编：

1. 先 `vcpkg install <pkg>:arm-uwp --overlay-triplets=C:/vcpkg-overlay/triplets --allow-unsupported`（会失败），
   拿它生成的 cross-file：`C:\vcpkg\buildtrees\<pkg>\meson-arm-uwp-rel.log`。
2. python 改造 cross-file（见 `port\build-cairo.ps1` 旁的生成逻辑）：
   - 编译器 v141 cl → `clang-cl.exe --target=thumbv7-unknown-windows-msvc`
   - 链接器 → `lld-link.exe`；去 `-ZW:nostdlib`
   - 每个 `*_link_args` 末尾加 `WindowsApp.lib`
   - 含 `__WRL_NO_DEFAULT_LIB__` 处注入 `-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00`
3. meson setup：`PATH` 前置 `C:\Program Files\LLVM\bin`；在 `arm32-uwp-env.ps1` 里跑；
   禁 Linux/桌面后端；prefix=`C:\vcpkg\installed\arm-uwp`。
4. **patch `build.ninja`：`/MACHINE:thumbv7` → `/MACHINE:ARM`**（meson 推错，3 处）。
5. 源码 App Container patch（CreateFileW→CreateFile2，排除 GDI/桌面后端等，按编译错误逐个）。
6. `meson compile` + `meson install`。

---

## 9. 记忆索引（`~\.claude\projects\...\memory\`）

- `webkit-win10m-port.md` — 项目总纲 + Phase 0 达成
- `phase0-decisions.md` — Phase 0 全部技术决策与坑（最详细）
- `phase1-plan.md` — Phase 1 架构 + 依赖配方
- `user-altair.md` — 用户背景（逆过 QQ Win10M 客户端，负责执行+反馈）

---

*大月坠落，我们在月面上盖楼。Phase 0 心脏已跳，Phase 1 让它睁眼。* 🌑→🌕
