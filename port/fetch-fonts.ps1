# fetch-fonts.ps1  --  download the packaged fallback fonts into harness\Assets\fonts.
#
# The app container has no access to the device's system fonts, so every face the engine may
# need has to travel inside the appx. The Latin cuts are the local Windows files (segoeui/arial/
# times/cour plus their bold); this script fetches the faces that cover everything those eight
# files do not: Simplified Chinese, the symbol blocks (arrows, check marks, technical and
# miscellaneous symbols) and emoji as monochrome outlines.
#
# All four are Noto, licensed under the SIL Open Font License 1.1, so they may be redistributed
# inside the package. The licence text of each family is fetched next to it and packaged too.
#
# Font binaries are gitignored (like the Microsoft cuts): run this once per clone. Every file is
# pinned to a release tag or a commit and verified by SHA-256, so a re-run is a no-op and an
# upstream change fails loudly instead of silently altering the package.
#
#   pwsh -File port\fetch-fonts.ps1            # fetch what is missing / wrong
#   pwsh -File port\fetch-fonts.ps1 -Force     # re-download everything
#
# After running, Harness.vcxproj picks the files up on its own (the entries are conditional).

param(
    [string]$FontsDir = (Join-Path $PSScriptRoot '..\harness\Assets\fonts'),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# Name  = file name under Assets\fonts
# Url   = pinned download (release asset or raw blob at a commit)
# Member= entry to lift out of the archive; empty means the download *is* the file
# Sha256= hash of the final file, not of the archive
$items = @(
    # Simplified Chinese, the language-specific subset of Noto Sans CJK (family "Noto Sans SC").
    # The noto-cjk releases ship this one as CFF/OpenType only - there is no static TTF cut of it
    # upstream - which FreeType's CFF driver handles like any other face.
    @{ Name = 'NotoSansSC-Regular.otf'
       Url  = 'https://github.com/notofonts/noto-cjk/releases/download/Sans2.004/18_NotoSansSC.zip'
       Member = 'NotoSansSC-Regular.otf'
       Sha256 = 'FAA6C9DF652116DDE789D351359F3D7E5D2285A2B2A1F04A2D7244DF706D5EA9' }
    @{ Name = 'OFL-NotoSansSC.txt'
       Url  = 'https://github.com/notofonts/noto-cjk/releases/download/Sans2.004/18_NotoSansSC.zip'
       Member = 'LICENSE'
       Sha256 = '6A73F9541C2DE74158C0E7CF6B0A58EF774F5A780BF191F2D7EC9CC53EFE2BF2' }

    # Arrows, box drawing, technical and miscellaneous symbols (family "Noto Sans Symbols").
    @{ Name = 'NotoSansSymbols-Regular.ttf'
       Url  = 'https://github.com/notofonts/symbols/releases/download/NotoSansSymbols-v2.003/NotoSansSymbols-v2.003.zip'
       Member = 'NotoSansSymbols/full/ttf/NotoSansSymbols-Regular.ttf'
       Sha256 = '0088617BAEC0E8AC47E022CC1F38695F772301C9EF6D1F24A785ABBEF1E05D79' }
    @{ Name = 'OFL-NotoSansSymbols.txt'
       Url  = 'https://github.com/notofonts/symbols/releases/download/NotoSansSymbols-v2.003/NotoSansSymbols-v2.003.zip'
       Member = 'OFL.txt'
       Sha256 = 'B118DD41337806A5D4797052C77CAF3BD096AED783E5EB21B4D11154351E1AC0' }

    # Check marks, geometric shapes, dingbats, the rest of the symbol blocks ("Noto Sans Symbols 2").
    @{ Name = 'NotoSansSymbols2-Regular.ttf'
       Url  = 'https://github.com/notofonts/symbols/releases/download/NotoSansSymbols2-v2.008/NotoSansSymbols2-v2.008.zip'
       Member = 'NotoSansSymbols2/full/ttf/NotoSansSymbols2-Regular.ttf'
       Sha256 = '3CE38EFFDB615DD929C8B0F52768DFA2CD21F206E3824BC7DF61E4074B41AE52' }
    @{ Name = 'OFL-NotoSansSymbols2.txt'
       Url  = 'https://github.com/notofonts/symbols/releases/download/NotoSansSymbols2-v2.008/NotoSansSymbols2-v2.008.zip'
       Member = 'OFL.txt'
       Sha256 = 'B118DD41337806A5D4797052C77CAF3BD096AED783E5EB21B4D11154351E1AC0' }

    # Emoji as black outlines (family "Noto Emoji"). The colour builds are CBDT/COLR bitmaps this
    # port cannot draw; the monochrome family is published only as a weight-variable TTF, whose
    # default instance is Regular - which is what FreeType renders when nothing sets coordinates.
    @{ Name = 'NotoEmoji-VariableFont_wght.ttf'
       Url  = 'https://raw.githubusercontent.com/google/fonts/b979dba422e445492b0eb9951ac52ee0b4d648c3/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf'
       Member = ''
       Sha256 = 'DE6C18832938AFC99CAF132B39D6A30A19BAC7F2E812E28DB2535B4608D27551' }
    @{ Name = 'OFL-NotoEmoji.txt'
       Url  = 'https://raw.githubusercontent.com/google/fonts/b979dba422e445492b0eb9951ac52ee0b4d648c3/ofl/notoemoji/OFL.txt'
       Member = ''
       Sha256 = '500BB1CCF43DF7BBB522112F9133A52B16E1C35E809632F5D8609B179152DE5B' }
)

function Test-FileHash256([string]$path, [string]$want)
{
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    return ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $want.ToUpperInvariant())
}

$FontsDir = [IO.Path]::GetFullPath($FontsDir)
if (-not (Test-Path -LiteralPath $FontsDir)) { New-Item -ItemType Directory -Path $FontsDir | Out-Null }

$cacheDir = Join-Path $env:TEMP 'apotheosis-fetch-fonts'
if (-not (Test-Path -LiteralPath $cacheDir)) { New-Item -ItemType Directory -Path $cacheDir | Out-Null }

Add-Type -AssemblyName System.IO.Compression.FileSystem

$fetched = 0
$kept    = 0
$failed  = @()

foreach ($it in $items) {
    $dest = Join-Path $FontsDir $it.Name

    if (-not $Force -and (Test-FileHash256 $dest $it.Sha256)) {
        Write-Host ("==> {0,-32} already present" -f $it.Name) -ForegroundColor DarkGray
        $kept++
        continue
    }

    # One cached copy per URL: the CJK archive is 48 MB and holds two of the wanted files.
    $leaf = [IO.Path]::GetFileName(($it.Url -split '\?')[0])
    if ([string]::IsNullOrWhiteSpace($leaf)) { $leaf = 'download.bin' }
    $tmp = Join-Path $cacheDir ('{0:x8}-{1}' -f ($it.Url.GetHashCode() -band 0xffffffff), $leaf)

    if ($Force -or -not (Test-Path -LiteralPath $tmp)) {
        Write-Host ("==> {0,-32} downloading" -f $it.Name) -ForegroundColor Cyan
        Write-Host ("    {0}" -f $it.Url) -ForegroundColor DarkGray
        try {
            $pp = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is 10x slower with the bar
            Invoke-WebRequest -Uri $it.Url -OutFile $tmp -UseBasicParsing
            $ProgressPreference = $pp
        } catch {
            Write-Host ("    download failed: {0}" -f $_.Exception.Message) -ForegroundColor Red
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
            $failed += $it.Name
            continue
        }
    }

    if ([string]::IsNullOrEmpty($it.Member)) {
        Copy-Item -LiteralPath $tmp -Destination $dest -Force
    } else {
        $zip = $null
        try {
            $zip = [IO.Compression.ZipFile]::OpenRead($tmp)
            $entry = $zip.Entries | Where-Object { $_.FullName -eq $it.Member }
            if (-not $entry) {
                Write-Host ("    archive has no entry {0}" -f $it.Member) -ForegroundColor Red
                $failed += $it.Name
                continue
            }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dest, $true)
        } finally {
            if ($zip) { $zip.Dispose() }
        }
    }

    if (Test-FileHash256 $dest $it.Sha256) {
        $len = (Get-Item -LiteralPath $dest).Length
        Write-Host ("    ok, {0:N0} bytes" -f $len) -ForegroundColor Green
        $fetched++
    } else {
        $got = if (Test-Path -LiteralPath $dest) { (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash } else { '<missing>' }
        Write-Host ("    SHA-256 mismatch: want {0}, got {1}" -f $it.Sha256, $got) -ForegroundColor Red
        Remove-Item -LiteralPath $dest -Force -ErrorAction SilentlyContinue
        $failed += $it.Name
    }
}

Write-Host ""
Write-Host ("fonts dir: {0}" -f $FontsDir)
Write-Host ("fetched {0}, already present {1}, failed {2}" -f $fetched, $kept, $failed.Count) `
    -ForegroundColor $(if ($failed.Count -eq 0) { 'Green' } else { 'Red' })

if ($failed.Count -gt 0) {
    foreach ($f in $failed) { Write-Host ("  failed: {0}" -f $f) -ForegroundColor Red }
    Write-Host "The package builds without them, but the engine will draw boxes for the characters they cover." -ForegroundColor Yellow
    exit 1
}

Write-Host "The downloads are cached in $cacheDir - delete it to reclaim the space." -ForegroundColor DarkGray
exit 0
