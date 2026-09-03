# Builds the ARM32 UWP harness. VS2017 performs official C++/CX XAML codegen;
# VS18 then compiles and links those generated sources with the v143 WebKit libraries.
param(
    [string]$SdkVersion = '10.0.22621.0',
    [string]$XamlSdkVersion = '10.0.17763.0',
    [string]$VCToolsVersion = '14.44.35207',
    [string]$VcpkgRoot = 'C:\vcpkg',
    [string]$IcuRoot = 'C:\icu-arm-uwp',
    [ValidateSet('Official', 'Fallback')]
    [string]$XamlMode = 'Official',
    # Apotheosis (M4): package an A/B engine tree + its driver without touching the baseline
    # (defaults: <root>\build-clang-gpu and <root>\port, see Harness.vcxproj).
    [string]$EngineBuildDir = '',
    [string]$DriverDir = '',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$proj = Join-Path $root 'harness\Harness.vcxproj'
$log = Join-Path $root 'harness-build.log'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere.exe was not found: $vswhere" }
$vs = & $vswhere -latest -products '*' -version '[17.0,19.0)' -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 or newer with MSBuild was not found.' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) { throw "MSBuild.exe was not found: $msbuild" }
$armCompiler = Join-Path $vs "VC\Tools\MSVC\$VCToolsVersion\bin\Hostx64\arm\cl.exe"
if (-not (Test-Path -LiteralPath $armCompiler)) { throw "MSVC $VCToolsVersion ARM compiler was not found: $armCompiler" }

$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$required = @(
    (Join-Path $sdkRoot "Include\$SdkVersion\um\Windows.h"),
    (Join-Path $sdkRoot "Lib\$SdkVersion\um\arm\WindowsApp.lib")
)
$missing = $required | Where-Object { -not (Test-Path -LiteralPath $_) }
if ($missing) { throw "SDK $SdkVersion is incomplete for ARM UWP:`n$($missing -join "`n")" }

$generator = Join-Path $PSScriptRoot 'gen-xaml-codebehind.ps1'
$commonArgs = @(
    $proj,
    '/p:Configuration=Release',
    '/p:Platform=ARM',
    "/p:ApotheosisWindowsSdkVersion=$SdkVersion",
    "/p:VCToolsVersion=$VCToolsVersion",
    "/p:ApotheosisVcpkgRoot=$VcpkgRoot",
    "/p:ApotheosisIcuRoot=$IcuRoot"
)
if ($EngineBuildDir) { $commonArgs += "/p:ApotheosisEngineBuildDir=$EngineBuildDir" }
if ($DriverDir) { $commonArgs += "/p:ApotheosisDriverDir=$DriverDir" }
Write-Host "=== Harness: XAML=$XamlMode, SDK=$SdkVersion, MSVC=$VCToolsVersion ===" -ForegroundColor Cyan

if ($Clean) {
    Write-Host '=== [clean] v143 outputs ===' -ForegroundColor Cyan
    & $msbuild @commonArgs /p:ApotheosisConsumeOfficialXaml=true /t:Clean /v:minimal
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($XamlMode -eq 'Official') {
    $vs2017 = & $vswhere -latest -products '*' -version '[15.0,16.0)' -requires Microsoft.Component.MSBuild -property installationPath
    if (-not $vs2017) { throw 'Visual Studio 2017 with MSBuild is required for official C++/CX XAML codegen.' }
    $msbuild2017 = Join-Path $vs2017 'MSBuild\15.0\Bin\amd64\MSBuild.exe'
    $v141ArmCompiler = Join-Path $vs2017 'VC\Tools\MSVC\14.16.27023\bin\Hostx64\arm\cl.exe'
    $xamlTask = Join-Path $sdkRoot "bin\$XamlSdkVersion\XamlCompiler\Microsoft.Windows.UI.Xaml.Build.Tasks.dll"
    foreach ($path in @($msbuild2017, $v141ArmCompiler, $xamlTask)) {
        if (-not (Test-Path -LiteralPath $path)) { throw "Official XAML dependency was not found: $path" }
    }

    $xamlArgs = @(
        $proj,
        '/p:Configuration=Release',
        '/p:Platform=ARM',
        '/p:PlatformToolset=v141',
        "/p:ApotheosisWindowsSdkVersion=$XamlSdkVersion",
        '/p:ApotheosisUseOfficialXaml=true',
        '/p:ApotheosisXamlCodegen=true',
        '/p:IntDir=Harness\ARM\XamlCodegen\'
    )
    # BuildCompile also links, so this pass needs the same engine/driver trees as the v143 pass;
    # otherwise it falls back to <root>\build-clang-gpu and fails with LNK1181 WebCore.lib.
    if ($EngineBuildDir) { $xamlArgs += "/p:ApotheosisEngineBuildDir=$EngineBuildDir" }
    if ($DriverDir) { $xamlArgs += "/p:ApotheosisDriverDir=$DriverDir" }
    Write-Host "=== [1/2] Official XAML codegen: VS2017 + SDK $XamlSdkVersion ===" -ForegroundColor Cyan
    & $msbuild2017 @xamlArgs /t:Clean /v:minimal
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $msbuild2017 @xamlArgs /t:BuildCompile /m /v:minimal
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $generated = @('App.g.h', 'App.g.hpp', 'App.xbf', 'MainPage.g.h', 'MainPage.g.hpp', 'MainPage.xbf', 'XamlTypeInfo.Impl.g.cpp', 'XamlTypeInfo.g.cpp')
    $missingGenerated = $generated | Where-Object { -not (Test-Path -LiteralPath (Join-Path $root "harness\Generated Files\$_")) }
    if ($missingGenerated) { throw "Official XAML codegen did not produce:`n$($missingGenerated -join "`n")" }
    $buildArgs = @($commonArgs + '/p:ApotheosisConsumeOfficialXaml=true')
} else {
    Write-Host '=== [1/2] Generate XamlReader fallback ===' -ForegroundColor Cyan
    & pwsh -NoProfile -File $generator
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $buildArgs = $commonArgs
}

Write-Host '=== [2/2] Build and package appx ===' -ForegroundColor Cyan
& $msbuild @buildArgs /m /v:minimal 2>&1 | Tee-Object $log
$code = $LASTEXITCODE
Write-Host "=== MSBuild exit code: $code ==="
exit $code
