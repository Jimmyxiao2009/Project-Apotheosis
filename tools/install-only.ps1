#Requires -Version 7.0
# 只安装(不先卸载,缩短无 app 窗口),带唤醒+整体重试。装完启动。
param([string]$Ip='192.168.3.51',[string]$Ver='0.1.7.22',[string]$Pub='x0rqga78m2mgc')
$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.Net.Http
$base="https://${Ip}:443"; $FN="EdgeHTMLReborn.Harness_${Ver}_arm__${Pub}"; $Praid="EdgeHTMLReborn.Harness_${Pub}!App"
$dir="E:\Apotheosis\harness\AppPackages\Harness\Harness_${Ver}_ARM_Test"; $appx="$dir\Harness_${Ver}_ARM.appx"; $cer="$dir\Harness_${Ver}_ARM.cer"
$h=[System.Net.Http.HttpClientHandler]::new(); $h.ServerCertificateCustomValidationCallback=[System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
$h.CookieContainer=[System.Net.CookieContainer]::new(); $cli=[System.Net.Http.HttpClient]::new($h); $cli.Timeout=[TimeSpan]::FromMinutes(8)
$script:Csrf=$null
function Sync($r){ $sc=$null; if($r.Headers.TryGetValues('Set-Cookie',[ref]$sc)){ foreach($l in $sc){ if($l -match 'CSRF-Token=([^;,\s]+)'){ $script:Csrf=$Matches[1]; try{$h.CookieContainer.Add([Uri]$base,[System.Net.Cookie]::new('CSRF-Token',$script:Csrf,'/',$Ip))}catch{} } } } }
function Wake(){ for($t=0;$t -lt 40;$t++){ try{ $r=$cli.GetAsync("$base/api/os/info").GetAwaiter().GetResult(); Sync $r; return $true }catch{ Start-Sleep 4 } }; return $false }
if(-not (Wake)){ Write-Host "❌ 设备唤不醒"; exit 1 }
Write-Host "设备在线"
$pkgsNow={ (($cli.GetAsync("$base/api/app/packagemanager/packages").GetAwaiter().GetResult().Content.ReadAsStringAsync().Result)|ConvertFrom-Json).InstalledPackages }
# 已装则跳过
if(& $pkgsNow | Where-Object {$_.PackageFullName -eq $FN}){ Write-Host "已装 $FN,跳过安装" }
else {
  $done=$false
  for($t=0;$t -lt 10 -and -not $done;$t++){
    if(-not (Wake)){ continue }
    try {
      $mp=[System.Net.Http.MultipartFormDataContent]::new()
      foreach($fl in @($appx,$cer)){ $b=[System.IO.File]::ReadAllBytes($fl); $n=[System.IO.Path]::GetFileName($fl); $bc=[System.Net.Http.ByteArrayContent]::new($b); $bc.Headers.ContentDisposition=[System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data'); $bc.Headers.ContentDisposition.Name="`"$n`""; $bc.Headers.ContentDisposition.FileName="`"$n`""; $bc.Headers.ContentType=[System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream'); $mp.Add($bc) }
      $req=[System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post,"$base/api/app/packagemanager/package?package=$([uri]::EscapeDataString([System.IO.Path]::GetFileName($appx)))")
      if($script:Csrf){$req.Headers.Add('X-CSRF-Token',$script:Csrf)}; $req.Content=$mp
      $resp=$cli.SendAsync($req).GetAwaiter().GetResult(); Sync $resp
      Write-Host "安装请求($t): $([int]$resp.StatusCode)"
      for($i=0;$i -lt 40;$i++){ Start-Sleep 3; try{ if(& $pkgsNow | Where-Object {$_.PackageFullName -eq $FN}){ $done=$true; break } }catch{} }
    } catch { Write-Host "  安装重试 $t(连接断)"; Start-Sleep 5 }
  }
  Write-Host ($(if($done){"✅ 已安装 $FN"}else{"❌ 安装未确认"}))
  if(-not $done){ exit 1 }
}
# 启动
$aid=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Praid)); $pkg=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($FN))
try{ $req=[System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post,"$base/api/taskmanager/app?appid=$([uri]::EscapeDataString($aid))&package=$([uri]::EscapeDataString($pkg))"); if($script:Csrf){$req.Headers.Add('X-CSRF-Token',$script:Csrf)}; $lr=$cli.SendAsync($req).GetAwaiter().GetResult(); Write-Host "启动: $([int]$lr.StatusCode)" }catch{ Write-Host "启动异常(可能已在跑)" }
