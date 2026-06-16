#pragma once
#include <string>
// 可执行内存可行性探针(JIT 前提)。返回人读报告(UTF-8)。纯 C++ + SEH,单独编译(非 /ZW)。
std::string RunJitProbe();
