# 增量重编 WebCore.lib(改了 RenderLayerBacking.cpp 的 paintsIntoWindow)→ 重链驱动 → 重打 harness。
$ErrorActionPreference='Continue'
. 'E:\Apotheosis\port\arm32-uwp-env.ps1' *> $null
$ninja = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$build = 'E:\Apotheosis\build-clang-gpu'
Write-Host "=== [1/3] ninja WebCore(增量) ==="
& $ninja -C $build WebCore *>&1 | Tee-Object 'E:\Apotheosis\port\_webcore-rebuild.log' | Select-Object -Last 3
$wc=$LASTEXITCODE
Write-Host "ninja WebCore exit=$wc"
if($wc -ne 0){ Write-Host 'WEBCORE FAIL'; exit 1 }
Write-Host "=== [2/3] 重链驱动 ==="
& pwsh -NoProfile -File 'E:\Apotheosis\port\link-driver-gpu.ps1' *>&1 | Out-File 'E:\Apotheosis\port\_relink-0715.log' -Encoding utf8
if(-not (Select-String -Path 'E:\Apotheosis\port\_relink-0715.log' -Pattern 'undefined=0  duplicate=0')){ Write-Host 'DRIVER LINK FAIL'; Get-Content 'E:\Apotheosis\port\_relink-0715.log' -Tail 10; exit 1 }
Write-Host "驱动链接 OK"
Write-Host "=== [3/3] 打 harness ==="
& pwsh -NoProfile -File 'E:\Apotheosis\port\build-harness.ps1' *>&1 | Out-File 'E:\Apotheosis\harness\_buildharness-0715.log' -Encoding utf8
Write-Host "harness exit=$LASTEXITCODE"
