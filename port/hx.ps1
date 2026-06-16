# hx.ps1 — 单 TU harness 跑测器:同步源→构建副本头,编译已知失败 unified source,统计 pack-expansion。
# 用法: pwsh -File port\hx.ps1 <tag>
param([string]$Tag = "run")

$ErrorActionPreference = 'Stop'
. 'E:\Apotheosis\port\arm32-uwp-env.ps1' *> $null   # 静默注入 ARM/SDK 环境(INCLUDE 供 clang-cl)

# 关键:同步源头 → 构建头副本(否则测的是陈旧副本 = 假结果)
Copy-Item 'E:\Apotheosis\WebKit\Source\WTF\wtf\StdLibExtras.h' 'E:\Apotheosis\build-clang-webcore\WTF\Headers\wtf\StdLibExtras.h' -Force
Copy-Item 'E:\Apotheosis\WebKit\Source\WTF\wtf\Variant.h'      'E:\Apotheosis\build-clang-webcore\WTF\Headers\wtf\Variant.h'      -Force
# 注:WebCore PrivateHeaders 是指回源的符号链接,改源即生效,无需同步(仅 WTF Headers 是真副本需手动同步)

Set-Location 'E:\Apotheosis\build-clang-webcore'
$log = "E:\Apotheosis\port\harness-$Tag.log"
cmd /c 'E:\Apotheosis\port\harness-cmd.bat' 1> $log 2>&1
$exit = $LASTEXITCODE

$pe  = @(Select-String -Path $log -SimpleMatch 'cannot mangle this pack expansion').Count
$dt  = @(Select-String -Path $log -SimpleMatch 'cannot mangle this decltype').Count
$err = @(Select-String -Path $log -Pattern ': error:').Count
Write-Host "[$Tag] EXIT=$exit  pack-expansion=$pe  decltype=$dt  total-errors=$err  log=$log"
