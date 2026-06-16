// GpuProbe.cpp — 见 GpuProbe.h。用老 Microsoft ANGLE.WindowsStore(ms-master)的 PropertySet 范式:
// SwapChainPanel 经 PropertySet(EGLNativeWindowTypeProperty)交给 eglCreateWindowSurface;
// D3D11 后端按 FL9_3 初始化(MAX_VERSION 9.3 = D3D feature level,覆盖 1020 等老设备下限)。
// CompileAsWinRT=true(用到 PropertySet/SwapChainPanel),但不挂 PCH(独立编译)。
#include <windows.h>
#include <inspectable.h>
#include <agile.h>            // Platform::Agile<>
#include <cstdio>
#include <string>
#include <thread>
#include <fstream>
// 这版 ms-master ANGLE 的 gl2.h 把核心 GL 函数原型放在 GL_GLEXT_PROTOTYPES 之下(否则 C3861 找不到 glClear 等)。
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include "GpuProbe.h"

using namespace Windows::UI::Xaml::Controls;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static GLuint CompileShader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

void Harness::RunGpuProbe(SwapChainPanel^ panel, Platform::String^ localStateDir)
{
    Platform::Agile<SwapChainPanel^> agile(panel);
    std::wstring dir = localStateDir ? std::wstring(localStateDir->Data()) : L"";

    std::thread([agile, dir]() {
        std::string log;
        char buf[160];
        auto step = [&](const char* m) { log += m; log += "\n"; };
        auto write = [&]() {
            if (dir.empty()) return;
            std::ofstream f(Narrow(dir) + "\\gpuprobe.txt", std::ios::binary | std::ios::trunc);
            if (f) f.write(log.data(), log.size());
        };

        step("=== GPU 探针:ANGLE(D3D11 FL9_3)+ SwapChainPanel + GL ES2 画三角形 ===");

        auto getPlatDisp = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (!getPlatDisp) { step("FAIL: eglGetPlatformDisplayEXT 未找到(libEGL 没加载?)"); write(); return; }

        // MAX_VERSION 9.3 = 请求 D3D11 feature level 9_3(老 ms-master ANGLE 语义),验老设备下限。
        const EGLint dattr[] = {
            EGL_PLATFORM_ANGLE_TYPE_ANGLE,              EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
            EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE, 9,
            EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE, 3,
            EGL_NONE
        };
        EGLDisplay dpy = getPlatDisp(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, dattr);
        if (dpy == EGL_NO_DISPLAY) { step("FAIL: eglGetPlatformDisplayEXT -> NO_DISPLAY"); write(); return; }
        if (!eglInitialize(dpy, nullptr, nullptr)) { sprintf_s(buf, "FAIL: eglInitialize err=0x%x", eglGetError()); step(buf); write(); return; }
        step("OK: eglInitialize(D3D11 FL9_3)");

        const EGLint cattr[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 16, EGL_STENCIL_SIZE, 8,
            EGL_NONE
        };
        EGLConfig cfg; EGLint nc = 0;
        if (!eglChooseConfig(dpy, cattr, &cfg, 1, &nc) || nc < 1) { step("FAIL: eglChooseConfig(无 ES2/RGBA8/D16S8 config)"); write(); return; }
        step("OK: eglChooseConfig");

        SwapChainPanel^ p = agile.Get();
        if (!p) { step("FAIL: SwapChainPanel agile 为空"); write(); return; }
        // ms-master ANGLE.WindowsStore:native window = PropertySet(含 SwapChainPanel + 固定渲染尺寸)。
        PropertySet^ props = ref new PropertySet();
        props->Insert(L"EGLNativeWindowTypeProperty", p);
        props->Insert(L"EGLRenderSurfaceSizeProperty", PropertyValue::CreateSize(Size(720, 1080)));
        EGLNativeWindowType win = reinterpret_cast<EGLNativeWindowType>(reinterpret_cast<IInspectable*>(props));
        EGLSurface surf = eglCreateWindowSurface(dpy, cfg, win, nullptr);
        if (surf == EGL_NO_SURFACE) { sprintf_s(buf, "FAIL: eglCreateWindowSurface err=0x%x", eglGetError()); step(buf); write(); return; }
        step("OK: eglCreateWindowSurface(SwapChainPanel)");

        const EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
        if (ctx == EGL_NO_CONTEXT) { sprintf_s(buf, "FAIL: eglCreateContext err=0x%x", eglGetError()); step(buf); write(); return; }
        if (!eglMakeCurrent(dpy, surf, surf, ctx)) { sprintf_s(buf, "FAIL: eglMakeCurrent err=0x%x", eglGetError()); step(buf); write(); return; }
        step("OK: GL ES2 上下文 current");

        const char* vendor   = (const char*)glGetString(GL_VENDOR);
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        const char* version  = (const char*)glGetString(GL_VERSION);
        sprintf_s(buf, "GL_VENDOR=%s",   vendor   ? vendor   : "?"); step(buf);
        sprintf_s(buf, "GL_RENDERER=%s", renderer ? renderer : "?"); step(buf);   // ★ D3D11 适配器名:证明真硬件
        sprintf_s(buf, "GL_VERSION=%s",  version  ? version  : "?"); step(buf);

        GLuint vs = CompileShader(GL_VERTEX_SHADER,   "attribute vec2 p;void main(){gl_Position=vec4(p,0.0,1.0);}");
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, "precision mediump float;uniform float t;void main(){gl_FragColor=vec4(1.0,0.55+0.45*t,0.10,1.0);}");
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs); glAttachShader(prog, fs);
        glBindAttribLocation(prog, 0, "p");
        glLinkProgram(prog);
        GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) { step("FAIL: shader program link 失败"); write(); return; }
        glUseProgram(prog);
        GLint tloc = glGetUniformLocation(prog, "t");

        const GLfloat tri[] = { 0.0f, 0.8f,  -0.8f, -0.8f,  0.8f, -0.8f };
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
        glViewport(0, 0, 720, 1080);

        // ~48 帧动画(三角形从橙到黄渐变),证明持续 GPU 渲染 + 交换链 Present 正常。
        for (int i = 0; i < 48; ++i) {
            float t = (float)i / 48.0f;
            glClearColor(0.05f, 0.12f, 0.22f, 1.0f);   // 深蓝底
            glClear(GL_COLOR_BUFFER_BIT);
            glUniform1f(tloc, t);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            eglSwapBuffers(dpy, surf);
        }
        GLenum gl = glGetError();
        sprintf_s(buf, "SUCCESS: 三角形已画 48 帧,glError=0x%x", gl); step(buf);
        step("★ 若设备上看到深蓝底 + 橙黄三角形 = GPU 管线在 App Container 通!");
        write();
        // 保留上下文/最后一帧(三角形留在屏上)。探针进程退出由 app 生命周期管。
    }).detach();
}
