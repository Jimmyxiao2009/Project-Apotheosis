# watch-and-deploy.ps1  —  扫子网, 发现 Win10M 的 Device Portal 就自动部署 harness。
param(
    [string]$Subnet = '192.168.3',
    [int]$Minutes = 25
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$appx = Join-Path $root 'harness\AppPackages\Harness\Harness_0.1.0.0_ARM_Test\Harness_0.1.0.0_ARM.appx'
$cer  = Join-Path $root 'harness\EdgeHTMLReborn.cer'
$deadline = (Get-Date).AddMinutes($Minutes)
$tried = @{}

function Probe-Wdp($ip) {
    foreach ($s in @(@{sc='https';pt=443}, @{sc='http';pt=80})) {
        try {
            $r = Invoke-WebRequest -Uri "$($s.sc)://${ip}:$($s.pt)/api/os/info" -SkipCertificateCheck -TimeoutSec 3 -ErrorAction Stop
            return @{ scheme=$s.sc; port=$s.pt; body=$r.Content }
        } catch {
            if ($_.Exception.Message -match '401|Unauthorized') { return @{ scheme=$s.sc; port=$s.pt; body='AUTH' } }
        }
    }
    return $null
}

Write-Host "==> 守望 $Subnet.x 上的 Win10M Device Portal(最多 $Minutes 分钟)..." -ForegroundColor Cyan
while ((Get-Date) -lt $deadline) {
    # 并行 ping 扫 .2 - .254
    $live = 2..254 | ForEach-Object -Parallel {
        if (Test-Connection -ComputerName "$using:Subnet.$_" -Count 1 -Quiet -TimeoutSeconds 1) { "$using:Subnet.$_" }
    } -ThrottleLimit 64
    foreach ($ip in $live) {
        if ($tried[$ip]) { continue }
        $wdp = Probe-Wdp $ip
        if (-not $wdp) { continue }
        $tried[$ip] = $true
        Write-Host "==> $ip 有 Device Portal($($wdp.scheme):$($wdp.port)): $($wdp.body)" -ForegroundColor Yellow
        # 识别是否 Windows Mobile
        if ($wdp.body -eq 'AUTH') {
            Write-Host "    需要认证 —— 请用 deploy-phase0.ps1 -DeviceIp $ip -Credential (Get-Credential) 手动部署" -ForegroundColor Yellow
            continue
        }
        if ($wdp.body -match 'Mobile|Phone|Lumia|10\.0\.1[0-9]{4}') {
            Write-Host "==> 判定为 Win10M 设备! 自动部署 ->" -ForegroundColor Green
            & (Join-Path $PSScriptRoot '..\tools\Wdp-Deploy.ps1') -Appx $appx -Certificate $cer -DeviceIp $ip -Scheme $wdp.scheme -Port $wdp.port
            Write-Host "==> 部署流程结束(IP $ip)。" -ForegroundColor Green
            return
        } else {
            Write-Host "    不像 Win10M(可能是别的设备),跳过。" -ForegroundColor DarkGray
        }
    }
    Start-Sleep -Seconds 15
}
Write-Host "==> 守望超时($Minutes 分钟内未发现 Win10M 设备)。设备上线后手动跑 deploy-phase0.ps1。" -ForegroundColor Yellow
