#Requires -Version 7.0
<#
.SYNOPSIS
    通过 Windows Device Portal (WDP) 把 appx/msix 推送并安装到 Windows 10 Mobile 设备。

.DESCRIPTION
    针对 Lumia 950 (Win10M) 的开发部署工具。封装 WDP 的 REST API:
      - 连通性/设备信息: GET /api/os/info
      - 安装:           POST /api/app/packagemanager/package?package=<file>   (multipart)
      - 安装状态:        GET  /api/app/packagemanager/state
      - 已装列表:        GET  /api/app/packagemanager/packages
      - 卸载:           DELETE /api/app/packagemanager/package?package=<FullName>
    自动处理 CSRF token(WDP 用 CSRF-Token cookie + X-CSRF-Token 头)与自签名证书。

.PARAMETER Appx
    要安装的主包路径 (.appx / .appxbundle / .msix / .msixbundle)。

.PARAMETER Dependencies
    依赖包路径数组(如 Microsoft.VCLibs ARM)。可选。

.PARAMETER Certificate
    侧载签名证书 (.cer)。首次侧载某证书时需要。可选。

.PARAMETER DeviceIp
    设备内网 IP。默认 192.168.3.51。

.PARAMETER Scheme / Port
    默认 https / 443。Win10M 也常用 http / 80(无证书烦恼但明文)。

.PARAMETER Credential
    WDP 若设了用户名/密码,用 -Credential 传 Basic 认证。

.PARAMETER List
    只列出设备上已安装的包,不部署。

.PARAMETER Uninstall
    传入 PackageFullName 执行卸载,不部署。

.EXAMPLE
    .\Wdp-Deploy.ps1 -Appx .\harness\AppPackages\Harness_ARM.appxbundle `
                     -Dependencies .\harness\AppPackages\Microsoft.VCLibs.ARM.appx

.EXAMPLE
    .\Wdp-Deploy.ps1 -List
    .\Wdp-Deploy.ps1 -Uninstall EdgeHTMLReborn.Harness_1.0.0.0_arm__abc123
#>
[CmdletBinding(DefaultParameterSetName = 'Deploy')]
param(
    [Parameter(ParameterSetName = 'Deploy', Mandatory, Position = 0)]
    [string]$Appx,

    [Parameter(ParameterSetName = 'Deploy')]
    [string[]]$Dependencies = @(),

    [Parameter(ParameterSetName = 'Deploy')]
    [string]$Certificate,

    [Parameter(ParameterSetName = 'List')]
    [switch]$List,

    [Parameter(ParameterSetName = 'Uninstall', Mandatory)]
    [string]$Uninstall,

    [string]$DeviceIp = '192.168.3.51',
    [ValidateSet('https', 'http')]
    [string]$Scheme = 'https',
    [int]$Port = 443,
    [pscredential]$Credential,
    [int]$TimeoutSec = 300
)

$ErrorActionPreference = 'Stop'

# http 默认走 80(除非用户显式给了 -Port)
if ($Scheme -eq 'http' -and -not $PSBoundParameters.ContainsKey('Port')) { $Port = 80 }

$BaseUri = "${Scheme}://${DeviceIp}:${Port}"

# ---------------------------------------------------------------------------
# 公共请求参数:跳过自签名证书校验 + 共享会话(带 cookie)+ 可选 Basic 认证
# ---------------------------------------------------------------------------
$script:Session = $null
$script:Csrf = $null   # WDP CSRF-Token:PS 的 cookie 容器收不下 WDP 的 Set-Cookie,直接从响应头抠

function Update-Csrf($resp) {
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
    param(
        [string]$Method = 'GET',
        [string]$Path,
        [hashtable]$Form,
        [switch]$Raw
    )
    $args = @{
        Uri                  = "$BaseUri$Path"
        Method               = $Method
        SkipCertificateCheck = $true
        ConnectionTimeoutSeconds = 15
    }
    if ($script:Session) { $args.WebSession = $script:Session }
    else { $args.SessionVariable = 'newSession' }
    if ($Credential) { $args.Authentication = 'Basic'; $args.Credential = $Credential; $args.AllowUnencryptedAuthentication = ($Scheme -eq 'http') }

    # 改状态的请求 (POST/DELETE) 需要带 CSRF token
    if ($Method -in 'POST', 'DELETE') {
        $token = Get-CsrfToken
        if ($token) { $args.Headers = @{ 'X-CSRF-Token' = $token } }
    }
    if ($Form) { $args.Form = $Form }

    $resp = Invoke-WebRequest @args
    if ($newSession) { $script:Session = $newSession }  # 首次建会话后保存
    Update-Csrf $resp

    if ($Raw) { return $resp }
    if ($resp.Content) { try { return $resp.Content | ConvertFrom-Json } catch { return $resp.Content } }
    return $resp
}

function Get-CsrfToken {
    # 先用一次 GET 让 WDP 下发 CSRF-Token(Update-Csrf 从响应头抠并存 $script:Csrf)
    if (-not $script:Csrf) {
        try { Invoke-Wdp -Method GET -Path '/api/os/info' -Raw | Out-Null } catch {}
    }
    return $script:Csrf
}

function Wait-InstallComplete {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    Write-Host "==> 等待安装完成..." -ForegroundColor Cyan
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        try {
            $resp = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/state' -Raw
        } catch {
            # 某些固件在无进行中安装时返回 204/404, 视为已结束
            Write-Host "    (状态查询结束)" -ForegroundColor DarkGray
            return $true
        }
        if ($resp.StatusCode -eq 204 -or -not $resp.Content) {
            Write-Host "    安装结束 (无进行中任务)" -ForegroundColor DarkGray
            return $true
        }
        $state = $resp.Content | ConvertFrom-Json
        $pct = if ($null -ne $state.PercentComplete) { " $($state.PercentComplete)%" } else { '' }
        Write-Host "    [$($state.CodeText)]$pct $($state.Reason)" -ForegroundColor DarkGray
        if ($state.Success -eq $true) { return $true }
        if ($state.Code -and $state.Code -ne 0 -and $state.Success -eq $false -and $state.CodeText -notmatch 'InProgress|Installing') {
            throw "安装失败: Code=$($state.Code) $($state.CodeText) - $($state.Reason)"
        }
    }
    throw "安装超时 (${TimeoutSec}s)"
}

# ===========================================================================
# 入口
# ===========================================================================

Write-Host "WDP 目标: $BaseUri" -ForegroundColor Yellow

# --- 连通性 + 设备信息 ---
try {
    $info = Invoke-Wdp -Method GET -Path '/api/os/info'
    Write-Host "==> 已连接: $($info.ComputerName)  OS $($info.OsVersion)  $($info.Platform)" -ForegroundColor Green
} catch {
    Write-Error "无法连接 WDP ($BaseUri): $($_.Exception.Message)`n提示: 确认设备已开 Device Portal,IP/端口/scheme 正确(Win10M 常用 http:80 或 https:443),如设了密码用 -Credential。"
    exit 1
}

# --- List 模式 ---
if ($List) {
    $pkgs = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/packages'
    Write-Host "`n已安装应用:" -ForegroundColor Cyan
    $pkgs.InstalledPackages | Sort-Object Name | ForEach-Object {
        "{0,-45} {1}" -f $_.Name, $_.PackageFullName
    }
    exit 0
}

# --- Uninstall 模式 ---
if ($Uninstall) {
    Write-Host "==> 卸载: $Uninstall" -ForegroundColor Cyan
    Invoke-Wdp -Method DELETE -Path "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($Uninstall))" | Out-Null
    Write-Host "==> 卸载请求已发送。" -ForegroundColor Green
    exit 0
}

# --- Deploy 模式 ---
$appxItem = Get-Item -LiteralPath $Appx
$pkgName  = $appxItem.Name

# multipart:每个文件一个 part,字段名 = 文件名(WDP 按 content-disposition 文件名识别)
$form = [ordered]@{}
$form[$pkgName] = $appxItem
foreach ($dep in $Dependencies) {
    $d = Get-Item -LiteralPath $dep
    $form[$d.Name] = $d
    Write-Host "    + 依赖: $($d.Name)" -ForegroundColor DarkGray
}
if ($Certificate) {
    $c = Get-Item -LiteralPath $Certificate
    $form[$c.Name] = $c
    Write-Host "    + 证书: $($c.Name)" -ForegroundColor DarkGray
}

Write-Host "==> 上传并安装: $pkgName ($([math]::Round($appxItem.Length/1MB,1)) MB)" -ForegroundColor Cyan
$installPath = "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($pkgName))"
Invoke-Wdp -Method POST -Path $installPath -Form $form | Out-Null

Wait-InstallComplete | Out-Null
Write-Host "==> 完成: $pkgName 已部署到 $DeviceIp" -ForegroundColor Green
