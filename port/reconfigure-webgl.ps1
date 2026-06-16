# 一次性增量重配:翻掉已固化的 ENABLE_WEBGL/WEBGPU=ON cache(.cmake 默认已改 OFF,但旧 cache 需 -D 覆盖)。
# 复用 configure-phase1.ps1 的 env 与 cmake 调用,只多两个 -D。重配后受影响的 5 个 WebGL binding 会重生成为 3-类型 union。
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\arm32-uwp-env.ps1" *> $null

$Root   = Split-Path -Parent $PSScriptRoot
$cmake  = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja  = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$WebKit = Join-Path $Root 'WebKit'
$Build  = Join-Path $Root 'build-clang-webcore'
$Toolchain = Join-Path $PSScriptRoot 'Toolchain-ARM32-UWP-clang.cmake'

$pkgconfig = (Get-ChildItem "C:\vcpkg\downloads\tools\msys2" -Recurse -Filter "pkg-config.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
$env:PKG_CONFIG_PATH = "C:\vcpkg\installed\arm-uwp\lib\pkgconfig"
$env:PATH = "C:\vcpkg\installed\x64-windows\tools\gperf;$env:PATH"

Write-Host "==> reconfigure (ENABLE_WEBGL/WEBGPU=OFF override)" -ForegroundColor Cyan
& $cmake -S $WebKit -B $Build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    "-DCMAKE_PREFIX_PATH=C:\icu-arm-uwp;C:\vcpkg\installed\arm-uwp" `
    "-DPKG_CONFIG_EXECUTABLE=$pkgconfig" `
    "-DPORT=WinUWP" `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DENABLE_STATIC_JSC=ON" `
    "-DENABLE_WEBGL=OFF" `
    "-DENABLE_WEBGPU=OFF"
Write-Host "==> reconfigure 退出码 $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Red'})
