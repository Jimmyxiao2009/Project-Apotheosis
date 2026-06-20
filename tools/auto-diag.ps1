#Requires -Version 7.0
<#  auto-diag.ps1 — 全自动:卸载旧→装 0.1.7.9→推 autodiag.txt(URL列表)到 LocalState→启动→轮询拉 autodump.txt。
    走 WDP REST + HttpClient(手搓 multipart + 从 Set-Cookie 抠 CSRF)。无需任何 UI 点按。 #>
[CmdletBinding()]
param(
    [string]$Ip='192.168.3.51',
    [string]$Ver='0.1.7.9',
    [string]$Pub='x0rqga78m2mgc',
    [string]$OutDir='E:\Apotheosis\crash',
    [string[]]$Urls=@('https://example.com','https://cn.bing.com','https://cn.bing.com/search?q=hello')
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Net.Http
$base="https://${Ip}:443"
$FN="EdgeHTMLReborn.Harness_${Ver}_arm__${Pub}"
$Praid="EdgeHTMLReborn.Harness_${Pub}!App"
$appxDir="E:\Apotheosis\harness\AppPackages\Harness\Harness_${Ver}_ARM_Test"
$appx="$appxDir\Harness_${Ver}_ARM.appx"
$cer ="$appxDir\Harness_${Ver}_ARM.cer"
New-Item -ItemType Directory -Force $OutDir | Out-Null

$h=[System.Net.Http.HttpClientHandler]::new()
$h.ServerCertificateCustomValidationCallback=[System.Net.Http.HttpClientHandler]::DangerousAcceptAnyServerCertificateValidator
$h.CookieContainer=[System.Net.CookieContainer]::new()
$cli=[System.Net.Http.HttpClient]::new($h); $cli.Timeout=[TimeSpan]::FromMinutes(8)
$script:Csrf=$null
function Sync-Csrf($resp){ $sc=$null; if($resp.Headers.TryGetValues('Set-Cookie',[ref]$sc)){ foreach($l in $sc){ if($l -match 'CSRF-Token=([^;,\s]+)'){ $script:Csrf=$Matches[1]; try{$h.CookieContainer.Add([Uri]$base,[System.Net.Cookie]::new('CSRF-Token',$script:Csrf,'/',$Ip))}catch{} } } } }
function Wdp([string]$method,[string]$path,[System.Net.Http.HttpContent]$content){
  $req=[System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::new($method),"$base$path")
  if($script:Csrf){ $req.Headers.Add('X-CSRF-Token',$script:Csrf) }
  if($content){ $req.Content=$content }
  $r=$cli.SendAsync($req).GetAwaiter().GetResult(); Sync-Csrf $r; return $r
}
function Txt($r){ return $r.Content.ReadAsStringAsync().Result }

# 0) CSRF
Sync-Csrf ($cli.GetAsync("$base/api/os/info").GetAwaiter().GetResult())
Write-Host "设备: $(Txt (Wdp GET '/api/os/info' $null))" -ForegroundColor Green

# 1) 卸载所有已装的 EdgeHTMLReborn.Harness
$pkgs=(Txt (Wdp GET '/api/app/packagemanager/packages' $null) | ConvertFrom-Json).InstalledPackages
foreach($p in ($pkgs | Where-Object { $_.PackageFullName -like '*EdgeHTMLReborn.Harness*' })){
  $u=Wdp DELETE "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($p.PackageFullName))" $null
  Write-Host "卸载 $($p.PackageFullName): $([int]$u.StatusCode)"
}

# 2) 安装 0.1.7.9(multipart: appx + cer)
$mp=[System.Net.Http.MultipartFormDataContent]::new()
foreach($f in @($appx,$cer)){
  $bytes=[System.IO.File]::ReadAllBytes($f); $name=[System.IO.Path]::GetFileName($f)
  $bc=[System.Net.Http.ByteArrayContent]::new($bytes)
  $bc.Headers.ContentDisposition=[System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data')
  $bc.Headers.ContentDisposition.Name="`"$name`""; $bc.Headers.ContentDisposition.FileName="`"$name`""
  $bc.Headers.ContentType=[System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream')
  $mp.Add($bc)
}
$ins=Wdp POST "/api/app/packagemanager/package?package=$([uri]::EscapeDataString([System.IO.Path]::GetFileName($appx)))" $mp
Write-Host "安装请求: $([int]$ins.StatusCode)"
# 轮询安装完成(packages 出现 0.1.7.9)
$ok=$false
for($i=0;$i -lt 40;$i++){
  Start-Sleep -Seconds 3
  try { $st=Txt (Wdp GET '/api/app/packagemanager/state' $null) } catch { $st='' }
  $now=(Txt (Wdp GET '/api/app/packagemanager/packages' $null) | ConvertFrom-Json).InstalledPackages
  if($now | Where-Object { $_.PackageFullName -eq $FN }){ $ok=$true; break }
  Write-Host "  …装中($i) state=$st"
}
if(-not $ok){ Write-Host "安装未确认,继续尝试" -ForegroundColor Yellow }
else { Write-Host "已安装 $FN" -ForegroundColor Green }

# 3) 推 autodiag.txt 到 LocalState(URL 每行一个)
$content=($Urls -join "`n")+"`n"
$FNe=[uri]::EscapeDataString($FN)
function Upload-LocalState($fname,$text){
  $mp2=[System.Net.Http.MultipartFormDataContent]::new()
  $bc=[System.Net.Http.ByteArrayContent]::new([System.Text.Encoding]::UTF8.GetBytes($text))
  $bc.Headers.ContentDisposition=[System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data')
  $bc.Headers.ContentDisposition.Name="`"$fname`""; $bc.Headers.ContentDisposition.FileName="`"$fname`""
  $bc.Headers.ContentType=[System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream')
  $mp2.Add($bc)
  return Wdp POST "/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$FNe&path=%5CLocalState" $mp2
}
$up=Upload-LocalState 'autodiag.txt' $content
Write-Host "推 autodiag.txt: $([int]$up.StatusCode)  $(Txt $up)"
if([int]$up.StatusCode -ge 400){
  # LocalState 可能未建 → 先启动一次建好,再推
  Write-Host "LocalState 可能未建,先启动一次…" -ForegroundColor Yellow
  $aid=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Praid))
  $pkg=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($FN))
  Wdp POST "/api/taskmanager/app?appid=$([uri]::EscapeDataString($aid))&package=$([uri]::EscapeDataString($pkg))" $null | Out-Null
  Start-Sleep -Seconds 8
  # 关掉
  Wdp DELETE "/api/taskmanager/app?package=$([uri]::EscapeDataString($pkg))" $null | Out-Null
  Start-Sleep -Seconds 2
  $up=Upload-LocalState 'autodiag.txt' $content
  Write-Host "重推 autodiag.txt: $([int]$up.StatusCode)  $(Txt $up)"
}

# 4) 启动 app(自动诊断开跑)
$aid=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Praid))
$pkg=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($FN))
$lr=Wdp POST "/api/taskmanager/app?appid=$([uri]::EscapeDataString($aid))&package=$([uri]::EscapeDataString($pkg))" $null
Write-Host "启动: $([int]$lr.StatusCode)" -ForegroundColor Green

# 5) 轮询 autodump.txt(等所有 URL 跑完,首行 GpuInit + 每个 URL 一段)
$want=$Urls.Count
$got=''
for($i=0;$i -lt 30;$i++){
  Start-Sleep -Seconds 5
  $g=Wdp GET "/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$FNe&path=%5CLocalState&filename=autodump.txt" $null
  if([int]$g.StatusCode -eq 200){
    $got=Txt $g
    $segs=([regex]::Matches($got,'########## URL:')).Count
    Write-Host "  autodump.txt 已写 $($got.Length)B,URL段=$segs/$want"
    if($segs -ge $want){ break }
  } else { Write-Host "  等 autodump.txt…($i) code=$([int]$g.StatusCode)" }
}
if($got){
  $out="$OutDir\autodump-$Ver.txt"
  [System.IO.File]::WriteAllText($out,$got)
  Write-Host "✅ 已拉回: $out ($($got.Length)B)" -ForegroundColor Green
} else {
  Write-Host "❌ 没拿到 autodump.txt" -ForegroundColor Red
}
