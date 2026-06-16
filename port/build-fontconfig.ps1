# build-fontconfig.ps1  —  手动 cross-build fontconfig 2.17.1 for arm-uwp,clang-cl(C11)。
# 复用 cairo-cross-clang.txt(通用 clang-cl arm-uwp 交叉文件)。
# 依赖:已装 freetype2.pc + expat.pc(installed\arm-uwp\lib\pkgconfig)。
# gperf:用 vcpkg host 版(installed\x64-windows\tools\gperf),meson 生成 fcobjshash.h 用。
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\arm32-uwp-env.ps1" | Out-Null            # INCLUDE/LIB = 14.44 arm + SDK
$env:PATH = "C:\Program Files\LLVM\bin;C:\vcpkg\installed\x64-windows\tools\gperf;$env:PATH"  # clang-cl/lld-link + gperf

$py     = "C:\vcpkg\downloads\tools\python\python-3.14.2-x64-1\python.exe"
if (-not (Test-Path $py)) { $py = (Get-Command python).Source }
$meson  = "C:\vcpkg\downloads\tools\meson-1.9.0-633807\meson.py"
$src    = "C:\vcpkg\buildtrees\fontconfig\src\2.17.1-6edd6fe145.clean"
$build  = "E:\Apotheosis\deps-build\fontconfig"
$prefix = "C:\vcpkg\installed\arm-uwp"
$cross  = "$PSScriptRoot\fontconfig-cross-clang.txt"   # 同 cairo 但去 UNICODE(fontconfig Win 代码走 ANSI 路径 API)

$env:PKG_CONFIG_PATH = "C:\vcpkg\installed\arm-uwp\lib\pkgconfig"
Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "==> meson setup fontconfig (clang-cl, arm-uwp)" -ForegroundColor Cyan
& $py $meson setup $build `
    -Dxml-backend=expat `
    -Dcache-build=disabled -Dtools=disabled -Dtests=disabled `
    -Ddoc=disabled -Ddoc-txt=disabled -Ddoc-man=disabled -Ddoc-pdf=disabled -Ddoc-html=disabled `
    -Dnls=disabled -Diconv=disabled `
    --backend ninja --wrap-mode nodownload -Ddebug=false --libdir lib `
    --native "C:\vcpkg\scripts\buildsystems\meson\none.txt" `
    --cross $cross --prefix $prefix $src
Write-Host "==> meson setup 退出码 $LASTEXITCODE" -ForegroundColor $(if($LASTEXITCODE -eq 0){'Green'}else{'Yellow'})
