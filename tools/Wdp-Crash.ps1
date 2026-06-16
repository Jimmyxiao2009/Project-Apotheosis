#Requires -Version 7.0
<#
.SYNOPSIS
    用 Windows Device Portal (WDP) 抓 Win10M 设备上 sideload 应用的崩溃 dump 并定性分析。

.DESCRIPTION
    "启动就闪退" 靠肉眼没法定位。本脚本走 WDP 的 usermode crash-dump REST API:
      - 找到目标包(默认匹配名字含 Harness/EdgeHTML)
      - 打开崩溃 dump 收集:   POST /api/debug/dump/usermode/crashcontrol?packageFullName=<FN>
      - 列出已有 dump:        GET  /api/debug/dump/usermode/dumps
      - 下载最新 dump:        GET  /api/debug/dump/usermode/crashdump?packageFullName=<FN>&fileName=<f>
      - 用 cdb 打异常码/模块/栈(异常码本身基本就能定性:
            C0000135=缺 DLL(打包) / C0000139=导出不匹配 / C0000005=访问违例(空指针)
            C0000409=__fastfail/RELEASE_ASSERT(引擎断言) / C06D007E=延迟加载模块缺失)

    典型流程:
      1) .\Wdp-Crash.ps1 -Enable          # 先开收集(只需一次)
      2) 在手机上点开 app 复现闪退
      3) .\Wdp-Crash.ps1 -Pull            # 拉最新 dump 并分析

.PARAMETER Enable   只打开崩溃 dump 收集然后退出。
.PARAMETER List     只列出设备上本包的 dump。
.PARAMETER Pull     下载最新 dump 到 -OutDir 并用 cdb 分析(默认动作)。
.PARAMETER Clear    删除设备上本包的所有 dump。
.PARAMETER Match    包名匹配子串(默认 'Harness|EdgeHTML')。
#>
[CmdletBinding()]
param(
    [switch]$Enable,
    [switch]$List,
    [switch]$Pull,
    [switch]$Clear,
    [string]$Match = 'Harness|EdgeHTML',
    [string]$OutDir = 'E:\Apotheosis\crash',
    [string]$DeviceIp = '192.168.3.51',
    [ValidateSet('https','http')][string]$Scheme = 'https',
    [int]$Port = 443,
    [pscredential]$Credential,
    [string]$AnalyzeFile   # 跳过设备,直接分析本地某个 .dmp
)

$ErrorActionPreference = 'Stop'
if ($Scheme -eq 'http' -and -not $PSBoundParameters.ContainsKey('Port')) { $Port = 80 }
$BaseUri = "${Scheme}://${DeviceIp}:${Port}"
$script:Session = $null
$script:Csrf = $null   # WDP 的 CSRF-Token,直接从 Set-Cookie 头抠(PS 的 cookie 容器收不下)

function Update-Csrf($resp) {
    # WDP 每次响应都可能刷新 CSRF-Token;直接读 Set-Cookie 头,既存 token 又手动塞回 cookie 容器。
    $sc = $resp.Headers['Set-Cookie']
    if ($sc) {
        foreach ($line in @($sc)) {
            if ($line -match 'CSRF-Token=([^;,\s]+)') {
                $script:Csrf = $Matches[1]
                if ($script:Session) {
                    try { $script:Session.Cookies.Add([System.Net.Cookie]::new('CSRF-Token', $script:Csrf, '/', $DeviceIp)) } catch {}
                }
            }
        }
    }
}

function Invoke-Wdp {
    param([string]$Method='GET', [string]$Path, [string]$OutFile)
    $a = @{ Uri="$BaseUri$Path"; Method=$Method; SkipCertificateCheck=$true; ConnectionTimeoutSeconds=15 }
    if ($script:Session) { $a.WebSession = $script:Session } else { $a.SessionVariable='newSession' }
    if ($Credential) { $a.Authentication='Basic'; $a.Credential=$Credential; $a.AllowUnencryptedAuthentication=($Scheme -eq 'http') }
    if ($Method -in 'POST','DELETE','PUT') {
        $t = Get-CsrfToken
        if ($t) { $a.Headers = @{ 'X-CSRF-Token' = $t } }
    }
    if ($OutFile) { $a.OutFile = $OutFile }
    $resp = Invoke-WebRequest @a
    if ($newSession) { $script:Session = $newSession }
    Update-Csrf $resp
    if ($OutFile) { return $resp }
    if ($resp.Content) { try { return $resp.Content | ConvertFrom-Json } catch { return $resp.Content } }
    return $resp
}

function Get-CsrfToken {
    if (-not $script:Csrf) { try { Invoke-Wdp -Method GET -Path '/api/os/info' | Out-Null } catch {} }
    return $script:Csrf
}

function Analyze-Dump([string]$file) {
    $cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
    if (-not (Test-Path $cdb)) { Write-Host "未找到 cdb,无法分析,dump 已存:$file" -ForegroundColor Yellow; return }
    Write-Host "`n========== cdb 分析:$([IO.Path]::GetFileName($file)) ==========" -ForegroundColor Cyan
    # .exr -1 = 最近异常记录(异常码在这);.ecxr 切到异常上下文;k 栈;lm 模块列表(看是否某模块没加载)。
    $cmds = '.symfix; .reload /f; .exr -1; .ecxr; kb; lm 1m; q'
    & $cdb -z $file -c $cmds 2>&1 | Out-Host
}

# 直接分析本地 dump
if ($AnalyzeFile) { Analyze-Dump $AnalyzeFile; return }

# --- 连通 + 找包 ---
Write-Host "WDP: $BaseUri" -ForegroundColor Yellow
$info = Invoke-Wdp -Method GET -Path '/api/os/info'
Write-Host "设备: $($info.ComputerName)  OS $($info.OsVersion)" -ForegroundColor Green

$pkgs = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/packages'
$target = $pkgs.InstalledPackages | Where-Object { $_.Name -match $Match -or $_.PackageFullName -match $Match } | Select-Object -First 1
if (-not $target) {
    Write-Host "没找到匹配 '$Match' 的已装包。现有包:" -ForegroundColor Red
    $pkgs.InstalledPackages | Sort-Object Name | ForEach-Object { "  {0,-40} {1}" -f $_.Name, $_.PackageFullName }
    exit 1
}
$FN = $target.PackageFullName
Write-Host "目标包: $($target.Name)`n        $FN" -ForegroundColor Green
$FNenc = [uri]::EscapeDataString($FN)

# --- Clear ---
if ($Clear) {
    $dumps = Invoke-Wdp -Method GET -Path "/api/debug/dump/usermode/dumps?packageFullName=$FNenc"
    $files = @($dumps.DumpFiles | ForEach-Object { $_.FileName })
    foreach ($f in $files) {
        Invoke-Wdp -Method DELETE -Path "/api/debug/dump/usermode/crashdump?packageFullName=$FNenc&fileName=$([uri]::EscapeDataString($f))" | Out-Null
        Write-Host "  已删 $f"
    }
    Write-Host "清空完成($($files.Count) 个)。" -ForegroundColor Green
    return
}

# --- Enable 崩溃收集(幂等)---
try {
    Invoke-Wdp -Method POST -Path "/api/debug/dump/usermode/crashcontrol?packageFullName=$FNenc" | Out-Null
    Write-Host "✓ 已开启崩溃 dump 收集(POST crashcontrol)。" -ForegroundColor Green
} catch {
    Write-Host "开启 crashcontrol 失败(可能已开/固件差异):$($_.Exception.Message)" -ForegroundColor DarkYellow
}
try {
    $cc = Invoke-Wdp -Method GET -Path "/api/debug/dump/usermode/crashcontrol?packageFullName=$FNenc"
    Write-Host "  crashcontrol 设置: $($cc | ConvertTo-Json -Compress)" -ForegroundColor DarkGray
} catch {}

if ($Enable) {
    Write-Host "`n下一步:在手机上点开 Harness 复现闪退,然后跑 .\Wdp-Crash.ps1 -Pull" -ForegroundColor Cyan
    return
}

# --- 列 dump ---
$dumps = Invoke-Wdp -Method GET -Path "/api/debug/dump/usermode/dumps?packageFullName=$FNenc"
# 固件差异:有的返回 {CrashDumps:[...]},有的 {DumpFiles:[...]}。
$dumpList = @($dumps.CrashDumps)
if (-not $dumpList -or $dumpList.Count -eq 0) { $dumpList = @($dumps.DumpFiles) }
if (-not $dumpList -or $dumpList.Count -eq 0) {
    Write-Host "`n设备上暂无本包 dump。" -ForegroundColor Yellow
    Write-Host "若刚开启收集:在手机上复现一次闪退,再跑 -Pull。" -ForegroundColor Cyan
    Write-Host "原始返回:$($dumps | ConvertTo-Json -Compress)" -ForegroundColor DarkGray
    return
}
Write-Host "`n本包 dump($($dumpList.Count)):" -ForegroundColor Cyan
$dumpList | ForEach-Object { "  {0}   {1}" -f $_.FileName, $_.FileDate }

if ($List) { return }

# --- Pull 最新 + 分析(默认动作)---
$newest = $dumpList | Sort-Object { try { [datetime]$_.FileDate } catch { 0 } } -Descending | Select-Object -First 1
if (-not $newest) { $newest = $dumpList[-1] }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$local = Join-Path $OutDir $newest.FileName
Write-Host "`n下载最新 dump: $($newest.FileName)" -ForegroundColor Cyan
Invoke-Wdp -Method GET -Path "/api/debug/dump/usermode/crashdump?packageFullName=$FNenc&fileName=$([uri]::EscapeDataString($newest.FileName))" -OutFile $local | Out-Null
Write-Host "  -> $local ($([math]::Round((Get-Item $local).Length/1KB)) KB)" -ForegroundColor Green
Analyze-Dump $local
