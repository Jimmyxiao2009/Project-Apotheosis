#Requires -Version 7.0
# Wait-GPU-Done.ps1 — 轻量心跳。只读 gpu-verify-status.txt,每 30s,最多 ~9 分钟。
# DONE → exit 0(可读 gpu-verify-result.txt);否则超时 exit 2(由上层续等)。顺便看护常驻 GPU 验证进程。
$status = 'E:\Apotheosis\gpu-verify-status.txt'
function Ensure-Detached {
    $alive = Get-CimInstance Win32_Process -Filter "Name='pwsh.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'Verify-GPU-Detached\.ps1' }
    if (-not $alive) {
        Start-Process pwsh -ArgumentList @('-NoProfile','-WindowStyle','Hidden','-File','E:\Apotheosis\tools\Verify-GPU-Detached.ps1','-Ip','192.168.3.51') -WindowStyle Hidden
        Write-Host 'GPU 验证进程缺失 → 已重启'
    }
}
for ($i = 0; $i -lt 18; $i++) {
    $s = ''
    if (Test-Path $status) { $s = (Get-Content $status -Raw -ErrorAction SilentlyContinue) }
    Write-Host "[$i] $($s.Trim())"
    if ($s -match 'DONE') { Write-Host 'GPU_VERIFY_DONE'; exit 0 }
    if ($s -notmatch 'DONE') { Ensure-Detached }
    Start-Sleep -Seconds 30
}
Write-Host 'STILL_WATCHING'
exit 2
