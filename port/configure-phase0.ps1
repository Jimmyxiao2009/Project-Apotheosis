# ============================================================================
# configure-phase0.ps1  —  配置 WTF + JavaScriptCore (CLoop) for ARM32 UWP
# 编译器: VS2026 v143 (14.44) arm cl, 经手动环境(arm32-uwp-env.ps1)驱动
#         —— 因 VS2026 vcvarsall 已移除 arm32 target, 不能用 vcvars。
# ICU:    vcpkg arm-uwp (用 VS2017 v141 编, 见 port\vcpkg-triplets\arm-uwp.cmake)
# ============================================================================
param(
    [string]$Root   = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet('Release','Debug')] [string]$Config = 'Release',
    [string]$IcuRoot = 'C:\icu-arm-uwp',
    [switch]$Clang   # Path B: 用 clang-cl 工具链(modern WebKit 必需)
)
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\arm32-uwp-env.ps1"   # 注入 INCLUDE/LIB/PATH + ruby/perl/python(clang-cl 也读 INCLUDE/LIB)

$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$WebKit = Join-Path $Root 'WebKit'
if ($Clang) {
    $Build  = Join-Path $Root ('build-clang-' + $Config.ToLower())
    $Toolchain = Join-Path $PSScriptRoot 'Toolchain-ARM32-UWP-clang.cmake'
} else {
    $Build  = Join-Path $Root ('build-' + $Config.ToLower())
    $Toolchain = Join-Path $PSScriptRoot 'Toolchain-ARM32-UWP.cmake'
}

if (-not (Test-Path "$IcuRoot\include\unicode\uversion.h")) {
    Write-Host "!! 未找到 ICU ($IcuRoot)。先跑 vcpkg install icu:arm-uwp。" -ForegroundColor Yellow
}

Write-Host "==> configure WinUWP ($Config)" -ForegroundColor Cyan
& $cmake -S $WebKit -B $Build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    "-DCMAKE_PREFIX_PATH=$IcuRoot" `
    "-DPORT=WinUWP" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DENABLE_STATIC_JSC=ON"

if ($LASTEXITCODE -eq 0) {
    Write-Host "==> configure 成功。编译: " -ForegroundColor Green -NoNewline
    Write-Host "& '$ninja' -C '$Build' JavaScriptCore" -ForegroundColor Green
} else {
    Write-Host "==> configure 失败 (exit $LASTEXITCODE)" -ForegroundColor Red
}
