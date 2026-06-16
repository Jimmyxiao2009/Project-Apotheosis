#Requires -Version 7.0
<#  Wdp-Cycle.ps1 — 一键:卸载→安装→开崩溃收集→清旧dump→远程启动→等待→查进程/拉新dump并定性。
    全程走 WDP REST + HttpClient(手搓 multipart + 从 Set-Cookie 头抠 CSRF,绕开 PS cookie 容器坑)。 #>
[CmdletBinding()]
param(
    [string]$Appx='E:\Apotheosis\harness\AppPackages\Harness\Harness_0.1.0.0_ARM_Test\Harness_0.1.0.0_ARM.appx',
    [string]$Cer ='E:\Apotheosis\harness\AppPackages\Harness\Harness_0.1.0.0_ARM_Test\Harness_0.1.0.0_ARM.cer',
    [string]$FullName='EdgeHTMLReborn.Harness_0.1.0.0_arm__x0rqga78m2mgc',
    [string]$Praid='EdgeHTMLReborn.Harness_x0rqga78m2mgc!App',
    [string]$Ip='192.168.3.51',
    [int]$WaitSec=14,
    [switch]$SkipInstall,   # 只启动+抓崩溃,不重装
    [string]$OutDir='E:\Apotheosis\crash'
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Net.Http
$base="https://${Ip}:443"
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
function WdpText([string]$method,[string]$path){ $r=Wdp $method $path $null; return @{Code=[int]$r.StatusCode;Body=$r.Content.ReadAsStringAsync().Result} }

# 0) 建会话拿 CSRF
Sync-Csrf ($cli.GetAsync("$base/api/os/info").GetAwaiter().GetResult())
$info=(WdpText GET '/api/os/info').Body
Write-Host "设备: $info" -ForegroundColor Green

if(-not $SkipInstall){
  # 1) 卸载旧
  $u=WdpText DELETE "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($FullName))"
  Write-Host "卸载: $($u.Code)"
  # 2) 安装新(multipart appx+cer)
  $form=[System.Net.Http.MultipartFormDataContent]::new()
  foreach($f in @($Appx,$Cer)){
    $name=[IO.Path]::GetFileName($f)
    $sc=[System.Net.Http.StreamContent]::new([IO.File]::OpenRead($f))
    $sc.Headers.ContentType=[System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream')
    $cd=[System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data'); $cd.Name="`"$name`""; $cd.FileName="`"$name`""
    $sc.Headers.ContentDisposition=$cd; $form.Add($sc)
  }
  $appxName=[IO.Path]::GetFileName($Appx)
  Write-Host "上传安装 $appxName …"
  $ins=Wdp POST "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($appxName))" $form
  Write-Host "安装请求: $([int]$ins.StatusCode) $($ins.Content.ReadAsStringAsync().Result)"
  # 3) 轮询安装完成
  for($i=1;$i -le 40;$i++){
    $st=WdpText GET '/api/app/packagemanager/state'
    if($st.Code -eq 204 -or -not $st.Body){ Write-Host "安装结束(无进行中任务)"; break }
    if($st.Body -match '"Success"\s*:\s*true'){ Write-Host "安装完成 ✓"; break }
    if($st.Body -match '"Success"\s*:\s*false' -and $st.Body -notmatch 'InProgress|Installing'){ Write-Host "安装失败: $($st.Body)" -ForegroundColor Red; break }
    Start-Sleep -Seconds 2
  }
}

# 4) 开崩溃收集
$ccPath="/api/debug/dump/usermode/crashcontrol?packageFullName=$([uri]::EscapeDataString($FullName))"
Write-Host "开崩溃收集: $((WdpText POST $ccPath).Code)"
# 5) 清旧 dump
$dl=(WdpText GET "/api/debug/dump/usermode/dumps?packageFullName=$([uri]::EscapeDataString($FullName))").Body
$old=@(); try{ $old=@(($dl|ConvertFrom-Json).CrashDumps) }catch{}
foreach($d in $old){ WdpText DELETE "/api/debug/dump/usermode/crashdump?packageFullName=$([uri]::EscapeDataString($FullName))&fileName=$([uri]::EscapeDataString($d.FileName))" | Out-Null }
Write-Host "清掉旧 dump: $($old.Count) 个"

# 6) 远程启动
$b64a=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Praid))
$b64p=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($FullName))
$lc=(WdpText POST "/api/taskmanager/app?appid=$([uri]::EscapeDataString($b64a))&package=$([uri]::EscapeDataString($b64p))").Code
Write-Host "启动: $lc"
Write-Host "等待 ${WaitSec}s 让其初始化/渲染/崩溃 …"
Start-Sleep -Seconds $WaitSec

# 7) 查进程
$pl=(WdpText GET '/api/resourcemanager/processes').Body
$alive=@(($pl|ConvertFrom-Json).Processes | Where-Object { $_.ImageName -match 'Harness' })
if($alive){ Write-Host "✓ 进程存活(未崩溃):" -ForegroundColor Green; $alive|ForEach-Object{ "  PID $($_.ProcessId) $($_.ImageName) RAM $([math]::Round($_.PrivateWorkingSet/1MB,1))MB" } }
else{ Write-Host "✗ 无 Harness 进程(已退出/崩溃/被挂起)" -ForegroundColor Yellow }

# 8) 查新 dump
$dl2=(WdpText GET "/api/debug/dump/usermode/dumps?packageFullName=$([uri]::EscapeDataString($FullName))").Body
$nd=@(); try{ $nd=@(($dl2|ConvertFrom-Json).CrashDumps) }catch{}
if($nd.Count){
  $newest=$nd | Sort-Object {try{[datetime]$_.FileDate}catch{0}} -Descending | Select-Object -First 1
  New-Item -ItemType Directory -Force $OutDir|Out-Null
  $local=Join-Path $OutDir $newest.FileName
  Write-Host "⚠ 新崩溃 dump: $($newest.FileName) — 下载分析…" -ForegroundColor Red
  $url="$base/api/debug/dump/usermode/crashdump?packageFullName=$([uri]::EscapeDataString($FullName))&fileName=$([uri]::EscapeDataString($newest.FileName))"
  [IO.File]::WriteAllBytes($local,$cli.GetByteArrayAsync($url).GetAwaiter().GetResult())
  Write-Host "  -> $local"
  & pwsh -NoProfile -File 'E:\Apotheosis\tools\Parse-Minidump.ps1' -Path $local 2>&1 | Select-Object -First 12
  Write-Host "（PC 反符号化用 Symbolize-Crash.ps1 看出错函数）"
} else {
  Write-Host "✓ 无新崩溃 dump —— 没有在初始化路径上崩!" -ForegroundColor Green
}
