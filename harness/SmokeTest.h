// SmokeTest.h  —  Phase 0 验证: 用 JSC C API 执行 "1+1", 返回结果字符串。
// 只依赖 JavaScriptCore 的 C API(JavaScript.h, 纯 C), 与 clang-cl 编的 .lib 链接兼容。
#pragma once
#include <string>

// 运行时设 JSC 单线程 GC(规避 App Container 下 SuspendThread/GetThreadContext)。
// 返回如 "1+1 = 2" 的结果, 或出错信息。
std::wstring RunJscSmokeTest();
