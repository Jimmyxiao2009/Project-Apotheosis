#Requires -Version 7.0
# Verify-JIT-WhenUp.ps1 — 设备一稳定上线就:部署最新 appx → 开崩溃 dump → 拉起 → 等 → 拉 jitresult.txt/stage.txt
# + 查崩溃 dump,把结论写 E:\Apotheosis\jit-verify-result.txt。
# 单轮最多 ~8 分钟轮询(避开后台 10 分钟上限)。设备未上线 exit 2(由上层重启续守);完成验证 exit 0。
param([string]$Ip = '192.168.3.51')
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Net.Http
$base = "https://${Ip}:443"
$result = 'E:\Apotheosis\jit-verify-result.txt'

function New-Client {
    $h = [System.Net.Http.HttpClientHandler]::new()
    $h.ServerCertificateCustomValidationCallback = [System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
    $h.CookieContainer = [System.Net.CookieContainer]::new()
    $c = [System.Net.Http.HttpClient]::new($h); $c.Timeout = [TimeSpan]::FromSeconds(20)
    $r0 = $null; try { $r0 = $c.GetAsync("$base/api/os/info").GetAwaiter().GetResult() } catch { return $null }
    if (-not $r0 -or [int]$r0.StatusCode -ne 200) { return $null }
    $csrf = $null; $sc = $null
    if ($r0.Headers.TryGetValues('Set-Cookie', [ref]$sc)) {
        foreach ($l in $sc) { if ($l -match 'CSRF-Token=([^;,\s]+)') { $csrf = $Matches[1]; $h.CookieContainer.Add([Uri]$base, [System.Net.Cookie]::new('CSRF-Token', $csrf, '/', $Ip)) } }
    }
    return [pscustomobject]@{ Client = $c; Csrf = $csrf }
}
function Send-Wdp($cli, $method, $url) {
    $req = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::new($method), $url)
    if ($cli.Csrf) { $req.Headers.Add('X-CSRF-Token', $cli.Csrf) }
    return $cli.Client.SendAsync($req).GetAwaiter().GetResult()
}

# --- 轮询设备稳定上线 ---
$cli = $null
for ($i = 0; $i -lt 14; $i++) {
    $cli = New-Client
    if ($cli) { Start-Sleep -Seconds 8; $c2 = New-Client; if ($c2) { $cli = $c2; break } }
    $cli = $null; Start-Sleep -Seconds 30
}
if (-not $cli) { Write-Host '设备未稳定上线(本轮超时)'; exit 2 }
Write-Host "=== 设备稳定上线 @ $Ip ,开始部署+验 JIT ==="

# --- 部署最新 appx(非破坏式)---
& pwsh -NoProfile -File 'E:\Apotheosis\tools\Deploy-Robust.ps1' -Ip $Ip 2>&1 | Select-Object -Last 8
Start-Sleep -Seconds 5

# --- 取已装包信息 ---
$cli = New-Client; if (-not $cli) { Write-Host '部署后失联'; exit 4 }
$pkgsJson = (Send-Wdp $cli 'GET' "$base/api/app/packagemanager/packages").Content.ReadAsStringAsync().GetAwaiter().GetResult()
$pkg = ($pkgsJson | ConvertFrom-Json).InstalledPackages | Where-Object { $_.PackageFullName -match 'EdgeHTMLReborn.Harness' } | Select-Object -First 1
if (-not $pkg) { Write-Host '未找到已装包'; exit 4 }
$pfn = $pkg.PackageFullName; $prid = $pkg.PackageRelativeId; $enc = [uri]::EscapeDataString($pfn)
Write-Host "已装: $pfn  (PRID=$prid)"

# --- 开崩溃 dump ---
Send-Wdp $cli 'POST' "$base/api/debug/dump/usermode/crashcontrol?packageFullName=$enc" | Out-Null
# 清旧 dump 计数基线
$before = ''
try { $before = (Send-Wdp $cli 'GET' "$base/api/debug/dump/usermode/dumps?packageFullName=$enc").Content.ReadAsStringAsync().GetAwaiter().GetResult() } catch {}

# --- 拉起 ---
$appB = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($prid))
$pkgB = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pfn))
$lr = Send-Wdp $cli 'POST' "$base/api/taskmanager/app?appid=$([uri]::EscapeDataString($appB))&package=$([uri]::EscapeDataString($pkgB))"
Write-Host "拉起 HTTP $([int]$lr.StatusCode)"
Start-Sleep -Seconds 22   # 等启动+JIT 初始化+首屏渲染

# --- 拉证据:jitresult.txt(JitProbe 三场景) + stage.txt(渲染诊断) + 崩溃 dump ---
function Get-LocalStateFile($cli, $fn) {
    try {
        $r = Send-Wdp $cli 'GET' "$base/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$enc&filename=$([uri]::EscapeDataString($fn))"
        if ([int]$r.StatusCode -eq 200) { return $r.Content.ReadAsStringAsync().GetAwaiter().GetResult() }
    } catch {}
    return $null
}
$cli = New-Client
$jit = Get-LocalStateFile $cli 'LocalState\jitresult.txt'
$stage = Get-LocalStateFile $cli 'LocalState\stage.txt'
$after = ''
try { $after = (Send-Wdp $cli 'GET' "$base/api/debug/dump/usermode/dumps?packageFullName=$enc").Content.ReadAsStringAsync().GetAwaiter().GetResult() } catch {}
$crashed = ($after.Length -gt $before.Length) -or ($after -match '\.dmp')

$out = @()
$out += "==== JIT 验证结果 ($Ip) ===="
$out += "包: $pfn"
$out += "拉起: HTTP $([int]$lr.StatusCode)"
$out += "崩溃dump(新): $(if($crashed){'⚠️ 有(见下)'}else{'无'})"
$out += "--- jitresult.txt(JitProbe 可执行内存三场景)---"
$out += $(if ($jit) { $jit } else { '(未取到——可能没生成/启动即崩/路径变)' })
$out += "--- stage.txt(渲染诊断 tail)---"
$out += $(if ($stage) { ($stage -split "`n" | Select-Object -Last 20) -join "`n" } else { '(未取到)' })
if ($crashed) { $out += "--- dumps 列表 ---"; $out += $after }
$out -join "`n" | Set-Content -Path $result -Encoding UTF8
Write-Host "结果已写 $result"
Write-Host ($out -join "`n")
exit 0
