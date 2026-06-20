#Requires -Version 7.0
<#  deploy-launch.ps1 — 卸载旧→装新→启动,不轮询(交互测用)。 #>
[CmdletBinding()]
param(
    [string]$Ip='192.168.3.51',
    [string]$Ver='0.1.7.23',
    [string]$Pub='x0rqga78m2mgc'
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Net.Http
$base="https://${Ip}:443"
$FN="EdgeHTMLReborn.Harness_${Ver}_arm__${Pub}"
$Praid="EdgeHTMLReborn.Harness_${Pub}!App"
$appxDir="E:\Apotheosis\harness\AppPackages\Harness\Harness_${Ver}_ARM_Test"
$appx="$appxDir\Harness_${Ver}_ARM.appx"; $cer="$appxDir\Harness_${Ver}_ARM.cer"
if(-not (Test-Path $appx)){ throw "appx 不存在: $appx" }
$h=[System.Net.Http.HttpClientHandler]::new()
$h.ServerCertificateCustomValidationCallback=[System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
$h.CookieContainer=[System.Net.CookieContainer]::new()
$cli=[System.Net.Http.HttpClient]::new($h); $cli.Timeout=[TimeSpan]::FromMinutes(8)
$script:Csrf=$null
function Sync-Csrf($resp){ $sc=$null; if($resp.Headers.TryGetValues('Set-Cookie',[ref]$sc)){ foreach($l in $sc){ if($l -match 'CSRF-Token=([^;,\s]+)'){ $script:Csrf=$Matches[1]; try{$h.CookieContainer.Add([Uri]$base,[System.Net.Cookie]::new('CSRF-Token',$script:Csrf,'/',$Ip))}catch{} } } } }
function Wdp([string]$m,[string]$p,[System.Net.Http.HttpContent]$c){ $req=[System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::new($m),"$base$p"); if($script:Csrf){$req.Headers.Add('X-CSRF-Token',$script:Csrf)}; if($c){$req.Content=$c}; $r=$cli.SendAsync($req).GetAwaiter().GetResult(); Sync-Csrf $r; return $r }
function Txt($r){ return $r.Content.ReadAsStringAsync().Result }

Sync-Csrf ($cli.GetAsync("$base/api/os/info").GetAwaiter().GetResult())
Write-Host "设备: $(Txt (Wdp GET '/api/os/info' $null))" -ForegroundColor Green

# 卸载所有旧 Harness(含本版,确保干净装)
foreach($p in ((Txt (Wdp GET '/api/app/packagemanager/packages' $null) | ConvertFrom-Json).InstalledPackages | Where-Object { $_.PackageFullName -like '*EdgeHTMLReborn.Harness*' })){
  $u=Wdp DELETE "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($p.PackageFullName))" $null
  Write-Host "卸载 $($p.PackageFullName): $([int]$u.StatusCode)"
  Start-Sleep 2
}

# 装新
$mp=[System.Net.Http.MultipartFormDataContent]::new()
foreach($f in @($appx,$cer)){ $b=[System.IO.File]::ReadAllBytes($f); $n=[System.IO.Path]::GetFileName($f); $bc=[System.Net.Http.ByteArrayContent]::new($b); $bc.Headers.ContentDisposition=[System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data'); $bc.Headers.ContentDisposition.Name="`"$n`""; $bc.Headers.ContentDisposition.FileName="`"$n`""; $bc.Headers.ContentType=[System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream'); $mp.Add($bc) }
$ins=Wdp POST "/api/app/packagemanager/package?package=$([uri]::EscapeDataString([System.IO.Path]::GetFileName($appx)))" $mp
Write-Host "安装请求: $([int]$ins.StatusCode)"
$ok=$false
for($i=0;$i -lt 40;$i++){ Start-Sleep 3; if((Txt (Wdp GET '/api/app/packagemanager/packages' $null) | ConvertFrom-Json).InstalledPackages | Where-Object { $_.PackageFullName -eq $FN }){ $ok=$true; break }; Write-Host "  …装中($i)" }
if(-not $ok){ Write-Host "❌ 安装未确认,终止" -ForegroundColor Red; exit 1 }
Write-Host "✅ 已安装 $FN" -ForegroundColor Green

# 启动
$aid=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Praid)); $pkg=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($FN))
$lr=Wdp POST "/api/taskmanager/app?appid=$([uri]::EscapeDataString($aid))&package=$([uri]::EscapeDataString($pkg))" $null
Write-Host "✅ 启动: $([int]$lr.StatusCode) — 设备上应已起来,可以开始测了" -ForegroundColor Green
