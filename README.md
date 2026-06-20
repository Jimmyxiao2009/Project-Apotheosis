# Project Apotheosis — EdgeHTML Reborn

将现代 **WebKit / WebCore** 移植到 **Windows 10 Mobile · ARM32 · UWP** 的浏览器引擎复活计划。
为已被微软放弃的 Windows Phone 生态重新带来一个能跑真实网页、带 JIT 与 GPU 合成的现代渲染引擎。

> Porting a modern **WebKit/WebCore** rendering engine to **Windows 10 Mobile (ARM32, UWP)** —
> bringing a real, JIT-accelerated, GPU-composited browser engine back to the abandoned Windows Phone platform.

## 现状 / Status

真机(Lumia, ARM32, Windows 10 Mobile 15254)已验证跑通:

- ✅ **WTF + JavaScriptCore** CLoop 真机运行(Phase 0)
- ✅ **WebCore 布局 + Cairo 软件渲染** —— Bing / GitHub / Apple / 微软 等真实站点正确渲染
- ✅ **常驻交互会话** —— 真实鼠标事件转发(点击 / 表单 / 链接导航)、滚动触发懒加载、屏幕键盘输入
- ✅ **JIT** —— 加 `codeGeneration` capability 后 App Container 放行可执行内存,JSC JIT 真机生效
- ✅ **GPU 合成** —— ANGLE (D3D11 FL9_3) + WebCore **TextureMapper**,合成直呈现到 `SwapChainPanel`
- ✅ **平滑滚动 / 捏合缩放(M3/M4)** —— 直呈现快滚 + 实时缩放变换 + 按新尺度重栅格
- 🚧 UI 向 Safari/Edge 形态演进中

## 架构 / Architecture

```
┌─────────────────────────────────────────────┐
│  Harness (C++/CX UWP App)                     │
│   · MainPage: 工具栏 / 地址栏 / 收藏历史下载   │
│   · 触摸手势 → 引擎滚动 / 点击 / 缩放          │
│   · GpuPanel (SwapChainPanel) ← GPU 直呈现     │
└───────────────┬─────────────────────────────┘
                │  C ABI (WebCoreDriver.h)
┌───────────────▼─────────────────────────────┐
│  WebCoreDriver (port/)                        │
│   · 常驻 Page 会话 / 真实事件派发 / 链接提取   │
│   · paintToRGBA: Cairo 软件 | TextureMapper GPU│
│   · PortChromeClient / FrameLoaderClient 等   │
└───────────────┬─────────────────────────────┘
                │
┌───────────────▼─────────────────────────────┐
│  WebKit / WebCore / JSC / WTF  (上游,另行向量化)│
│   · ARM32 移植补丁记录于工程笔记,未纳入本仓库  │
└─────────────────────────────────────────────┘
```

## 仓库内容 / What's tracked

本仓库只跟踪移植层与宿主,**不含** GB 级的上游 WebKit 源(向量化后另存)与可重新下载的 ANGLE 二进制:

- `port/` —— WebCore 驱动、Port 层客户端、各 stub、构建/链接脚本
- `harness/` —— UWP 宿主 App(C++/CX)、XAML UI、appx 清单
- `tools/` —— WDP(Device Portal)远程部署 / 抓崩溃 / 自动诊断脚本

## 构建 / Build (概要)

ARM32 UWP 工具链(clang-cl + lld-link)。引擎与依赖(Cairo/ICU/libcurl/freetype/harfbuzz/ANGLE…)分别预编译为 ARM `.lib`,再:

```powershell
# 1. 编 + 链接 WebCore 驱动 → WebCoreDriver-gpu.lib
pwsh -File port\link-driver-gpu.ps1
# 2. 构建宿主 appx
pwsh -File port\build-harness.ps1
# 3. 经 Device Portal 部署到真机并启动
pwsh -File tools\deploy-launch.ps1 -Ver <版本号>
```

## 平台 / Target

Lumia 系列等 ARM32 Windows 10 Mobile 设备(`Windows.Universal` MinVersion 10.0.15063)。

---

*Reviving the Windows Phone web for the people who never let it die.* 📱
