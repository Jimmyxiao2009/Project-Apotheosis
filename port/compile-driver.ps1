# compile-driver.ps1 — 用与 WebCore TU 完全相同的 clang-cl flags 独立编一个驱动 .cpp。
# 复用 harness-cmd.bat 的完整 -I/-D/flags(它已去 PCH),只替换 /showIncludes、/Fo、/Fd、源文件尾部。
# 用法: pwsh -File port\compile-driver.ps1 <src.cpp> <out.obj>
param([Parameter(Mandatory)][string]$Src, [Parameter(Mandatory)][string]$Obj)
$ErrorActionPreference = 'Stop'
. 'E:\Apotheosis\port\arm32-uwp-env.ps1' *> $null

$cmd = (Get-Content 'E:\Apotheosis\port\harness-cmd.bat' -Raw).Trim()
# 去掉 harness 专属的输出/源/showIncludes 部分
$cmd = $cmd -replace '/showIncludes',''
$cmd = $cmd -replace '/Fo\S+',''
$cmd = $cmd -replace '/Fd\S+',''
$cmd = $cmd -replace '-c\s+--\s+\S+UnifiedSource\S+',''
# 追加 WebKitLegacy include 目录(PortPlatformStrategies 需 WebResourceLoadScheduler.h),再编译(只编不链)
$cmd = "$cmd -IE:\Apotheosis\WebKit\Source\WebKitLegacy\WebCoreSupport -IE:\Apotheosis\WebKit\Source\WebKitLegacy /Fo`"$Obj`" -c -- `"$Src`""

# 写进临时 .bat 当文件跑,绕开 cmd 8191 命令行长度上限
$bat = "E:\Apotheosis\port\_driver_compile.bat"
Set-Content -Path $bat -Value $cmd -Encoding ASCII
$log = "E:\Apotheosis\port\driver-compile.log"
Set-Location 'E:\Apotheosis\build-clang-webcore'   # 相对 include(若有)对齐
cmd /c $bat 1> $log 2>&1
$exit = $LASTEXITCODE
$err = @(Select-String -Path $log -Pattern ': error:|fatal error:').Count
Write-Host "[compile-driver] EXIT=$exit  errors=$err  obj=$Obj  log=$log"
if ($exit -ne 0) { Get-Content $log -Tail 25 }
