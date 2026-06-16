#pragma once
// GpuProbe — GPU 路径1 可行性探针。在给定 SwapChainPanel 上用 ANGLE(EGL/GL ES 2.0 over D3D11 FL9_3)
// 初始化并画一个动画三角形,验证 GPU 管线在 Win10M ARM32 App Container 里能通(对标 JitProbe 验可执行内存)。
// 在后台线程跑(EGL 全程一个线程);每步结果 + GL_VENDOR/RENDERER/VERSION + eglGetError 写 LocalState\gpuprobe.txt。
namespace Harness {
    void RunGpuProbe(Windows::UI::Xaml::Controls::SwapChainPanel^ panel, Platform::String^ localStateDir);
}
