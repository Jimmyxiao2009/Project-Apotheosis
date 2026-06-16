# link-driver-jit.ps1 — 同 link-driver.ps1,但驱动用 JIT 头(compile-driver-jit)编、链 build-clang-jit 库。
# 产出 .jit.obj + 归档 WebCoreDriver.lib(JIT 版,供 JIT harness 链接;CLoop 版可用 link-driver.ps1 重生)。
$ErrorActionPreference = 'Stop'
. 'E:\Apotheosis\port\arm32-uwp-env.ps1' *> $null
$P = 'E:\Apotheosis\port'

$srcs = @(
  'WebCoreDriver','PortPlatformStrategies','LoadingFrameLoaderClient',
  'webcore-driver-stubs','stubs-crypto','stubs-pasteboard','stubs-network','stubs-ax','stubs-other','stubs-loader'
)
foreach ($s in $srcs) {
    if (-not (Test-Path "$P\$s.cpp")) { Write-Host "缺源 $s.cpp" -ForegroundColor Yellow; continue }
    & pwsh -NoProfile -File "$P\compile-driver-jit.ps1" "$P\$s.cpp" "$P\$s.jit.obj" | Out-Null
    if (-not (Test-Path "$P\$s.jit.obj") -or (Get-Item "$P\$s.jit.obj").LastWriteTime -lt (Get-Item "$P\$s.cpp").LastWriteTime) {
        Write-Host "!! $s.cpp(JIT)编译失败,看 driver-compile-jit.log" -ForegroundColor Red
        Get-Content "$P\driver-compile-jit.log" -Tail 25; exit 1
    }
}
$objs = $srcs | ForEach-Object { "$P\$_.jit.obj" }

$lld = 'C:\Program Files\LLVM\bin\lld-link.exe'
$libs = @(
  'WebCore.lib','JavaScriptCore.lib','PAL.lib','WTF.lib','JavaScriptCore.lib','WTF.lib'
  'libcurl.lib','libssl.lib','libcrypto.lib'
  'cairo.lib','pixman-1.lib','freetype.lib','fontconfig.lib','libexpat.lib','harfbuzz.lib'
  'jpeg.lib','libpng16.lib','libwebp.lib','libwebpdemux.lib','libsharpyuv.lib'
  'libxml2.lib','sqlite3.lib','z.lib','bz2.lib','brotlidec.lib','brotlicommon.lib'
  'icuuc.lib','icuin.lib','icudt.lib','WindowsApp.lib'
)
$log = "$P\link-driver-jit.log"
& $lld /DLL /MACHINE:ARM /OUT:"$P\WebCoreDriver-jit.dll" `
    $objs `
    /LIBPATH:"E:\Apotheosis\build-clang-jit\lib" `
    /LIBPATH:"C:\vcpkg\installed\arm-uwp\lib" `
    /LIBPATH:"C:\icu-arm-uwp\lib" `
    $libs `
    /INCLUDE:WebCoreRenderHtml /EXPORT:WebCoreRenderHtml /EXPORT:WebCoreLoadUrl `
    /OPT:REF /OPT:NOICF /INCREMENTAL:NO /errorlimit:0 `
    *> $log
$code = $LASTEXITCODE
$unres = @(Select-String -Path $log -Pattern 'error: undefined symbol:')
$dup   = @(Select-String -Path $log -Pattern 'duplicate symbol|already defined')
Write-Host "[link-driver-jit] EXIT=$code  undefined=$($unres.Count)  duplicate=$($dup.Count)  log=$log"
if ($code -eq 0) {
  & 'C:\Program Files\LLVM\bin\llvm-lib.exe' /OUT:"$P\WebCoreDriver-jit.lib" $objs | Out-Null
  Write-Host "🎉 JIT 驱动链接成功! WebCoreDriver-jit.lib 归档($($objs.Count) obj)" -ForegroundColor Green
}
else {
  if ($dup.Count) { Write-Host "--- 重复符号 ---"; $dup | ForEach-Object { ($_.Line -replace '.*(duplicate symbol|already defined):? ?','').Trim() } | Sort-Object -Unique | Select-Object -First 30 }
  if ($unres.Count) { Write-Host "--- 未定义符号 ---"; $unres | ForEach-Object { ($_.Line -replace '.*error: undefined symbol: ','').Trim() } | Sort-Object -Unique | Select-Object -First 40 }
}
