# Apotheosis: configure, build and run the TileGrid v2 host tests.
# Exit code = number of failed checks (0 = green).
#
#   pwsh -File tests\tilegrid\run-tests.ps1
#   pwsh -File tests\tilegrid\run-tests.ps1 -WebKitSourceDir D:\src\webkit -Clean

[CmdletBinding()]
param(
    [string]$WebKitSourceDir = 'C:/code/apotheosis/webkit',
    [string]$BuildDir = '',
    [ValidateSet('Ninja', 'VS2022')]
    [string]$Generator = 'VS2022',
    [string]$Config = 'RelWithDebInfo',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $BuildDir) { $BuildDir = Join-Path $here 'build' }

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue)
if (-not $cmake) { throw 'cmake not on PATH.' }

$args = @('-S', $here, '-B', $BuildDir, "-DWEBKIT_SOURCE_DIR=$($WebKitSourceDir -replace '\\', '/')")
if ($Generator -eq 'VS2022') {
    $args += @('-G', 'Visual Studio 17 2022', '-A', 'x64')
} else {
    # Ninja needs a compiler in the environment: run this from a
    # "x64 Native Tools Command Prompt" or after vcvars64.bat.
    $args += @('-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Config")
}

& cmake @args
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

$buildArgs = @('--build', $BuildDir)
if ($Generator -eq 'VS2022') { $buildArgs += @('--config', $Config) }
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$exe = Join-Path $BuildDir 'tilegrid-tests.exe'
if (-not (Test-Path $exe)) { $exe = Join-Path $BuildDir "$Config\tilegrid-tests.exe" }
if (-not (Test-Path $exe)) { throw "tilegrid-tests.exe not found under $BuildDir" }

& $exe
$failures = $LASTEXITCODE
if ($failures -eq 0) {
    Write-Host 'tilegrid: green' -ForegroundColor Green
} else {
    Write-Host "tilegrid: $failures failed check(s)" -ForegroundColor Red
}
exit $failures
