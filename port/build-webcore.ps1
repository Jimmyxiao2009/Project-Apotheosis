# 全量重编 WebCore,keep-going 收集所有残留。先删 WebCore PCH 防 mtime 陈旧。
. 'E:\Apotheosis\port\arm32-uwp-env.ps1' *> $null
$log = 'E:\Apotheosis\webcore-pch4.log'
$ninja = (Get-Command ninja).Source
# 删除 WebCore 陈旧 PCH —— 强制本次运行内重建,与 TU 同批 mtime 自洽
Remove-Item 'E:\Apotheosis\build-clang-webcore\Source\WebCore\CMakeFiles\WebCore.dir\cmake_pch.cxx.pch','E:\Apotheosis\build-clang-webcore\Source\WebCore\CMakeFiles\WebCore.dir\cmake_pch.cxx.obj' -Force -ErrorAction SilentlyContinue
& $ninja -k 0 -C E:\Apotheosis\build-clang-webcore WebCore 2>&1 | Tee-Object $log
Write-Host "=== ninja 退出码: $LASTEXITCODE ==="
