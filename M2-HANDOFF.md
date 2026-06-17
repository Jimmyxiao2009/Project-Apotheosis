# M2 交接提示词 —— GPU 呈现(TextureMapper → SwapChainPanel)

> 把这份连同仓库一起给新会话即可接手 M2。背景路线在记忆 `native-feel-0_1_7-plan.md`(M0-M6),
> GPU 全貌在 `gpu-acceleration.md`,交互/会话在 `live-session.md`。分支 **gpu-path1**,引擎 **build-clang-gpu**。

## 0. 现在在哪(M0 ✅ / M1 ✅)
- **M0 通过**(真机):ScrollViewer 包 SwapChainPanel,系统原生 DirectManipulation 能丝滑平移+捏合 ANGLE 渲染的三角形 → **原生平移/缩放架构成立**。代码即 0.1.5/0.1.7.0 的 GpuProbe(`harness/GpuProbe.cpp`,用预编译 NuGet ANGLE 的 PropertySet 范式在 SwapChainPanel 上建 EGL 表面、画 GL ES2)。
- **M1 完成**(0.1.7.1,提交 5968505):
  - `port/PortChromeClient.{h,cpp}` = 真 ChromeClient 子类(EmptyChromeClient 合成钩子 final 不能覆写),`attachRootGraphicsLayer` 捕获**根 GraphicsLayer**,`triggerRenderingUpdate/setNeedsOneShotDrawingSynchronization` 置 `m_needsPresent`,`allowedCompositingTriggers()=AllTriggers`。公开 `rootLayer()` / `takeNeedsPresent()`。
  - `WebCoreDriver.cpp buildSession`:`makeUniqueRefWithoutRefCountedCheck<PortChromeClient>()` → 裸指针存 `g_session->chrome` → move 进 `pageConfiguration.chromeClient`;`setAcceleratedCompositingEnabled(true)+setForceCompositingMode(true)` → **图层树已建**。
  - `paintToRGBA` 已设 `PaintBehavior::FlattenCompositingLayers|Snapshotting`(开合成后软件 paint 仍出完整页面,不回归)。
  - `WebCoreEnableCompositing()` 返回根图层是否已附;harness 在 after-load 写 `stage.txt` 的 `compositing=<n>`。
  - **harness 已链 build-clang-gpu + WebCoreDriver-gpu.lib**(`Harness.vcxproj` 的 AdditionalDependencies/LibraryDirectories 已切)。`link-driver-gpu.ps1` 产出 `WebCoreDriver-gpu.lib`(12 obj,0 未定义,含 ANGLE 导入库)。
  - **仍走软件 paintToRGBA 呈现**(贴 WriteableBitmap 到 RenderImage)。GPU 呈现 = M2。

## 1. M2 目标
把呈现从"软件 paintToRGBA→WriteableBitmap"换成"**TextureMapper 把图层树合成进 ANGLE swapchain,直接显示在 SwapChainPanel**"。完成后页面真正跑在 GPU 上(M3/M4 的丝滑滚动/真缩放才有载体)。**先离屏验证、再直呈现**(降风险)。

## 2. 要加的驱动导出(port/WebCoreDriver.cpp + 两个 WebCoreDriver.h 同步)
- `int WebCoreGpuInit(void* swapChainPanelInspectable, int w, int h)` — **引擎线程**调一次:从 SwapChainPanel 建 EGL display/context/window-surface,make current 并**永驻本线程**。返回 0 成功。镜像 `GpuProbe.cpp` 的 EGL 起法(`eglGetPlatformDisplayEXT(D3D11, MAX_VERSION 9.3)` → `eglChooseConfig(ES2,RGBA8,**D24S8 或 D16S8**)` → `PropertySet{EGLNativeWindowTypeProperty=panel, EGLRenderSurfaceSizeProperty=Size(w,h)}` 当 native window → `eglCreateWindowSurface` → `eglCreateContext(ES2)` → `eglMakeCurrent`)。**关键**:TextureMapper 的模板裁剪要 stencil,config 必须含 `EGL_STENCIL_SIZE>=8`。panel 由 harness 以 `IInspectable*`(`Platform::Agile<SwapChainPanel^>` 跨线程)传入。
- `int WebCoreComposite()` — **引擎线程**每次呈现调:跑 TextureMapper recipe(见 §3)把 `g_session->chrome->rootLayer()` 的图层树合成进 swapchain + `eglSwapBuffers`。返回 0。**替代** GPU 路径下的 paintToRGBA。
- (可选离屏验证)`int WebCoreCompositeReadback(uint8_t* outRGBA)` — 同 recipe 但 `beginPainting` 渲到离屏 `BitmapTexture`(`BitmapTexturePool`)后 `glReadPixels(GL_RGBA/GL_BGRA)` 回 outRGBA。用现有 WriteableBitmap 通道显示 → **先证 TextureMapper 合成对,再切 SwapChainPanel 直呈现**(WCScene.cpp 的 `m_usesOffscreenRendering` 分支即此)。

## 3. TextureMapper recipe(每帧,镜像 `Source/WebKit/GPUProcess/graphics/wc/WCScene.cpp` 的 `WCScene::update`)
```
context->makeContextCurrent();                              // GLContext / eglMakeCurrent
// 1. 把合成器待提交变更刷进 TextureMapperLayer 树:
//    经 FrameView::flushCompositingStateIncludingSubframes 或
//    RenderLayerCompositor::flushPendingLayerChanges(true)（RLC.cpp:831）
//    → GraphicsLayerTextureMapper::flushCompositingState（GLTM.cpp:542,递归 commitLayerChanges）
// 2. 上传脏 tile 内容到 GL 纹理:
rootGLTM->updateBackingStoreIncludingSubLayers(*textureMapper);   // GLTM.cpp:560
// 3. 推进动画:
rootTMLayer->applyAnimationsRecursively(MonotonicTime::now());    // WCScene.cpp:254
// 4. 合成到默认帧缓冲(swapchain):
glViewport(0,0,w,h);
textureMapper->beginPainting(TextureMapper::FlipY::No, /*surface*/nullptr);  // 绑默认 FBO
rootTMLayer->paint(*textureMapper);                                          // TextureMapperLayer.h:117
textureMapper->endPainting();
context->swapBuffers();                                                      // eglSwapBuffers
```
- **根**:`g_session->chrome->rootLayer()` 是 `WebCore::GraphicsLayer*`。它实为 `GraphicsLayerTextureMapper`(同步路径,USE_GRAPHICS_LAYER_TEXTURE_MAPPER=ON)。取其 `TextureMapperLayer`:`static_cast<GraphicsLayerTextureMapper*>(root)`,其 `.layer()` / 内部 `m_layer`(看 `GraphicsLayerTextureMapper.h:159`)给 `rootTMLayer`。
- **TextureMapper 构造**:`TextureMapper::create()`——**必须先有 GLContext current**(WCScene.cpp:78-82 明确注释)。一个 TextureMapper / GL context,长存。GpuInit 里建。
- **GL context**:WebCore 的 `PlatformDisplay::sharedDisplay()`(→`PlatformDisplayWin`,EGL 经链入的预编译 ANGLE)+ `GLContext::create(display, nativeWindow)`(`egl/GLContext.cpp:133 createWindowContext`,fallthrough `eglCreateWindowSurface`)。或直接像 GpuProbe 裸 EGL 起、把 EGLContext 喂给 TextureMapper。两条都用**同一份预编译 ANGLE**(GPU WebCore.lib 链的 ANGLE::EGL/GLES 就是它),一致。
- 关键文件:`OptionsWin.cmake:122-131`,`texmap/TextureMapper.{h,cpp}`(`.cpp:210-271`),`texmap/GraphicsLayerTextureMapper.{h,cpp}`(`.cpp:425-570`),`texmap/TextureMapperLayer.h:112,117`,`texmap/BitmapTexture.cpp:61,296-335`,`platform/graphics/win/PlatformDisplayWin.cpp:35-52`,`platform/graphics/egl/GLContext.cpp:88,133-180,359-374`,`rendering/RenderLayerCompositor.cpp:831,3152,5248`。

## 4. harness 侧(MainPage.xaml/.cpp)
- 显示面切到 GPU:把 `GpuPanel`(SwapChainPanel,现 `Visibility=Collapsed`)设可见、覆盖 `RenderImage`;尺寸 720×1080(=kW/kH)。**先**仍可保留 RenderImage 作软件回退(运行时 `g_useGpu` 切)。
- 启动时(GpuPanel Loaded / 首次会话):post 引擎任务 `WebCoreGpuInit(reinterpret_cast<IInspectable*>(GpuPanel), kW, kH)`;用 `Platform::Agile<SwapChainPanel^>` 跨线程把 panel 传到引擎线程。
- 把各呈现点(NavigateTo/ApplyEngineFrame/PumpScroll/SendKeyToEngine/OnLiveTick 现在调 `WebCoreSessionLoad`/`*Paint` 后 `BlitToBitmap`)在 GPU 路改为引擎线程调 `WebCoreComposite()`(无需再 marshal 像素回 UI、无需 WriteableBitmap)。**离屏验证阶段**:仍 `WebCoreCompositeReadback(rgba)`+BlitToBitmap,先证合成对。
- **线程铁律**:present 只在引擎线程;**绝不**让 UI 线程同步 wait 引擎(ANGLE 把 surface create/resize marshal 回 panel dispatcher,UI↔引擎互等=死锁,`RunOnUIThread` 10s 超时会 `std::terminate`)。现有 post()/RunAsync 单向异步即安全,保持。

## 5. 构建 / 测试 / 部署命令
```powershell
# 改驱动后重链 GPU 驱动(产出 WebCoreDriver-gpu.lib):
pwsh -File E:\Apotheosis\port\link-driver-gpu.ps1
# 单文件编译验证(快):
pwsh -File E:\Apotheosis\port\compile-driver-gpu.ps1 E:\Apotheosis\port\WebCoreDriver.cpp E:\Apotheosis\port\WebCoreDriver.gpu.obj
# 构建 harness(已链 build-clang-gpu):
pwsh -File E:\Apotheosis\port\build-harness.ps1   # 看 harness-build.log;appx 在 harness\AppPackages\...\_Test\
# 量 appx 大小用 PowerShell .Length(别用 ls -la,Windows 属主名带空格会读错)。
# 部署/拉起/拉日志:tools\Deploy-Robust.ps1 -Ip 192.168.3.51;WDP REST 见 wdp-debug-toolchain 记忆。
# 设备常不在(校园网/息晚掉 WiFi)——脱离式守护模式见 tools\Verify-*-Detached.ps1。
```
- 版本号:Package.appxmanifest 升 0.1.7.2。提交到 gpu-path1。

## 6. M2 验收 + 风险
- **验收**(真机):加载一个页面,看见它**合成在 GPU 面上**(取代 Image);▲▼ FAB 仍能滚(M3 才接手势)。先离屏 readback 验"合成出的像素 == 软件 paint";再切直呈现。
- **风险**:① ANGLE↔UI 线程 marshal 死锁(present 只在引擎线程、绝不同步等 UI);② 首帧前图层树/TextureMapperLayer 可能空(GpuInit 与首次 flush 的时序;rootLayer() 在加载/合成更新后才非空——M1 的 compositing=1 即此)；③ stencil(config 要 `EGL_STENCIL_SIZE>=8`,否则圆角裁剪/clip 失效或崩);④ tile 内存(长页 `TextureMapperTiledBackingStore`,别开 ENABLE_ASYNC_SCROLLING);⑤ flushCompositingState 的入口在 2.52.4 的确切签名要去 RenderLayerCompositor/LocalFrameView 源码核（`flushPendingLayerChanges` 还是 `flushCompositingStateForThisFrame`），编译报错即对照修;⑥ `port/` 与 `harness/` 两份 WebCoreDriver.h 新导出要同步。
- M2 后:**M3** 丝滑滚动+修懒加载(`WebCoreSetScrollPosition` 绝对滚→updateLayout→isolatedUpdateRendering 触发 IntersectionObserver)、**M4** 真捏合缩放(`WebCoreCommitPageScale` settle-only)、**M5** 文本选择。详见 `native-feel-0_1_7-plan.md`。

## 7. 一句话起手
> 在 gpu-path1 上,给 WebCoreDriver.cpp 加 `WebCoreGpuInit`(镜像 GpuProbe.cpp 的 EGL+PropertySet 起法,config 带 stencil)和 `WebCoreComposite`(镜像 WCScene::update 的 recipe,根取 `g_session->chrome->rootLayer()` 转 GraphicsLayerTextureMapper),先做 `WebCoreCompositeReadback` 用现有 WriteableBitmap 通道离屏验证合成正确,再把 GpuPanel 设可见、present 切到引擎线程直呈现;`link-driver-gpu` + `build-harness` 出 0.1.7.2,真机验"页面合成在 GPU 面上"。
