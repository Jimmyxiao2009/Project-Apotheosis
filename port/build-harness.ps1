# build-harness.ps1 — 构建 Phase 1b harness appx(v143 ARM)。
# C++/CX 两段式陷阱:MarkupCompilePass2(生成 .g.hpp)在 ClCompile 之后跑,
# 首次/改 XAML 后必须先显式生成 .g.hpp,否则 ClCompile 找不到。
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
$proj = 'E:\Apotheosis\harness\Harness.vcxproj'
$log = 'E:\Apotheosis\harness-build.log'

Write-Host "=== [1/2] 生成 XAML .g.hpp(MarkupCompilePass1+2) ===" -ForegroundColor Cyan
& $msbuild $proj /p:Configuration=Release /p:Platform=ARM /t:MarkupCompilePass1`;MarkupCompilePass2 /v:minimal 2>&1 | Out-Null

Write-Host "=== [2/2] 全量构建 + 打包 appx ===" -ForegroundColor Cyan
& $msbuild $proj /p:Configuration=Release /p:Platform=ARM /m /v:minimal 2>&1 | Tee-Object $log
$code = $LASTEXITCODE
Write-Host "=== MSBuild 退出码: $code ==="
exit $code   # 传播 MSBuild 退出码(否则脚本恒返回 0,掩盖编译失败)
