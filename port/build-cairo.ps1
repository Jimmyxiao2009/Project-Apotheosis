# build-cairo.ps1  —  手动 cross-build cairo(image 后端)for arm-uwp,用 clang-cl(有 C11)。
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\arm32-uwp-env.ps1" | Out-Null  # INCLUDE/LIB = 14.44 arm + SDK(clang-cl 找 CRT/SDK 头)
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"  # meson 检测 clang-cl 时要找 lld-link

$py    = "C:\vcpkg\downloads\tools\python\python-3.14.2-x64-1\python.exe"
if (-not (Test-Path $py)) { $py = (Get-Command python).Source }
$meson = "C:\vcpkg\downloads\tools\meson-1.9.0-633807\meson.py"
$src   = "C:\vcpkg\buildtrees\cairo\src\1.18.4-237a3f692a.clean"
$build = "E:\Apotheosis\deps-build\cairo"
$prefix= "C:\vcpkg\installed\arm-uwp"
$cross = "$PSScriptRoot\cairo-cross-clang.txt"
$ninja = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

$env:PKG_CONFIG_PATH = "C:\vcpkg\installed\arm-uwp\lib\pkgconfig"
Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "==> meson setup cairo (clang-cl, arm-uwp)" -ForegroundColor Cyan
& $py $meson setup $build `
    -Dfontconfig=enabled -Dfreetype=enabled `
    -Dxlib=disabled -Dxcb=disabled -Dxlib-xcb=disabled `
    -Dglib=disabled -Dlzo=disabled -Dtests=disabled `
    -Dzlib=enabled -Dpng=enabled -Dspectre=disabled -Ddwrite=disabled `
    -Dgtk2-utils=disabled -Dsymbol-lookup=disabled `
    --backend ninja --wrap-mode nodownload -Ddebug=false --libdir lib `
    --native "C:\vcpkg\scripts\buildsystems\meson\none.txt" `
    --cross $cross --prefix $prefix $src
Write-Host "==> meson setup 退出码 $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Yellow'})
