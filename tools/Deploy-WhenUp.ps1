#Requires -Version 7.0
# Deploy-WhenUp.ps1 — 轮询设备,一旦稳定上线就部署最新 appx(经 Wdp-Cycle),再做初始化崩溃检查。
# 单轮最多 ~8.5 分钟(避开后台命令 10 分钟上限);未上线则退出码 2,由上层重启本脚本继续守候。
param([int]$Ip = 0)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Net.Http
$base = 'https://192.168.3.51:443'
function Probe {
    $h = [System.Net.Http.HttpClientHandler]::new()
    $h.ServerCertificateCustomValidationCallback = [System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
    $c = [System.Net.Http.HttpClient]::new($h); $c.Timeout = [TimeSpan]::FromSeconds(5)
    try { $r = $c.GetAsync("$base/api/os/info").GetAwaiter().GetResult(); return ([int]$r.StatusCode -eq 200) } catch { return $false }
}

for ($i = 0; $i -lt 16; $i++) {
    if (Probe) {
        Start-Sleep -Seconds 8
        if (Probe) {   # 连续两次成功 = 连接稳定,开始非破坏式部署(失败保留旧版,可重试)
            Write-Host "=== 设备稳定上线,部署最新版(非破坏式)==="
            & pwsh -NoProfile -File 'E:\Apotheosis\tools\Deploy-Robust.ps1' 2>&1 | Select-Object -Last 12
            if ($LASTEXITCODE -eq 0) { Write-Host "=== 部署成功 ==="; exit 0 }
            Write-Host "=== 部署未成功(退出码 $LASTEXITCODE),保留旧版,稍后重试 ==="
            exit 4   # 部署没成,让上层再续一轮重试
        }
    }
    Start-Sleep -Seconds 30
}
Write-Host "设备仍未稳定上线(本轮 ~8.5 分钟超时)"
exit 2
