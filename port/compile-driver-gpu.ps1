# compile-driver-gpu.ps1 — 同 compile-driver-jit.ps1,但头/库路径换成 build-clang-gpu
# (GPU 配置:USE_TEXTURE_MAPPER/USE_ANGLE ON + ENABLE_JIT ON;TextureMapper/GraphicsLayer 头才有)。
# 用法: pwsh -File port\compile-driver-gpu.ps1 <src.cpp> <out.obj>
param([Parameter(Mandatory)][string]$Src, [Parameter(Mandatory)][string]$Obj)
$ErrorActionPreference = 'Stop'
. 'E:\Apotheosis\port\arm32-uwp-env.ps1' *> $null

$cmd = (Get-Content 'E:\Apotheosis\port\harness-cmd.bat' -Raw).Trim()
$cmd = $cmd -replace '/showIncludes',''
$cmd = $cmd -replace '/Fo\S+',''
$cmd = $cmd -replace '/Fd\S+',''
$cmd = $cmd -replace '-c\s+--\s+\S+UnifiedSource\S+',''
$cmd = $cmd -replace 'build-clang-webcore','build-clang-gpu'
$cmd = "$cmd -IE:\Apotheosis\WebKit\Source\WebKitLegacy\WebCoreSupport -IE:\Apotheosis\WebKit\Source\WebKitLegacy /Fo`"$Obj`" -c -- `"$Src`""

$bat = "E:\Apotheosis\port\_driver_compile_gpu.bat"
Set-Content -Path $bat -Value $cmd -Encoding ASCII
$log = "E:\Apotheosis\port\driver-compile-gpu.log"
Set-Location 'E:\Apotheosis\build-clang-gpu'
cmd /c $bat 1> $log 2>&1
$exit = $LASTEXITCODE
$err = @(Select-String -Path $log -Pattern ': error:|fatal error:').Count
Write-Host "[compile-driver-gpu] EXIT=$exit  errors=$err  obj=$Obj  log=$log"
if ($exit -ne 0) { Get-Content $log -Tail 25 }
