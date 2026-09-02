#Requires -Version 7.0
# Deploy-Robust.ps1 — 非破坏式部署最新 appx 到设备:不卸载(靠版本号递增做更新覆盖),
# 失败也保留旧版。glob 最高版本 appx,HttpClient 多段上传,轮询安装状态,动态查 FullName 开崩溃收集。
# Apotheosis: $Root = repo root, defaults to the parent of tools\ so the script works from any checkout location.
param([string]$Ip = '192.168.3.51', [int]$TimeoutMin = 8, [string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http
$base = "https://${Ip}:443"

# 取最高版本的 appx + 同目录 cer
$appx = Get-ChildItem (Join-Path $Root 'harness\AppPackages\Harness') -Recurse -Filter 'Harness_*_ARM.appx' |
    Sort-Object { [version]([regex]::Match($_.Name, '_(\d+\.\d+\.\d+\.\d+)_').Groups[1].Value) } -Descending |
    Select-Object -First 1
if (-not $appx) { Write-Host "找不到 appx"; exit 1 }
$cer = Get-ChildItem $appx.DirectoryName -Filter '*.cer' | Select-Object -First 1
Write-Host "部署: $($appx.Name)"

$h = [System.Net.Http.HttpClientHandler]::new()
$h.ServerCertificateCustomValidationCallback = [System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
$h.CookieContainer = [System.Net.CookieContainer]::new()
$cli = [System.Net.Http.HttpClient]::new($h); $cli.Timeout = [TimeSpan]::FromMinutes($TimeoutMin)
$csrf = $null
function Sync($r) { $sc = $null; if ($r.Headers.TryGetValues('Set-Cookie', [ref]$sc)) { foreach ($l in $sc) { if ($l -match 'CSRF-Token=([^;,\s]+)') { $script:csrf = $Matches[1]; try { $h.CookieContainer.Add([Uri]$base, [System.Net.Cookie]::new('CSRF-Token', $script:csrf, '/', $Ip)) } catch {} } } } }
function GET($p) { $r = $cli.GetAsync("$base$p").GetAwaiter().GetResult(); Sync $r; return $r.Content.ReadAsStringAsync().Result }

Sync ($cli.GetAsync("$base/api/os/info").GetAwaiter().GetResult())
Write-Host "设备: $(GET '/api/os/info')"

# 安装(非破坏:不卸载,版本递增做更新)。上传失败立即重试(连接可能很快恢复),最多 4 次。
$installSent = $false
for ($try = 1; $try -le 4 -and -not $installSent; $try++) {
    $form = [System.Net.Http.MultipartFormDataContent]::new()   # 流会被消费,每次重建
    foreach ($f in @($appx.FullName, $cer.FullName)) {
        $name = [IO.Path]::GetFileName($f)
        $sc = [System.Net.Http.StreamContent]::new([IO.File]::OpenRead($f))
        $sc.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream')
        $cd = [System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data'); $cd.Name = "`"$name`""; $cd.FileName = "`"$name`""
        $sc.Headers.ContentDisposition = $cd; $form.Add($sc)
    }
    $req = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, "$base/api/app/packagemanager/package?package=$([uri]::EscapeDataString($appx.Name))")
    if ($csrf) { $req.Headers.Add('X-CSRF-Token', $csrf) }
    $req.Content = $form
    Write-Host "上传安装中(44MB,尝试 $try/4)…"
    try {
        $ins = $cli.SendAsync($req).GetAwaiter().GetResult(); Sync $ins
        Write-Host "安装请求: $([int]$ins.StatusCode) $($ins.Content.ReadAsStringAsync().Result)"
        $installSent = $true
    } catch {
        Write-Host "上传中断(尝试 $try): $($_.Exception.GetBaseException().Message)" -ForegroundColor Yellow
        Start-Sleep -Seconds 3
    } finally { $form.Dispose() }
}
if (-not $installSent) { Write-Host "上传 4 次均失败,放弃本轮" -ForegroundColor Red; exit 5 }

for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Seconds 2
    $st = $cli.GetAsync("$base/api/app/packagemanager/state").GetAwaiter().GetResult(); Sync $st
    $b = $st.Content.ReadAsStringAsync().Result
    if ([int]$st.StatusCode -eq 204 -or -not $b) { Write-Host "安装结束(无进行中任务)"; break }
    if ($b -match '"Success"\s*:\s*true') { Write-Host "安装完成 ✓"; break }
    if ($b -match '"Success"\s*:\s*false' -and $b -notmatch 'InProgress|Installing') { Write-Host "安装失败: $b" -ForegroundColor Red; exit 3 }
}

# 查实际 FullName + 开崩溃收集。非破坏式更新时,装好后包列表查询会与"包替换"竞态(瞬时查不到)→ 重试几次。
$pkgs = $null
for ($q = 0; $q -lt 6 -and -not $pkgs; $q++) {
    try { $pkgs = (GET '/api/app/packagemanager/packages' | ConvertFrom-Json).InstalledPackages | Where-Object { $_.PackageFullName -match 'EdgeHTMLReborn.Harness' } } catch {}
    if (-not $pkgs) { Start-Sleep -Seconds 3 }
}
if (-not $pkgs) { Write-Host "⚠ 安装后未查到 Harness 包(查询多次仍空;安装请求已 202,实际可能已装)" -ForegroundColor Yellow; exit 3 }
$target = $pkgs | Sort-Object { [version]("$($_.Version.Major).$($_.Version.Minor).$($_.Version.Build).$($_.Version.Revision)") } -Descending | Select-Object -First 1
$FN = $target.PackageFullName
Write-Host "已装: $FN" -ForegroundColor Green
$req2 = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, "$base/api/debug/dump/usermode/crashcontrol?packageFullName=$([uri]::EscapeDataString($FN))")
if ($csrf) { $req2.Headers.Add('X-CSRF-Token', $csrf) }
Write-Host "崩溃收集: $([int]$cli.SendAsync($req2).GetAwaiter().GetResult().StatusCode)"

# 启动 App(PRAID 由 FullName 推导:<Name>_<PublisherHash>!App)
$praid = $target.PackageRelativeId
if (-not $praid) { $praid = "$($FN.Split('_')[0])_$($FN.Split('__')[-1])!App" }
$aid = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($praid))
$pkg = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($FN))
$lreq = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, "$base/api/taskmanager/app?appid=$([uri]::EscapeDataString($aid))&package=$([uri]::EscapeDataString($pkg))")
if ($csrf) { $lreq.Headers.Add('X-CSRF-Token', $csrf) }
Write-Host "启动: $([int]$cli.SendAsync($lreq).GetAwaiter().GetResult().StatusCode) ($praid)"
Write-Host "=== 部署成功 ==="
exit 0
