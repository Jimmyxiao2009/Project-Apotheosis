# fetch-publicsuffix.ps1  --  download the Public Suffix List into harness\Assets.
#
# The engine needs to know where a host name stops being a registry and starts being somebody's
# site: "www.example.com" and "cdn.example.com" belong to the same owner, "a.example.co.uk" and
# "b.other.co.uk" do not. WebCore asks PublicSuffixStore for that (RegistrableDomain), and the
# cookie jar's accept policy is built on it - without real data a cookie set by one subdomain is
# never stored for, nor sent to, another subdomain of the same site.
#
# The GTK port answers this from libsoup's copy of the list. This port has no libsoup, so the
# list travels inside the appx as a plain data file and the driver parses it once at startup
# (port\stubs-other.cpp, WebCoreSetPublicSuffixListBlob).
#
# The list is published by Mozilla under the Mozilla Public License 2.0, which permits
# redistribution; the licence text is fetched next to it and packaged too.
#
# The data file is gitignored (like the fonts): run this once per clone. It is pinned to a
# commit and verified by SHA-256, so a re-run is a no-op and an upstream change fails loudly
# instead of silently altering the package.
#
#   pwsh -File port\fetch-publicsuffix.ps1            # fetch if missing / wrong
#   pwsh -File port\fetch-publicsuffix.ps1 -Force     # re-download
#
# After running, Harness.vcxproj picks the file up on its own (the entry is conditional).
#
# To move to a newer list: put the new commit in $commit, run with -Force, and copy the hash the
# script prints on the mismatch into $items.

param(
    [string]$AssetsDir = (Join-Path $PSScriptRoot '..\harness\Assets'),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# Pinned commit of https://github.com/publicsuffix/list. The list changes almost daily; pinning
# keeps a clone reproducible and makes an update a deliberate, reviewable step.
$commit = '3955e3ec29b94c3cca7bd4509c5f14a7c0959e26'

$items = @(
    # The list itself. Its internationalised rules are written in Unicode, while the host names
    # the engine looks up always arrive punycoded, so those rules would never match and an IDN
    # registry would be mistaken for somebody's site. Rather than carry an IDNA encoder into the
    # driver for ~100 rules, the labels are punycoded here, once, at fetch time - so Sha256 is
    # the hash of the download (what upstream published) and Sha256Out the hash of the file that
    # ends up in the package.
    @{ Name     = 'public_suffix_list.dat'
       Url      = "https://raw.githubusercontent.com/publicsuffix/list/$commit/public_suffix_list.dat"
       Sha256   = 'A26F7D7E334778ED69216CEDB5451EF82031FEBA6615C12039783CDD94E1FCAE'
       Sha256Out = '8352993AB535B27588B89461F6847C1568E98C747582079347B5156E2C933CC0'
       Punycode = $true }
    @{ Name   = 'MPL-2.0-PublicSuffixList.txt'
       Url    = "https://raw.githubusercontent.com/publicsuffix/list/$commit/LICENSE"
       Sha256 = '66A3107D5AD6A058AAB753EAAC2047CCB2ED0E39465DD0FE5844DA3E300D5172' }
)

# True when the text holds a character outside ASCII. Written as a code-point test on
# purpose: a regex escape for the range is easy to turn into a literal NUL byte in this
# file, and a NUL makes git treat the script as binary.
function Test-NonAscii([string]$text) {
    foreach ($ch in $text.ToCharArray()) { if ([int]$ch -gt 127) { return $true } }
    return $false
}

# Rewrite every rule's non-ASCII labels as punycode, leaving comments, blank lines and the "!"
# and "*" rule markers exactly where they were.
function Convert-RulesToPunycode([string]$path) {
    $idn = New-Object System.Globalization.IdnMapping
    $out = New-Object Text.StringBuilder
    foreach ($line in [IO.File]::ReadAllLines($path, [Text.Encoding]::UTF8)) {
        $rule = $line
        if ($rule -and -not $rule.StartsWith('//') -and (Test-NonAscii $rule)) {
            $bang = $rule.StartsWith('!')
            $body = if ($bang) { $rule.Substring(1) } else { $rule }
            $labels = $body.Split('.') | ForEach-Object {
                if (Test-NonAscii $_) { $idn.GetAscii($_) } else { $_ }
            }
            $rule = (&{ if ($bang) { '!' } else { '' } }) + ($labels -join '.')
        }
        [void]$out.AppendLine($rule)
    }
    # LF and no BOM: the driver's parser reads bytes, and this keeps the file diffable.
    [IO.File]::WriteAllText($path, ($out.ToString() -replace "`r`n", "`n"), (New-Object Text.UTF8Encoding $false))
}

function Get-Sha256([string]$path) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToUpperInvariant()
}

if (-not (Test-Path -LiteralPath $AssetsDir)) {
    New-Item -ItemType Directory -Path $AssetsDir -Force | Out-Null
}

foreach ($item in $items) {
    $dest = Join-Path $AssetsDir $item.Name

    # A punycoded file no longer hashes to the download, so its idempotence check uses Sha256Out.
    $wantOnDisk = if ($item.Punycode) { $item.Sha256Out } else { $item.Sha256 }

    if ((Test-Path -LiteralPath $dest) -and -not $Force -and $wantOnDisk) {
        if ((Get-Sha256 $dest) -eq $wantOnDisk) {
            Write-Host ("ok      {0}" -f $item.Name)
            continue
        }
        Write-Host ("stale   {0} - re-downloading" -f $item.Name) -ForegroundColor Yellow
    }

    Write-Host ("fetch   {0}" -f $item.Name)
    $tmp = "$dest.part"
    try {
        Invoke-WebRequest -Uri $item.Url -OutFile $tmp -UseBasicParsing
        $got = Get-Sha256 $tmp
        if ($got -ne $item.Sha256) {
            throw ("SHA-256 mismatch for {0}`n  expected {1}`n  got      {2}`n" -f $item.Name, $item.Sha256, $got)
        }
        if ($item.Punycode) {
            Convert-RulesToPunycode $tmp
            $outHash = Get-Sha256 $tmp
            if ($item.Sha256Out -and $item.Sha256Out -ne $outHash) {
                throw ("punycoded {0} hashes to {1}, expected {2}" -f $item.Name, $outHash, $item.Sha256Out)
            }
            Write-Host ("        punycoded, sha256 {0}" -f $outHash)
        }
        Move-Item -LiteralPath $tmp -Destination $dest -Force
        Write-Host ("done    {0}" -f $item.Name) -ForegroundColor Green
    } finally {
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
    }
}
