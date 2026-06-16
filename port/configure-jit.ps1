# configure-jit.ps1 — 在独立目录 build-clang-jit 里配置启用 JIT 的 JSC/WebCore。
# 与 configure-phase1 同,但:ENABLE_C_LOOP=OFF, ENABLE_JIT=ON(Baseline),DFG/FTL 先 OFF。
# ARMv7 Thumb-2 JIT 在 JSC 里有(WebKitFeatures.cmake:97 给 ARM-Linux 开),这里强行给 ARM-Windows 开。
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
$Build  = Join-Path $Root 'build-clang-jit'
$Toolchain = Join-Path $PSScriptRoot 'Toolchain-ARM32-UWP-clang.cmake'

$pkgconfig = (Get-ChildItem "C:\vcpkg\downloads\tools\msys2" -Recurse -Filter "pkg-config.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
$env:PKG_CONFIG_PATH = "C:\vcpkg\installed\arm-uwp\lib\pkgconfig"
$env:PATH = "C:\vcpkg\installed\x64-windows\tools\gperf;$env:PATH"

Write-Host "==> configure JIT build (build-clang-jit, $Config)" -ForegroundColor Cyan
& $cmake -S $WebKit -B $Build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    "-DCMAKE_PREFIX_PATH=$IcuRoot;C:\vcpkg\installed\arm-uwp" `
    "-DPKG_CONFIG_EXECUTABLE=$pkgconfig" `
    "-DPORT=WinUWP" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DENABLE_STATIC_JSC=ON" `
    "-DENABLE_C_LOOP=OFF" `
    "-DENABLE_JIT=ON" `
    "-DENABLE_DFG_JIT=OFF" `
    "-DENABLE_FTL_JIT=OFF" `
    "-DENABLE_SAMPLING_PROFILER=OFF" `
    "-DUSE_SYSTEM_MALLOC=ON"
Write-Host "==> configure 退出码 $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Red'})
