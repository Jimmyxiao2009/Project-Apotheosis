#Requires -Version 7.0
<#  auto-diag2.ps1 — 卸载旧→装新→启动(内置 URL 自动诊断)→轮询拉 autodump.txt。无上传(URL 已编进 app)。 #>
[CmdletBinding()]
param(
    [string]$Ip='192.168.3.51',
    [string]$Ver='0.1.7.10',
    [string]$Pub='x0rqga78m2mgc',
    [int]$UrlCount=3,
    [string]$OutDir='E:\Apotheosis\crash'
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Net.Http
$base="https://${Ip}:443"
$FN="EdgeHTMLReborn.Harness_${Ver}_arm__${Pub}"
$Praid="EdgeHTMLReborn.Harness_${Pub}!App"
$appxDir="E:\Apotheosis\harness\AppPackages\Harness\Harness_${Ver}_ARM_Test"
$appx="$appxDir\Harness_${Ver}_ARM.appx"; $cer="$appxDir\Harness_${Ver}_ARM.cer"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$h=[System.Net.Http.HttpClientHandler]::new()
$h.ServerCertificateCustomValidationCallback=[System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
$h.CookieContainer=[System.Net.CookieContainer]::new()
$cli=[System.Net.Http.HttpClient]::new($h); $cli.Timeout=[TimeSpan]::FromMinutes(8)
$script:Csrf=$null
function Sync-Csrf($resp){ $sc=$null; if($resp.Headers.TryGetValues('Set-Cookie',[ref]$sc)){ foreach($l in $sc){ if($l -match 'CSRF-Token=([^;,\s]+)'){ $script:Csrf=$Matches[1]; try{$h.CookieContainer.Add([Uri]$base,[System.Net.Cookie]::new('CSRF-Token',$script:Csrf,'/',$Ip))}catch{} } } } }
function Wdp([string]$m,[string]$p,[System.Net.Http.HttpContent]$c){ $req=[System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::new($m),"$base$p"); if($script:Csrf){$req.Headers.Add('X-CSRF-Token',$script:Csrf)}; if($c){$req.Content=$c}; $r=$cli.SendAsync($req).GetAwaiter().GetResult(); Sync-Csrf $r; return $r }
function Txt($r){ return $r.Content.ReadAsStringAsync().Result }
$FNe=[uri]::EscapeDataString($FN)

Sync-Csrf ($cli.GetAsync("$base/api/os/info").GetAwaiter().GetResult())
Write-Host "设备: $(Txt (Wdp GET '/api/os/info' $null))" -ForegroundColor Green
foreach($p in ((Txt (Wdp GET '/api/app/packagemanager/packages' $null) | ConvertFrom-Json).InstalledPackages | Where-Object { $_.PackageFullName -like '*EdgeHTMLReborn.Harness*' })){
  $u=Wdp DELETE "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($p.PackageFullName))" $null
  Write-Host "卸载 $($p.PackageFullName): $([int]$u.StatusCode)"
}
$mp=[System.Net.Http.MultipartFormDataContent]::new()
foreach($f in @($appx,$cer)){ $b=[System.IO.File]::ReadAllBytes($f); $n=[System.IO.Path]::GetFileName($f); $bc=[System.Net.Http.ByteArrayContent]::new($b); $bc.Headers.ContentDisposition=[System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data'); $bc.Headers.ContentDisposition.Name="`"$n`""; $bc.Headers.ContentDisposition.FileName="`"$n`""; $bc.Headers.ContentType=[System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream'); $mp.Add($bc) }
$ins=Wdp POST "/api/app/packagemanager/package?package=$([uri]::EscapeDataString([System.IO.Path]::GetFileName($appx)))" $mp
Write-Host "安装请求: $([int]$ins.StatusCode)"
$ok=$false
for($i=0;$i -lt 40;$i++){ Start-Sleep 3; if((Txt (Wdp GET '/api/app/packagemanager/packages' $null) | ConvertFrom-Json).InstalledPackages | Where-Object { $_.PackageFullName -eq $FN }){ $ok=$true; break }; Write-Host "  …装中($i)" }
Write-Host ($(if($ok){"已安装 $FN"}else{"安装未确认"})) -ForegroundColor ($(if($ok){'Green'}else{'Yellow'}))
$aid=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Praid)); $pkg=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($FN))
$lr=Wdp POST "/api/taskmanager/app?appid=$([uri]::EscapeDataString($aid))&package=$([uri]::EscapeDataString($pkg))" $null
Write-Host "启动: $([int]$lr.StatusCode)" -ForegroundColor Green
$got=''
for($i=0;$i -lt 36;$i++){
  Start-Sleep 5
  $g=Wdp GET "/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$FNe&path=%5CLocalState&filename=autodump.txt" $null
  if([int]$g.StatusCode -eq 200){ $got=Txt $g; $segs=([regex]::Matches($got,'########## URL:')).Count; Write-Host "  autodump $($got.Length)B 段=$segs/$UrlCount"; if($segs -ge $UrlCount -and $got -match 'usesCompositing'){ break } }
  else { Write-Host "  等…($i) $([int]$g.StatusCode)" }
}
if($got){ $out="$OutDir\autodump-$Ver.txt"; [System.IO.File]::WriteAllText($out,$got); Write-Host "✅ $out ($($got.Length)B)" -ForegroundColor Green } else { Write-Host "❌ 没拿到" -ForegroundColor Red }
