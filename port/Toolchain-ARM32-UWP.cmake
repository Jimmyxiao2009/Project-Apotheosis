# ============================================================================
# Toolchain-ARM32-UWP.cmake
# 目标: ARM32 (ARMv7 Thumb-2) + Windows 10 Mobile UWP / App Container
# 设备: Lumia 950 (Snapdragon 810), Win10M 1709 (build 15254)
# 用法: 先在 "ARM Native Tools" 或 `vcvarsall.bat x64_arm uwp 10.0.<sdk>` 环境中,
#       再用 Ninja 配置时通过 -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE 或 -DCMAKE_TOOLCHAIN_FILE
#       引用本文件。
# ============================================================================

set(CMAKE_SYSTEM_NAME       WindowsStore)   # 触发 UWP/AppContainer 构建, 定义 WINAPI_FAMILY_APP
set(CMAKE_SYSTEM_VERSION    10.0)           # 用已安装的最新 Win10 SDK 编译
set(CMAKE_SYSTEM_PROCESSOR  ARM)            # 32-bit ARM
set(CMAKE_CROSSCOMPILING    TRUE)

# 编译器由 vcvars(x64_arm uwp)提供, 不写死路径。Ninja 单配置生成器。

# --- App Container 必备宏 ---
add_compile_definitions(
    WINAPI_FAMILY=WINAPI_FAMILY_APP
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _UNICODE UNICODE
    _WIN32_WINNT=0x0A00
    __WRL_NO_DEFAULT_LIB__
)

# --- 补齐纯 MSVC 缺失的 GCC/Clang 内建宏(WebKit 已弃 MSVC, 大量代码假设它们存在)---
# ARM32: 指针 4 字节、小端。PlatformCPU.h 等靠这些判断, 否则 #error 或误判成大端。
add_compile_definitions(
    __SIZEOF_POINTER__=4
    __ORDER_LITTLE_ENDIAN__=1234
    __ORDER_BIG_ENDIAN__=4321
    __ORDER_PDP_ENDIAN__=3412
    __BYTE_ORDER__=1234
)

# --- 只链 UWP 伞库, 去掉桌面默认库 (kernel32/user32/...) ---
set(CMAKE_C_STANDARD_LIBRARIES   "WindowsApp.lib" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "WindowsApp.lib" CACHE STRING "" FORCE)

# --- 编译选项 ---
add_compile_options(
    /EHsc                       # C++ 异常
    /bigobj                     # CLoop 的 LowLevelInterpreter 单 obj 段巨多, 必须
    /Zc:__cplusplus
    /permissive-
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Release>:/Gy>    # 函数级链接
)

# --- 链接选项 ---
add_link_options(
    /APPCONTAINER               # WindowsStore 一般会加, 显式保证
    /MANIFEST:NO
    /WINMD:NO
)

# CMake 在 WindowsStore 下可能默认对目标加 /ZW(C++/CX), WebKit 是纯 C++, 关掉:
set(CMAKE_VS_WINRT_BY_DEFAULT OFF)
string(REPLACE "/ZW" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# 注意: 若改用 VS 生成器 (-G "Visual Studio 18 2026" -A ARM), 需在 vcxproj 层
# 设 <CompileAsWinRT>false</CompileAsWinRT>, 上面的 /ZW 剥离仅对 Ninja 路径有效。
