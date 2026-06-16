// JitProbe.cpp — 验证 App Container 能否申请并执行运行期生成的代码(JIT 的前提)。
// 纯 C++ + SEH;必须以 CompileAsWinRT=false 单独编译(C++/CX /ZW 下禁用 __try/__except)。
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

// Thumb-2: movs r0, #42 ; bx lr  →  调用应返回 42
static const unsigned char kCode[] = { 0x2A, 0x20, 0x70, 0x47 };

// SEH 包住实际调用:执行被 DEP/ACG 拦会抛访问违例,这里捕获而非崩溃。
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
    std::memcpy(mem, kCode, sizeof(kCode));   // 需可写(RW 或 RWX)
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

static std::string line(const char* name, const TR& r)
{
    char buf[400];
    if (!r.allocOk)
        std::snprintf(buf, sizeof buf, "%s: 分配失败 err=%lu\n", name, r.allocErr);
    else if (!r.protectOk)
        std::snprintf(buf, sizeof buf, "%s: 改可执行被拒 err=%lu  (此路不能JIT)\n", name, r.protectErr);
    else if (r.crashed)
        std::snprintf(buf, sizeof buf, "%s: 保护OK但执行崩溃(DEP/ACG拦)  (此路不能JIT)\n", name);
    else if (r.called && r.ret == 42)
        std::snprintf(buf, sizeof buf, "%s: 成功 返回%d  ★可以JIT!\n", name, r.ret);
    else
        std::snprintf(buf, sizeof buf, "%s: 调用返回异常值%d(预期42)\n", name, r.ret);
    return buf;
}

std::string RunJitProbe()
{
    std::string s = "=== JIT 可执行内存测试(Thumb-2 movs r0,#42; bx lr 应返回42)===\n";
    s += line("[A] RW->改RX (W^X, 现代JSC用法)", runScenario(PAGE_READWRITE, true, PAGE_EXECUTE_READ));
    s += line("[B] 直接分配RWX", runScenario(PAGE_EXECUTE_READWRITE, false, 0));
    s += line("[C] RW->改RWX", runScenario(PAGE_READWRITE, true, PAGE_EXECUTE_READWRITE));
    s += "判读:任一 ★ = JIT可行(A最关键);全失败/崩溃 = JIT不通,退 asm LLInt。\n";
    return s;
}
