#Requires -Version 7.0
# Watcher-Detached.ps1 — 脱离式常驻看门狗。独立进程持续轮询设备,稳定上线即非破坏式部署最新 appx,
# 结果写 deploy-status.txt(SUCCESS/FAIL/WATCHING)。部署成功后退出。由 Start-Process 脱离启动,
# 不受 10 分钟后台命令上限约束,可守整夜。
$marker = 'E:\Apotheosis\deploy-status.txt'
$base = 'https://192.168.3.51:443'
Add-Type -AssemblyName System.Net.Http
function Probe {
    $h = [System.Net.Http.HttpClientHandler]::new()
    $h.ServerCertificateCustomValidationCallback = [System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
    $c = [System.Net.Http.HttpClient]::new($h); $c.Timeout = [TimeSpan]::FromSeconds(5)
    try { $r = $c.GetAsync("$base/api/os/info").GetAwaiter().GetResult(); return ([int]$r.StatusCode -eq 200) } catch { return $false }
}
Set-Content $marker "WATCHING since $(Get-Date -Format 'MM-dd HH:mm:ss')"
$fails = 0
while ($true) {
    if (Probe) {
        # 设备一冒头就立刻开传(不等第二次探测),最大化利用短暂窗口。Deploy-Robust 内部重试上传。
        Set-Content $marker "DEPLOYING $(Get-Date -Format 'MM-dd HH:mm:ss')…"
        $out = (& pwsh -NoProfile -File 'E:\Apotheosis\tools\Deploy-Robust.ps1' 2>&1 | Out-String)
        $code = $LASTEXITCODE
        if ($code -eq 0) {
            Set-Content $marker "SUCCESS $(Get-Date -Format 'MM-dd HH:mm:ss')`n$out"
            exit 0
        }
        $fails++
        Set-Content $marker "FAIL#$fails $(Get-Date -Format 'MM-dd HH:mm:ss') (exit $code)`n$out"
        Start-Sleep -Seconds 10   # 失败后短歇再探(连接可能很快恢复)
    } else {
        Start-Sleep -Seconds 12
    }
}
