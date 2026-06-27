#Requires -Version 7.0
<#  pull-file.ps1 — 经 WDP 从 app LocalState 拉一个文本文件打印出来。 #>
[CmdletBinding()]
param(
    [string]$Ip='192.168.3.51',
    [string]$Ver='0.1.7.25',
    [string]$Pub='x0rqga78m2mgc',
    [Parameter(Mandatory)][string]$File
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Net.Http
$base="https://${Ip}:443"
$FN="EdgeHTMLReborn.Harness_${Ver}_arm__${Pub}"
$FNe=[uri]::EscapeDataString($FN)
$h=[System.Net.Http.HttpClientHandler]::new()
$h.ServerCertificateCustomValidationCallback=[System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
$h.CookieContainer=[System.Net.CookieContainer]::new()
$cli=[System.Net.Http.HttpClient]::new($h); $cli.Timeout=[TimeSpan]::FromSeconds(30)
$script:Csrf=$null
function Sync-Csrf($resp){ $sc=$null; if($resp.Headers.TryGetValues('Set-Cookie',[ref]$sc)){ foreach($l in $sc){ if($l -match 'CSRF-Token=([^;,\s]+)'){ $script:Csrf=$Matches[1]; try{$h.CookieContainer.Add([Uri]$base,[System.Net.Cookie]::new('CSRF-Token',$script:Csrf,'/',$Ip))}catch{} } } } }
Sync-Csrf ($cli.GetAsync("$base/api/os/info").GetAwaiter().GetResult())
$req=[System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Get,"$base/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$FNe&path=%5CLocalState&filename=$([uri]::EscapeDataString($File))")
if($script:Csrf){$req.Headers.Add('X-CSRF-Token',$script:Csrf)}
$r=$cli.SendAsync($req).GetAwaiter().GetResult()
Write-Host "HTTP $([int]$r.StatusCode) for $File"
if([int]$r.StatusCode -eq 200){ Write-Host "===== $File =====" -ForegroundColor Green; Write-Host ($r.Content.ReadAsStringAsync().Result) }
else { Write-Host "(文件不存在或读取失败)" -ForegroundColor Yellow }
