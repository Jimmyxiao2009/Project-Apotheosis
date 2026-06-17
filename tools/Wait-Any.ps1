#Requires -Version 7.0
# Wait-Any.ps1 — 合并心跳。同时盯 GPU 构建(gpu-build-status.txt)+ GPU 探针(gpu-verify-status.txt)。
# 任一完成(构建 DONE/FAIL,或探针 DONE)→ exit 0 唤我;否则 ~9 分钟超时 exit 2(上层续等)。
# 顺便看护两个脱离式进程(死了且未完成则重启)。
$buildS = 'E:\Apotheosis\gpu-build-status.txt'
$probeS = 'E:\Apotheosis\gpu-verify-status.txt'
function Alive($pat) { [bool](Get-CimInstance Win32_Process -Filter "Name='pwsh.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match $pat }) }
for ($i = 0; $i -lt 18; $i++) {
    $b = if (Test-Path $buildS) { (Get-Content $buildS -Raw -EA SilentlyContinue).Trim() } else { '' }
    $p = if (Test-Path $probeS) { (Get-Content $probeS -Raw -EA SilentlyContinue).Trim() } else { '' }
    Write-Host "[$i] build=[$b] probe=[$p]"
    if ($b -match 'DONE|FAIL') { Write-Host 'BUILD_FINISHED'; exit 0 }
    if ($p -match 'DONE')      { Write-Host 'PROBE_DONE'; exit 0 }
    # 看护:构建进程死了但 marker 还 BUILDING → 重启(ninja 增量续编)
    if ($b -match 'BUILDING' -and -not (Alive 'build-gpu-detached\.ps1')) {
        Start-Process pwsh -ArgumentList @('-NoProfile','-WindowStyle','Hidden','-File','E:\Apotheosis\port\build-gpu-detached.ps1') -WindowStyle Hidden
        Write-Host '构建进程缺失 → 已重启(增量续编)'
    }
    # 看护:探针进程死了但未 DONE → 重启
    if ($p -notmatch 'DONE' -and -not (Alive 'Verify-GPU-Detached\.ps1')) {
        Start-Process pwsh -ArgumentList @('-NoProfile','-WindowStyle','Hidden','-File','E:\Apotheosis\tools\Verify-GPU-Detached.ps1','-Ip','192.168.3.51') -WindowStyle Hidden
        Write-Host '探针进程缺失 → 已重启'
    }
    Start-Sleep -Seconds 30
}
Write-Host 'STILL_WAITING'; exit 2
