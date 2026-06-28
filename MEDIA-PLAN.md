# MEDIA-PLAN.md — HTML5 `<video>`/`<audio>` 播放(ARM32 UWP App Container)

> 状态:**未开始**。这是"考完试再做"的大里程碑落地方案。
> 量级:与当初 GPU 合成线相当——多轮真机迭代,不是填桩级的活。

## 0. 现状(为什么现在不能播)

- `port/draft-OptionsWinUWP-webcore-block.cmake`:`ENABLE_VIDEO / ENABLE_WEB_AUDIO / ENABLE_MEDIA_SOURCE / ENABLE_MEDIA_STREAM` 全 **OFF**;`USE_MEDIA_FOUNDATION` 强制 OFF。
- 所以 `<video>`/`<audio>` 没有任何 `MediaPlayerPrivate` 后端,DOM 元素是死的(不播放、无控件)。
- **关键坑**:WebKit 自带的 Windows 媒体后端 `MediaPlayerPrivateMediaFoundation` 用的是**桌面 MF**——`EVR`(增强视频渲染器)+ `d3d9` + `dxva2` + `evr/mf/mfplat/strmiids`。这些在 **UWP App Container 里被禁**(`port/draft-PlatformWinUWP.cmake` 注释里因此把整个 MediaFoundation INTERFACE 库 DROP 了)。
- **结论:不能简单"打开开关 + 加回源码块"。必须写一个 App-Container 兼容的新后端。**

## 1. 目标(v1 范围)

- **v1**:渐进式 `<video src="...mp4/.m4a">` / `<audio src>` 播放(H.264/AAC,设备有硬解)。play/pause/seek/音量/时间/缓冲/结束事件。
- **不在 v1**:MSE(MediaSource,YouTube/B 站等自适应流要它,是另一大块)、EME/DRM、WebRTC/MediaStream(摄像头麦克风)、字幕轨。

## 2. 技术路线

写一个新的 `MediaPlayerPrivateInterface` 子类(`MediaPlayerPrivateWinUWP`),经 `MediaPlayerFactory` 注册,**走 App-Container 允许的媒体 API**。两个候选解码/播放引擎:

- **方案 A(推荐起步):WinRT `Windows.Media.Playback.MediaPlayer` + 帧服务器模式**
  - `MediaPlayer.IsVideoFrameServerEnabled = true` + `VideoFrameAvailable` 事件 → `CopyFrameToVideoSurface(IDirect3DSurface)` 把当前帧拷进一个 D3D11 纹理。
  - 音频:MediaPlayer 自己出声(不需要 Web Audio)。
  - 加载/控制:`MediaPlayer.Source = MediaSource::CreateFromUri(uri)`;play/pause/`PlaybackSession.Position`/`NaturalDuration`/`BufferingProgress`/`PlaybackState`。
  - **优点**:WinRT、App Container 安全、硬解、控制面全;**难点**:帧 D3D11 纹理 → 喂进 WebKit 合成层。
- **方案 B(更底层):`IMFMediaEngine`(MF 的 UWP 友好引擎)+ DXGI swapchain / `OnVideoStreamTick` 回调** → 同样拿到 D3D 表面。控制更细但更繁。

**先做方案 A。**

## 3. 帧 → WebKit 合成器(最硬的一块)

WebKit 侧 `MediaPlayerPrivate::paint()` / 平台层 `nativeImageForCurrentTime` 要给出当前帧。两条对接路:

1. **GPU 路(配合现有 TextureMapper+ANGLE)**:把 MediaPlayer 拷出的 D3D11 纹理,经 ANGLE 的 `EGL_ANGLE_d3d_texture_client_buffer` 包成 EGLImage/GL 纹理 → 作为视频层贴进 TextureMapper 合成(类似现在 GPU 直呈现的层)。**首选**(已有 D3D11/ANGLE 管线)。
2. **软件路回退**:`CopyFrameToVideoSurface` 到一个 CPU 可读 surface → readback RGBA → 走 Cairo `paintToRGBA` 那条(慢,但能先验证管线通)。先用它打通,再上 GPU 路。

注意线程:MediaPlayer 事件在 UI/ASTA 线程;WebCore `MediaPlayer` 在引擎线程。状态变化要 marshal 回引擎线程(经现有 `WebEngine::instance().post()` 模式),**严禁 UI 同步 wait 引擎**(见 CLAUDE.md 线程铁律)。

## 4. 落地步骤

1. **配置**:`ENABLE_VIDEO ON`(`ENABLE_WEB_AUDIO` 可暂留 OFF,`<video>` 声音走 MediaPlayer);`USE_MEDIA_FOUNDATION` **保持 OFF**(不要桌面后端)。重配引擎 `configure-gpu.ps1`。
2. **新后端**:`MediaPlayerPrivateWinUWP.{h,cpp}`(放 port/ 或注入 WebCore 源),实现 `MediaPlayerPrivateInterface`:load/cancelLoad/play/pause/seek/duration/currentTime/paused/seeking/networkState/readyState/setVolume/muted/naturalSize/hasVideo/hasAudio/`paint`/`nativeImageForCurrentTime`/`platformLayer`(GPU 层)。`registerMediaEngine`。
3. **WinRT 桥**:因引擎是 clang-cl(无 C++/CX),用 **WRL(`Microsoft::WRL`)+ ABI 头**或经 C ABI 把 MediaPlayer 的活交给 harness(C++/CX)做、引擎侧只持句柄 + 收帧/状态回调。**倾向:harness 侧 C++/CX 管 MediaPlayer,新增 C ABI 让引擎 创建/控制/取帧**(参照现有 GPU panel 经 C ABI 的做法)。
4. **帧对接**:先软件 readback 打通(看到画面动)→ 再切 ANGLE D3D11 纹理共享上 GPU 层。
5. **控件**:WebKit 自带 media-controls(JS/CSS 资源,cmake 里 `media-controls` DESTINATION 那块)——确认是否随包;不行就先用原生控件或最简自绘。
6. **真机迭代**:崩溃用 `Wdp-Crash.ps1` 抓;阶段埋点同 OOBE 调试那套。

## 5. 风险清单

- App Container 里 MediaPlayer 帧服务器 / D3D 互操作的实际可用性(要真机试探,可能要 `ID3D11Device` 多线程保护)。
- D3D11 纹理跨(MediaPlayer 的 device)↔(ANGLE 的 device)共享 → 多半要 `IDXGIResource` 共享句柄或同一 device。
- 线程/生命周期(导航离开页面要干净 teardown MediaPlayer,别像早期 GPU 那样无 dump 闪退)。
- 仅渐进下载;**MSE 不做 → 大量视频站(YouTube 等)仍不能播**,要管理预期。

## 6. 提交/分支建议

在 `gpu-path1` 上开,或拉 `media-path1` 子线;每步真机验过再 squash。引擎改动加 `#if defined(WK_WINUWP)` + `Apotheosis:` 注释守卫(上游守则)。
