# ============================================================================
# setup-env.ps1  —  补齐 Phase 0 构建机的非 VS 依赖
# 顺序: 先在 VS Installer 装好 [ARM build tools] + [C++ UWP support (ARM)] +
#       [C++ CMake 工具], 再跑本脚本。
# 自动处理: Ruby(offlineasm)、CMake(若缺)、vcpkg + icu:arm-uwp。
# ============================================================================
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)
$ErrorActionPreference = 'Stop'

function Have($name) { [bool](Get-Command $name -ErrorAction SilentlyContinue) }

Write-Host "==> 检查 / 安装 Ruby (offlineasm 硬依赖)" -ForegroundColor Cyan
if (Have ruby) { Write-Host "    ruby 已在: $((Get-Command ruby).Source)" }
else {
    winget install --id RubyInstallerTeam.Ruby.3.3 --accept-source-agreements --accept-package-agreements
    Write-Host "    Ruby 装好后请重开终端使 PATH 生效。" -ForegroundColor Yellow
}

Write-Host "==> 检查 CMake" -ForegroundColor Cyan
if (Have cmake) { Write-Host "    cmake 已在: $((Get-Command cmake).Source)" }
else {
    Write-Host "    PATH 上无 cmake。优先用 VS 自带(装了 'C++ CMake 工具'后在" -ForegroundColor Yellow
    Write-Host "    ...\VC\Tools\... 或用 Developer PowerShell);或 winget install Kitware.CMake" -ForegroundColor Yellow
}

Write-Host "==> 检查 Python / Perl" -ForegroundColor Cyan
foreach ($t in 'python','perl') {
    if (Have $t) { Write-Host "    $t 已在" } else { Write-Host "    缺 $t (JSC 生成阶段需要)" -ForegroundColor Yellow }
}

Write-Host "==> 准备 vcpkg + icu:arm-uwp" -ForegroundColor Cyan
$vcpkg = Join-Path $Root "vcpkg"
if (-not (Test-Path (Join-Path $vcpkg "vcpkg.exe"))) {
    if (-not (Test-Path $vcpkg)) {
        git clone https://github.com/microsoft/vcpkg.git $vcpkg
    }
    & (Join-Path $vcpkg "bootstrap-vcpkg.bat") -disableMetrics
}
$env:VCPKG_ROOT = $vcpkg
Write-Host "    VCPKG_ROOT = $vcpkg"
Write-Host "    安装 ICU (arm-uwp triplet)... 这步要用到 ARM32 UWP 编译器,确保 VS 组件已装。" -ForegroundColor Yellow
& (Join-Path $vcpkg "vcpkg.exe") install icu:arm-uwp

Write-Host "`n==> 完成。把 VCPKG_ROOT 设为环境变量后即可 configure:" -ForegroundColor Green
Write-Host "    [Environment]::SetEnvironmentVariable('VCPKG_ROOT','$vcpkg','User')" -ForegroundColor Green
Write-Host "    然后在 'ARM' 开发者环境里跑 port\configure-phase0.ps1" -ForegroundColor Green
