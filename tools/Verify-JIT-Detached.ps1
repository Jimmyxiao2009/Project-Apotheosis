#Requires -Version 7.0
# Verify-JIT-Detached.ps1 — 脱离式常驻 JIT 验证。独立进程永久轮询设备,稳定上线即:
#   部署最新 appx → 开崩溃 dump → 拉起 → 等 → 拉 jitresult.txt/stage.txt + 查崩溃 dump → 写结果。
# 状态写 jit-verify-status.txt(WATCHING/VERIFYING/DONE/RETRY);详情写 jit-verify-result.txt。
# 由 Start-Process 脱离启动,不受 10 分钟后台上限约束,可守整夜;验证成功后 exit 0。
param([string]$Ip = '192.168.3.51')
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Net.Http
$base = "https://${Ip}:443"
$result = 'E:\Apotheosis\jit-verify-result.txt'
$status = 'E:\Apotheosis\jit-verify-status.txt'
function Mark($s) { Set-Content -Path $status -Value "$s $(Get-Date -Format 'MM-dd HH:mm:ss')" -Encoding UTF8 }

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
function Get-LSFile($cli, $enc, $fn) {
    try {
        $r = Send-Wdp $cli 'GET' "$base/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$enc&filename=$([uri]::EscapeDataString($fn))"
        if ([int]$r.StatusCode -eq 200) { return $r.Content.ReadAsStringAsync().GetAwaiter().GetResult() }
    } catch {}
    return $null
}

Mark 'WATCHING'
$round = 0
while ($true) {
    $cli = New-Client
    if (-not $cli) { Mark 'WATCHING'; Start-Sleep -Seconds 15; continue }
    Start-Sleep -Seconds 8; $cli2 = New-Client          # 二次确认稳定
    if (-not $cli2) { Start-Sleep -Seconds 12; continue }
    $cli = $cli2; $round++
    Mark "VERIFYING(round$round)"

    & pwsh -NoProfile -File 'E:\Apotheosis\tools\Deploy-Robust.ps1' -Ip $Ip 2>&1 | Out-Null
    Start-Sleep -Seconds 5
    $cli = New-Client; if (-not $cli) { Mark "RETRY(deploy后失联)"; Start-Sleep -Seconds 15; continue }
    $pkgsJson = (Send-Wdp $cli 'GET' "$base/api/app/packagemanager/packages").Content.ReadAsStringAsync().GetAwaiter().GetResult()
    $pkg = ($pkgsJson | ConvertFrom-Json).InstalledPackages | Where-Object { $_.PackageFullName -match 'EdgeHTMLReborn.Harness' } | Select-Object -First 1
    if (-not $pkg) { Mark "RETRY(未装上包)"; Start-Sleep -Seconds 15; continue }
    $pfn = $pkg.PackageFullName; $prid = $pkg.PackageRelativeId; $enc = [uri]::EscapeDataString($pfn)

    Send-Wdp $cli 'POST' "$base/api/debug/dump/usermode/crashcontrol?packageFullName=$enc" | Out-Null
    $before = ''; try { $before = (Send-Wdp $cli 'GET' "$base/api/debug/dump/usermode/dumps?packageFullName=$enc").Content.ReadAsStringAsync().GetAwaiter().GetResult() } catch {}
    $appB = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($prid))
    $pkgB = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pfn))
    $lr = Send-Wdp $cli 'POST' "$base/api/taskmanager/app?appid=$([uri]::EscapeDataString($appB))&package=$([uri]::EscapeDataString($pkgB))"
    $launchCode = [int]$lr.StatusCode
    Start-Sleep -Seconds 22

    $cli = New-Client
    $jit = $null; $stage = $null; $after = ''
    if ($cli) {
        $jit = Get-LSFile $cli $enc 'LocalState\jitresult.txt'
        $stage = Get-LSFile $cli $enc 'LocalState\stage.txt'
        try { $after = (Send-Wdp $cli 'GET' "$base/api/debug/dump/usermode/dumps?packageFullName=$enc").Content.ReadAsStringAsync().GetAwaiter().GetResult() } catch {}
    }
    $crashed = ($after.Length -gt $before.Length) -or ($after -match '\.dmp')

    $out = @()
    $out += "==== JIT 验证结果 ($Ip)  $(Get-Date -Format 'MM-dd HH:mm:ss') ===="
    $out += "包: $pfn"
    $out += "拉起: HTTP $launchCode"
    $out += "崩溃dump(新): $(if($crashed){'有(见下)'}else{'无'})"
    $out += "--- jitresult.txt(JitProbe 可执行内存三场景)---"
    $out += $(if ($jit) { $jit } else { '(未取到——没生成/启动即崩/路径变)' })
    $out += "--- stage.txt(渲染诊断 tail)---"
    $out += $(if ($stage) { ($stage -split "`n" | Select-Object -Last 24) -join "`n" } else { '(未取到)' })
    if ($crashed) { $out += "--- dumps ---"; $out += $after }
    ($out -join "`n") | Set-Content -Path $result -Encoding UTF8
    Mark 'DONE'
    exit 0
}
