#Requires -Version 7.0
# Verify-GPU-Detached.ps1 — 脱离式常驻 GPU 探针验证。设备一稳定上线就部署最新 appx → 拉起 → 拉 gpuprobe.txt + stage.txt
# + 查崩溃,把结论写 gpu-verify-result.txt,状态写 gpu-verify-status.txt(WATCHING/VERIFYING/DONE)。验完 exit 0。
param([string]$Ip = '192.168.3.51')
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Net.Http
$base = "https://${Ip}:443"
$result = 'E:\Apotheosis\gpu-verify-result.txt'
$status = 'E:\Apotheosis\gpu-verify-status.txt'
function Mark($s) { Set-Content -Path $status -Value "$s $(Get-Date -Format 'MM-dd HH:mm:ss')" -Encoding UTF8 }
function New-Client {
    $h = [System.Net.Http.HttpClientHandler]::new()
    $h.ServerCertificateCustomValidationCallback = [System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
    $h.CookieContainer = [System.Net.CookieContainer]::new()
    $c = [System.Net.Http.HttpClient]::new($h); $c.Timeout = [TimeSpan]::FromSeconds(20)
    $r0 = $null; try { $r0 = $c.GetAsync("$base/api/os/info").GetAwaiter().GetResult() } catch { return $null }
    if (-not $r0 -or [int]$r0.StatusCode -ne 200) { return $null }
    $csrf = $null; $sc = $null
    if ($r0.Headers.TryGetValues('Set-Cookie', [ref]$sc)) { foreach ($l in $sc) { if ($l -match 'CSRF-Token=([^;,\s]+)') { $csrf = $Matches[1]; $h.CookieContainer.Add([Uri]$base, [System.Net.Cookie]::new('CSRF-Token', $csrf, '/', $Ip)) } } }
    return [pscustomobject]@{ Client = $c; Csrf = $csrf }
}
function Send-Wdp($cli, $m, $u) { $req = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::new($m), $u); if ($cli.Csrf) { $req.Headers.Add('X-CSRF-Token', $cli.Csrf) }; return $cli.Client.SendAsync($req).GetAwaiter().GetResult() }
function Get-LS($cli, $enc, $fn) { try { $r = Send-Wdp $cli 'GET' "$base/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$enc&filename=$([uri]::EscapeDataString($fn))"; if ([int]$r.StatusCode -eq 200) { return $r.Content.ReadAsStringAsync().GetAwaiter().GetResult() } } catch {}; return $null }

Mark 'WATCHING'
while ($true) {
    $cli = New-Client
    if (-not $cli) { Mark 'WATCHING'; Start-Sleep -Seconds 15; continue }
    Start-Sleep -Seconds 8; $cli2 = New-Client; if (-not $cli2) { Start-Sleep -Seconds 12; continue }
    $cli = $cli2
    Mark 'VERIFYING'
    & pwsh -NoProfile -File 'E:\Apotheosis\tools\Deploy-Robust.ps1' -Ip $Ip 2>&1 | Out-Null
    Start-Sleep -Seconds 5
    $cli = New-Client; if (-not $cli) { Mark 'RETRY'; Start-Sleep -Seconds 15; continue }
    $pkg = ($(Send-Wdp $cli 'GET' "$base/api/app/packagemanager/packages").Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json).InstalledPackages | Where-Object { $_.PackageFullName -match 'EdgeHTMLReborn.Harness' } | Sort-Object { $_.Version.Major*1000+$_.Version.Minor*100+$_.Version.Build } -Descending | Select-Object -First 1
    if (-not $pkg) { Mark 'RETRY(无包)'; Start-Sleep -Seconds 15; continue }
    $pfn = $pkg.PackageFullName; $prid = $pkg.PackageRelativeId; $enc = [uri]::EscapeDataString($pfn)
    Send-Wdp $cli 'POST' "$base/api/debug/dump/usermode/crashcontrol?packageFullName=$enc" | Out-Null
    $appB = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($prid)); $pkgB = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pfn))
    $lr = Send-Wdp $cli 'POST' "$base/api/taskmanager/app?appid=$([uri]::EscapeDataString($appB))&package=$([uri]::EscapeDataString($pkgB))"
    Start-Sleep -Seconds 20
    $cli = New-Client
    $gpu = Get-LS $cli $enc 'LocalState\gpuprobe.txt'
    $stage = Get-LS $cli $enc 'LocalState\stage.txt'
    $dm = ''; try { $dm = (Send-Wdp $cli 'GET' "$base/api/debug/dump/usermode/dumps?packageFullName=$enc").Content.ReadAsStringAsync().GetAwaiter().GetResult() } catch {}
    $crashed = $dm -match [Regex]::Escape($pkg.Version.ToString())
    $out = @("==== GPU 探针验证 ($Ip) $(Get-Date -Format 'MM-dd HH:mm:ss') ====", "包: $pfn", "拉起: HTTP $([int]$lr.StatusCode)", "崩溃: $(if($crashed){'⚠ 有本版崩溃'}else{'无'})", "--- gpuprobe.txt ---", $(if ($gpu) { $gpu } else { '(未取到——启动即崩/线程没到写文件/EGL 初始化前崩)' }), "--- stage.txt tail ---", $(if ($stage) { ($stage -split "`n" | Select-Object -Last 10) -join "`n" } else { '(无)' }))
    ($out -join "`n") | Set-Content -Path $result -Encoding UTF8
    Mark 'DONE'
    exit 0
}
