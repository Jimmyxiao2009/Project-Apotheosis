#Requires -Version 7.0
# Wait-Done.ps1 — 轻量心跳。只读 jit-verify-status.txt(不部署/不碰设备),每 30s 一次,最多 ~9 分钟。
#   标记含 DONE → exit 0(验证完成,可读 jit-verify-result.txt);否则超时 exit 2(由上层重启续等)。
$status = 'E:\Apotheosis\jit-verify-status.txt'
function Ensure-Detached {
    # 常驻验证进程不在(且未 DONE)则重启它。排除自身($PID)与查询命令自身。
    $alive = Get-CimInstance Win32_Process -Filter "Name='pwsh.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'Verify-JIT-Detached\.ps1' }
    if (-not $alive) {
        Start-Process pwsh -ArgumentList @('-NoProfile','-WindowStyle','Hidden','-File','E:\Apotheosis\tools\Verify-JIT-Detached.ps1','-Ip','192.168.3.51') -WindowStyle Hidden
        Write-Host '常驻验证进程缺失 → 已重启'
    }
}
for ($i = 0; $i -lt 18; $i++) {
    $s = ''
    if (Test-Path $status) { $s = (Get-Content $status -Raw -ErrorAction SilentlyContinue) }
    Write-Host "[$i] $($s.Trim())"
    if ($s -match 'DONE') { Write-Host 'VERIFY_DONE'; exit 0 }
    if ($s -notmatch 'DONE') { Ensure-Detached }
    Start-Sleep -Seconds 30
}
Write-Host 'STILL_WATCHING'
exit 2
