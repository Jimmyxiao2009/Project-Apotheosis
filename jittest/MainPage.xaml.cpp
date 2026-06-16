#include "pch.h"
#include "MainPage.xaml.h"
#include "MainPage.g.hpp"
#include <windows.h>
#include <fstream>

using namespace JITTest;
using namespace Platform;
using namespace Windows::UI::Xaml;

// Thumb-2: movs r0, #42 ; bx lr   →  应返回 42
static const unsigned char kCode[] = { 0x2A, 0x20, 0x70, 0x47 };

// 单独函数做 SEH __try/__except(不含 C++ 对象,避免 C2712)。执行被 DEP/ACG 拦会抛访问违例,这里捕获。
static bool tryCallThumb(void* mem, int* out)
{
    typedef int (*Fn)();
    Fn f = reinterpret_cast<Fn>(reinterpret_cast<uintptr_t>(mem) | 1u);   // ARM Thumb 位
    __try {
        *out = f();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct TR {
    bool allocOk = false; unsigned long allocErr = 0;
    bool protectOk = false; unsigned long protectErr = 0;
    bool called = false; bool crashed = false; int ret = 0;
};

static TR runScenario(unsigned long allocProt, bool doReprotect, unsigned long reprotectFlag)
{
    TR r;
    SIZE_T sz = 4096;
    void* mem = VirtualAllocFromApp(nullptr, sz, MEM_COMMIT | MEM_RESERVE, allocProt);
    if (!mem) { r.allocErr = GetLastError(); return r; }
    r.allocOk = true;
    std::memcpy(mem, kCode, sizeof(kCode));   // 此时必须可写(RW 或 RWX)
    if (doReprotect) {
        ULONG old = 0;
        if (!VirtualProtectFromApp(mem, sz, reprotectFlag, &old)) {
            r.protectErr = GetLastError();
            VirtualFree(mem, 0, MEM_RELEASE);
            return r;
        }
    }
    r.protectOk = true;
    FlushInstructionCache(GetCurrentProcess(), mem, sz);
    int out = 0;
    if (tryCallThumb(mem, &out)) { r.called = true; r.ret = out; }
    else { r.crashed = true; }
    VirtualFree(mem, 0, MEM_RELEASE);
    return r;
}

static std::wstring fmt(const wchar_t* name, const TR& r)
{
    wchar_t buf[600];
    if (!r.allocOk)
        swprintf_s(buf, L"%s\n  分配失败 err=%lu\n\n", name, r.allocErr);
    else if (!r.protectOk)
        swprintf_s(buf, L"%s\n  分配OK，改可执行被拒 err=%lu  ← 此路不能 JIT\n\n", name, r.protectErr);
    else if (r.crashed)
        swprintf_s(buf, L"%s\n  分配OK 保护OK，但执行崩溃(DEP/ACG 拦执行)  ← 此路不能 JIT\n\n", name);
    else if (r.called && r.ret == 42)
        swprintf_s(buf, L"%s\n  ✅ 成功!分配→写码→可执行→调用 返回 %d  ← 可以 JIT!\n\n", name, r.ret);
    else
        swprintf_s(buf, L"%s\n  调用返回异常值 %d(预期 42)\n\n", name, r.ret);
    return buf;
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

MainPage::MainPage()
{
    InitializeComponent();
    RunTests();
}

void MainPage::RunTests()
{
    std::wstring log;
    log += L"目标:App Container 能否申请并执行运行期生成的代码(JIT 前提)。\n";
    log += L"测试码:Thumb-2 movs r0,#42; bx lr(应返回 42)。\n\n";

    log += fmt(L"[A] 分配RW→写→改RX(W^X,现代 JSC 用法)", runScenario(PAGE_READWRITE, true, PAGE_EXECUTE_READ));
    log += fmt(L"[B] 直接分配 RWX(PAGE_EXECUTE_READWRITE)", runScenario(PAGE_EXECUTE_READWRITE, false, 0));
    log += fmt(L"[C] 分配RW→写→改RWX", runScenario(PAGE_READWRITE, true, PAGE_EXECUTE_READWRITE));

    log += L"判读:任一 ✅ = JIT 可行(A 最关键)。全失败/崩溃 = JIT 不通,退 asm LLInt。\n";

    ResultText->Text = ref new String(log.c_str());

    // 写 LocalState\jitresult.txt(UTF-8)供 WDP 拉取
    try {
        auto local = Windows::Storage::ApplicationData::Current->LocalFolder->Path;
        std::wstring path = std::wstring(local->Data()) + L"\\jitresult.txt";
        std::ofstream f(WideToUtf8(path), std::ios::binary | std::ios::trunc);
        if (f) { std::string u = WideToUtf8(log); f.write(u.data(), u.size()); }
    } catch (...) {}
}
