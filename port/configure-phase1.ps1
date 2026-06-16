# ============================================================================
# configure-phase1.ps1 — 配置 WTF + JSC + WebCore for ARM32 UWP(clang-cl)。
# 在 Phase 0 基础上:CMAKE_PREFIX_PATH 增加 vcpkg arm-uwp(cairo/freetype/fontconfig/
# harfbuzz/jpeg/webp/xml/sqlite/png/zlib),并配 pkg-config(Cairo/HarfBuzz/Fontconfig/
# WebP 的 Find 模块靠 pkg-config)。网络栈(Curl/OpenSSL/PSL)本轮延后。
# ============================================================================
param(
    [ValidateSet('Release','Debug')] [string]$Config = 'Release',
    [string]$IcuRoot = 'C:\icu-arm-uwp'
)
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\arm32-uwp-env.ps1"

$Root   = Split-Path -Parent $PSScriptRoot
$cmake  = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja  = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$WebKit = Join-Path $Root 'WebKit'
$Build  = Join-Path $Root 'build-clang-webcore'
$Toolchain = Join-Path $PSScriptRoot 'Toolchain-ARM32-UWP-clang.cmake'

$pkgconfig = (Get-ChildItem "C:\vcpkg\downloads\tools\msys2" -Recurse -Filter "pkg-config.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
$env:PKG_CONFIG_PATH = "C:\vcpkg\installed\arm-uwp\lib\pkgconfig"
# gperf(host 工具,WebKit 代码生成需要):用 vcpkg 装的 x64 版
$env:PATH = "C:\vcpkg\installed\x64-windows\tools\gperf;$env:PATH"
Write-Host "pkg-config: $pkgconfig" -ForegroundColor DarkGray

Write-Host "==> configure WinUWP Phase 1 (WebCore, $Config)" -ForegroundColor Cyan
& $cmake -S $WebKit -B $Build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    "-DCMAKE_PREFIX_PATH=$IcuRoot;C:\vcpkg\installed\arm-uwp" `
    "-DPKG_CONFIG_EXECUTABLE=$pkgconfig" `
    "-DPORT=WinUWP" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DENABLE_STATIC_JSC=ON"
Write-Host "==> configure 退出码 $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Red'})
