# 精简增量构建:不删 PCH(本轮未改 PCH 头),只重生成被删的 binding + 重编受影响 TU。
. 'E:\Apotheosis\port\arm32-uwp-env.ps1' *> $null
$log = 'E:\Apotheosis\webcore-pch7.log'
$ninja = (Get-Command ninja).Source
& $ninja -k 0 -C E:\Apotheosis\build-clang-webcore WebCore 2>&1 | Tee-Object $log
Write-Host "=== ninja 退出码: $LASTEXITCODE ==="
