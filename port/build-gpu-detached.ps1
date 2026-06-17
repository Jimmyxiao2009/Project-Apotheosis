# build-gpu-detached.ps1 — 脱离式跑 GPU WebCore 构建(build-clang-gpu,TextureMapper+ANGLE+JIT)。
# 多小时,由 Start-Process 脱离启动(不受 10 分钟后台上限)。状态写 gpu-build-status.txt(BUILDING/DONE/FAIL),
# 全量输出写 build-gpu.log。撞 thumbv7 移植编译错会 FAIL,看 log 修了再起。
$ErrorActionPreference = 'Continue'
. 'E:\Apotheosis\port\arm32-uwp-env.ps1' *> $null
$build = 'E:\Apotheosis\build-clang-gpu'
$ninja = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$marker = 'E:\Apotheosis\gpu-build-status.txt'
$log = 'E:\Apotheosis\build-gpu.log'
Set-Content $marker "BUILDING since $(Get-Date -Format 'MM-dd HH:mm:ss')"
# 目标 WebCore 会拉起依赖 PAL/WTF/JavaScriptCore;-k 0 继续编完尽量多错一次性暴露
& $ninja -C $build -k 0 WTF PAL JavaScriptCore WebCore *>&1 | Out-File $log -Encoding utf8
$code = $LASTEXITCODE
if ($code -eq 0) { Set-Content $marker "DONE $(Get-Date -Format 'MM-dd HH:mm:ss')" }
else { Set-Content $marker "FAIL(exit $code) $(Get-Date -Format 'MM-dd HH:mm:ss')" }
