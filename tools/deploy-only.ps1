#Requires -Version 7.0
# 仅部署(卸载+安装),带连接重试(设备间歇休眠)。无启动/抓取。
param([string]$Ip='192.168.3.51',[string]$Ver='0.1.7.16',[string]$Pub='x0rqga78m2mgc')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Net.Http
$base="https://${Ip}:443"
$FN="EdgeHTMLReborn.Harness_${Ver}_arm__${Pub}"
$dir="E:\Apotheosis\harness\AppPackages\Harness\Harness_${Ver}_ARM_Test"
$appx="$dir\Harness_${Ver}_ARM.appx"; $cer="$dir\Harness_${Ver}_ARM.cer"
$h=[System.Net.Http.HttpClientHandler]::new()
$h.ServerCertificateCustomValidationCallback=[System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
$h.CookieContainer=[System.Net.CookieContainer]::new()
$cli=[System.Net.Http.HttpClient]::new($h); $cli.Timeout=[TimeSpan]::FromMinutes(8)
$script:Csrf=$null
function Sync($r){ $sc=$null; if($r.Headers.TryGetValues('Set-Cookie',[ref]$sc)){ foreach($l in $sc){ if($l -match 'CSRF-Token=([^;,\s]+)'){ $script:Csrf=$Matches[1]; try{$h.CookieContainer.Add([Uri]$base,[System.Net.Cookie]::new('CSRF-Token',$script:Csrf,'/',$Ip))}catch{} } } } }
# 带重试的发送(连接断=休眠,等它醒)
function Wdp($m,$p,$c){
  for($t=0;$t -lt 20;$t++){
    try {
      $req=[System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::new($m),"$base$p")
      if($script:Csrf){$req.Headers.Add('X-CSRF-Token',$script:Csrf)}
      if($c){$req.Content=$c}
      $r=$cli.SendAsync($req).GetAwaiter().GetResult(); Sync $r; return $r
    } catch { Write-Host "  (重试 ${t}: 连接断,等设备唤醒)"; Start-Sleep 4 }
  }
  throw "WDP $m $p 连续失败"
}
function Wake(){ for($t=0;$t -lt 30;$t++){ try{ $cli.GetAsync("$base/api/os/info").GetAwaiter().GetResult()|Out-Null; return }catch{ Start-Sleep 3 } }; throw "设备唤不醒" }
Wake; Sync ($cli.GetAsync("$base/api/os/info").GetAwaiter().GetResult())
Write-Host "设备在线"
foreach($p in ((($cli.GetAsync("$base/api/app/packagemanager/packages").GetAwaiter().GetResult().Content.ReadAsStringAsync().Result)|ConvertFrom-Json).InstalledPackages | Where-Object {$_.PackageFullName -like '*EdgeHTMLReborn.Harness*'})){ $u=Wdp DELETE "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($p.PackageFullName))" $null; Write-Host "卸载 $($p.PackageFullName): $([int]$u.StatusCode)" }
# 安装:含 body,失败需重建 multipart,故包在循环里整体重试
$ins=$null
for($t=0;$t -lt 8;$t++){
  try {
    Wake
    $mp=[System.Net.Http.MultipartFormDataContent]::new()
    foreach($f in @($appx,$cer)){ $b=[System.IO.File]::ReadAllBytes($f); $n=[System.IO.Path]::GetFileName($f); $bc=[System.Net.Http.ByteArrayContent]::new($b); $bc.Headers.ContentDisposition=[System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data'); $bc.Headers.ContentDisposition.Name="`"$n`""; $bc.Headers.ContentDisposition.FileName="`"$n`""; $bc.Headers.ContentType=[System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream'); $mp.Add($bc) }
    $req=[System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post,"$base/api/app/packagemanager/package?package=$([uri]::EscapeDataString([System.IO.Path]::GetFileName($appx)))")
    if($script:Csrf){$req.Headers.Add('X-CSRF-Token',$script:Csrf)}
    $req.Content=$mp
    $ins=$cli.SendAsync($req).GetAwaiter().GetResult(); Sync $ins
    Write-Host "安装请求: $([int]$ins.StatusCode)"; break
  } catch { Write-Host "  安装重试 $t(连接断)"; Start-Sleep 4 }
}
$ok=$false
for($i=0;$i -lt 50;$i++){ Start-Sleep 3; try{ $now=(($cli.GetAsync("$base/api/app/packagemanager/packages").GetAwaiter().GetResult().Content.ReadAsStringAsync().Result)|ConvertFrom-Json).InstalledPackages; if($now|Where-Object{$_.PackageFullName -eq $FN}){ $ok=$true; break } }catch{} }
Write-Host ($(if($ok){"✅ 已安装 $FN"}else{"❌ 安装未确认"}))
