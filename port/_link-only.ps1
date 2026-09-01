$ErrorActionPreference = 'Stop'
. 'E:\Apotheosis\port\arm32-uwp-env.ps1' *> $null

$P = 'E:\Apotheosis\port'
$srcs = @('WebCoreDriver','PortPlatformStrategies','LoadingFrameLoaderClient','PortNetworkStorageSession','PortChromeClient',
  'webcore-driver-stubs','stubs-crypto','stubs-pasteboard','stubs-network','stubs-ax','stubs-other','stubs-loader')
$objs = $srcs | ForEach-Object { "$P\$_.gpu.obj" }

$lld = 'C:\Program Files\LLVM\bin\lld-link.exe'
$libs = @(
  'WebCore.lib','JavaScriptCore.lib','PAL.lib','WTF.lib','JavaScriptCore.lib','WTF.lib'
  'libEGL.lib','libGLESv2.lib'
  'libcurl.lib','libssl.lib','libcrypto.lib'
  'cairo.lib','pixman-1.lib','freetype.lib','fontconfig.lib','libexpat.lib','harfbuzz.lib'
  'jpeg.lib','libpng16.lib','libwebp.lib','libwebpdemux.lib','libsharpyuv.lib'
  'libxml2.lib','sqlite3.lib','z.lib','bz2.lib','brotlidec.lib','brotlicommon.lib'
  'icuuc.lib','icuin.lib','icudt.lib','WindowsApp.lib'
)

# all C ABI exports the harness needs (keep in sync with port/WebCoreDriver.h)
$exports = @(
  'WebCoreRenderHtml','WebCoreLoadUrl',
  'WebCoreSetCACertPath','WebCoreSetCACertBlob',
  'WebCoreSetCookieJarPath','WebCoreSetCookieJsonPath','WebCoreFlushCookiesToDisk',
  'WebCoreReleaseMemory','WebCoreClearCookies',
  'WebCoreGetLastError','WebCoreGetDiag','WebCoreGetTitle','WebCoreGetUrl',
  'WebCoreDownload','WebCoreGetLinkCount','WebCoreGetLink',
  'WebCoreSessionLoad','WebCoreSessionPaint','WebCoreCloseSession',
  'WebCoreClickAt','WebCoreScrollBy','WebCoreSyncLinks',
  'WebCoreEditDebug','WebCoreFocusedEditable','WebCoreTypeText','WebCoreKeyAction',
  'WebCoreSetUserAgentMobile','WebCoreSetUserAgentString',
  'WebCoreEnableCompositing','WebCoreGpuInit','WebCoreGpuLayerInfo','WebCoreGpuSetFlip',
  'WebCoreComposite','WebCoreCompositeReadback',
  'WebCoreEvalJS','WebCoreLiveTick','WebCoreGetPendingResourceCount','WebCoreGetFrameHash',
  'WebCoreFindString','WebCoreFindNext','WebCoreFindClear',
  'WebCoreGetPageScale','WebCoreSetPageScale'
)
$expArgs = @(); foreach ($e in $exports) { $expArgs += "/EXPORT:$e" }

& $lld /DLL /MACHINE:ARM /OUT:"$P\WebCoreDriver-gpu.dll" `
    $objs `
    /LIBPATH:"E:\Apotheosis\build-clang-gpu\lib" `
    /LIBPATH:"E:\Apotheosis\angle\arm" `
    /LIBPATH:"C:\vcpkg\installed\arm-uwp\lib" `
    /LIBPATH:"C:\icu-arm-uwp\lib" `
    $libs `
    /INCLUDE:WebCoreRenderHtml `
    /OPT:REF /OPT:NOICF /INCREMENTAL:NO /errorlimit:0 `
    $expArgs 2>&1 | Tee-Object -FilePath "$P\link-driver-gpu.log"
$exit = $LASTEXITCODE
Write-Host "[link-only] EXIT=$exit"
