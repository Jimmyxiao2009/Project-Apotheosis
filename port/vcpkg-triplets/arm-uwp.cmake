# overlay triplet: arm-uwp 用 VS2017 v141 构建(VS2026 vcvarsall 已移除 arm32 target)
# v141 编出的 ICU 与 v143 编出的 WebKit ABI 兼容(同 v14x / msvcp140 / UCRT)
# 实际使用副本在 ASCII 路径 C:\vcpkg-overlay\triplets\ (vcpkg 对非 ASCII 路径敏感)
set(VCPKG_TARGET_ARCHITECTURE arm)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME WindowsStore)
set(VCPKG_CMAKE_SYSTEM_VERSION 10.0.19041.0)

set(VCPKG_VISUAL_STUDIO_PATH "C:/Program Files (x86)/Microsoft Visual Studio/2017/Community")
set(VCPKG_PLATFORM_TOOLSET v141)
