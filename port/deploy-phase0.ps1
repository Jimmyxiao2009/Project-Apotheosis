# ============================================================================
# deploy-phase0.ps1  —  设备上线后一键部署 Phase 0 harness 到 Lumia 950
# 用法(设备开机/连同一 WiFi/开 Device Portal 后):
#   .\port\deploy-phase0.ps1 -DeviceIp <设备当前IP> [-Scheme http|https] [-Port n] [-Credential (Get-Credential)]
# ============================================================================
param(
    [Parameter(Mandatory)] [string]$DeviceIp,
    [ValidateSet('https','http')] [string]$Scheme = 'https',
    [int]$Port = 443,
    [pscredential]$Credential
)
$root = Split-Path -Parent $PSScriptRoot
$appx = Join-Path $root 'harness\AppPackages\Harness\Harness_0.1.0.0_ARM_Test\Harness_0.1.0.0_ARM.appx'
$cer  = Join-Path $root 'harness\EdgeHTMLReborn.cer'

$args = @{ Appx = $appx; Certificate = $cer; DeviceIp = $DeviceIp; Scheme = $Scheme }
if ($PSBoundParameters.ContainsKey('Port')) { $args.Port = $Port }
if ($Credential) { $args.Credential = $Credential }

& (Join-Path $PSScriptRoot '..\tools\Wdp-Deploy.ps1') @args
