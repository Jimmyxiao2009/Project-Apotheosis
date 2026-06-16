# ============================================================================
# Toolchain-ARM32-UWP-clang.cmake  —  Path B: clang-cl 瞄准 ARM32 UWP
# 目标: ARM32 (Thumb-2) + Windows 10 Mobile UWP / App Container
# 编译器: LLVM clang-cl(modern WebKit 的官方 Windows 编译器), 用 MSVC 14.44 的
#         ARM 头/库 + Win10 SDK(经 arm32-uwp-env.ps1 设的 INCLUDE/LIB)。
# 关键: clang-cl 原生提供 __SIZEOF_POINTER__/__BYTE_ORDER__/__attribute__ 等,
#       故这里"不"补 GCC 宏(那是 Path A 纯 MSVC 才需要的)。
# ============================================================================

set(CMAKE_SYSTEM_NAME       WindowsStore)
set(CMAKE_SYSTEM_VERSION    10.0)
set(CMAKE_SYSTEM_PROCESSOR  ARM)
set(CMAKE_CROSSCOMPILING    TRUE)

set(_CLANG "C:/Program Files/LLVM/bin/clang-cl.exe")
set(CMAKE_C_COMPILER   "${_CLANG}")
set(CMAKE_CXX_COMPILER "${_CLANG}")

# 瞄准 ARM32 Windows(Thumb-2)。CMake 会把它转成 clang 的 --target=。
set(CMAKE_C_COMPILER_TARGET   "thumbv7-unknown-windows-msvc")
set(CMAKE_CXX_COMPILER_TARGET "thumbv7-unknown-windows-msvc")

# 让 CMake 接受交叉编译的编译器(不尝试运行产物)
set(CMAKE_C_COMPILER_WORKS   TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)

# --- App Container 必备宏 ---
add_compile_definitions(
    WINAPI_FAMILY=WINAPI_FAMILY_APP
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _UNICODE UNICODE
    _WIN32_WINNT=0x0A00
    __WRL_NO_DEFAULT_LIB__
    WK_WINUWP=1        # 本 port 自有标记: 守卫桌面专用 API(dbghelp/winmm 等)
)

# --- 只链 UWP 伞库 ---
set(CMAKE_C_STANDARD_LIBRARIES   "WindowsApp.lib" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "WindowsApp.lib" CACHE STRING "" FORCE)

# --- 编译选项 ---
# /FI 强制包含 clang-cl-ARM intrinsic 垫片(补 _CountLeadingZeros 等 MSVC STL 用到、
# 但 clang-cl 在 ARM 上没实现的 intrinsic), 必须在 STL 头之前。
add_compile_options(
    /bigobj
    "/FI${CMAKE_CURRENT_LIST_DIR}/clang-cl-arm-shim.h"
    $<$<CONFIG:Release>:/O2>
)

# --- 链接选项(clang-cl 默认用 lld-link, 不支持 /WINMD:NO → 去掉)---
add_link_options(
    /APPCONTAINER
    /MANIFEST:NO
)

set(CMAKE_VS_WINRT_BY_DEFAULT OFF)
