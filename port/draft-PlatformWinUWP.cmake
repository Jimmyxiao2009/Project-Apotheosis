# ============================================================================
# PlatformWinUWP.cmake  —  WebCore platform glue for the WinUWP port
# (EdgeHTML Reborn — ARM32 Win10-Mobile App Container)
#
# Modeled on Source/WebCore/PlatformWin.cmake, but stripped to the
# App-Container-safe subset and re-pointed at the Cairo (software) graphics
# backend + the GTK/WPE-style FreeType/Fontconfig/HarfBuzz font backend.
#
# RULES APPLIED (vs. PlatformWin.cmake):
#   * KEEP  : compiler-only flags; coordinate/struct conversion *Win.cpp;
#             Curl/OpenSSL/ImageDecoders/Cairo glue; portable text/locale;
#             read-only Win helpers (SystemInfo/UserAgent/MIME/Logging).
#   * DROP  : MSAA/oleacc accessibility; clipboard/drag/OLE pasteboard;
#             native <select> popup; CreateWindow fullscreen/overlay;
#             cursor; GDI image/icon/DIB paint; MediaFoundation video;
#             EGL/ANGLE/OpenGL display + GL fences; SharedMemoryWin;
#             Uniscribe + the win/win-cairo GDI/DirectWrite font backend.
#   * REPLACE font block with include(platform/FreeType.cmake) (+Cairo.cmake
#             for the software graphics backend).
#   * DROP  link libs: usp10 (Uniscribe -> HarfBuzz) and the whole
#             MediaFoundation INTERFACE lib (ENABLE_VIDEO/USE_MEDIA_FOUNDATION
#             must be OFF so its block never runs).
#
# STATUS: BEST-EFFORT FIRST DRAFT. This has NOT been configured/compiled.
# Expect to iterate against real configure + clang-cl errors. Every spot
# that is uncertain / likely-to-need-a-stub is marked with "# UWP-TODO".
# Several "kept" Win sources still reference desktop-only APIs at the .cpp
# level and will need NotImplemented stubs (see STUB notes inline + in
# OptionsWinUWP.cmake). The first goal is a CONFIGURE that succeeds and a
# link error surface that tells us exactly which stubs to write.
# ============================================================================

add_definitions(/bigobj -D__STDC_CONSTANT_MACROS)

# --- Form-control theming -------------------------------------------------
# Adwaita paints controls purely via Cairo (no UxTheme/GDI), so it is the
# App-Container-safe RenderTheme. WebCore on Windows normally uses
# RenderThemeWin (GDI/UxTheme) — we deliberately do NOT take that path.
# UWP-TODO: confirm RenderTheme resolves to Adwaita and never UxTheme.
include(platform/Adwaita.cmake)

# --- Networking + crypto + image decoding ---------------------------------
# Curl is the only ResourceLoader backend wired for this port; OpenSSL is its
# TLS/crypto backend. ImageDecoders pulls JPEG/PNG/WebP (all mandatory in
# 2.52.4 — ScalableImageDecoder compiles them unconditionally on non-COCOA).
include(platform/Curl.cmake)
include(platform/ImageDecoders.cmake)
include(platform/OpenSSL.cmake)

# DROPPED vs PlatformWin.cmake L7: include(platform/TextureMapper.cmake)
#   TextureMapper is the GL / coordinated-graphics accelerated compositor.
#   This port is software-only (ENABLE_WEBGL/WEBGPU/GPU_PROCESS OFF,
#   USE_TEXTURE_MAPPER OFF), so it is intentionally NOT included.
#   UWP-TODO: if some kept graphics-layer code references TextureMapper
#   symbols at link time, we may have to either re-add a trimmed subset or
#   force USE_TEXTURE_MAPPER OFF hard (done in OptionsWinUWP.cmake).

# --- Graphics backend: Cairo (software image-surface) ---------------------
# REQUIRED. Cairo.cmake adds SourcesCairo.txt. Note: that unified list still
# contains GraphicsContextGLCairo.cpp, but it is self-guarded by
# '#if ENABLE(WEBGL) && USE(CAIRO)' and compiles to nothing with WEBGL OFF,
# so pulling Cairo.cmake verbatim is safe. The GTK-only GdkPixbuf branch in
# ImageBufferUtilitiesCairo.cpp is inert as long as PLATFORM(GTK) is undefined
# (it is, for PORT=WinUWP) — the active path is cairo-only PNG encode.
if (USE_CAIRO)
    include(platform/Cairo.cmake)
# elseif (USE_SKIA)              # Skia path intentionally unsupported here.
#     include(platform/Skia.cmake)
endif ()

# DROPPED vs PlatformWin.cmake L15-17: if (USE_DAWN) include(platform/Dawn.cmake)
#   WebGPU/Dawn is off for this port.

# --- Font/text backend: FreeType + Fontconfig + HarfBuzz ------------------
# This REPLACES the entire win/win-cairo GDI+DirectWrite+Uniscribe+MLang font
# stack (PlatformWin.cmake L203-214) and SystemFontDatabaseWin (L83).
# FreeType.cmake supplies FontCacheFreeType / FontPlatformDataFreeType /
# FontCustomPlatformDataFreeType / SimpleFontDataFreeType /
# GlyphPageTreeNodeFreeType / FontSetCache / RefPtrFontconfig +
# ComplexTextControllerHarfBuzz / FontDescriptionHarfBuzz, and under USE_CAIRO
# adds platform/graphics/cairo/FontCairoHarfbuzzNG.cpp (the Cairo glyph-draw
# glue the dropped Font*WinCairo files used to provide). It also appends the
# Fontconfig::/Freetype::/HarfBuzz::/HarfBuzz::ICU link libs.
# UWP-TODO (runtime, not cmake): Fontconfig needs a writable cache dir or a
#   baked-in fonts.conf inside the App Container sandbox (FONTCONFIG_PATH /
#   sysroot shim). Out of cmake scope but required to actually render glyphs.
include(platform/FreeType.cmake)

# --------------------------------------------------------------------------
# Private include dirs (trimmed from PlatformWin.cmake L19-30).
#   DROPPED: accessibility/win (MSAA), platform/graphics/egl + opengl (GL),
#            platform/video-codecs (no media).
#   KEPT   : page/win (EventHandler/Frame glue), graphics/opentype (OT/WOFF
#            parsing + FontMemoryResource header), graphics/win (shared
#            headers: IntRectWin/SharedGDIObject/LocalWindowsContext etc.),
#            platform/mediacapabilities, platform/network/win, platform/win.
# UWP-TODO: graphics/win is kept ONLY for headers used by kept conversion
#   files; if it drags in GDI .cpp expectations, prune further.
# --------------------------------------------------------------------------
list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    "${WEBCORE_DIR}/page/win"
    "${WEBCORE_DIR}/platform/graphics/opentype"
    "${WEBCORE_DIR}/platform/graphics/win"
    "${WEBCORE_DIR}/platform/mediacapabilities"
    "${WEBCORE_DIR}/platform/network/win"
    "${WEBCORE_DIR}/platform/win"
)

# --------------------------------------------------------------------------
# Sources (trimmed from PlatformWin.cmake L32-116).
# Every line below was classified App-Container-safe OR is a struct/coordinate
# conversion file touching only POINT/RECT/SIZE/BSTR/XFORM (no forbidden API).
# Files marked "# STUB:" build today but reference desktop-only APIs in their
# .cpp and WILL need a NotImplemented / WinRT replacement before they link or
# run correctly.
# --------------------------------------------------------------------------
list(APPEND WebCore_SOURCES
    # --- page/win: event + frame glue (no HWND message pump needed here) ---
    page/win/EventHandlerWin.cpp
    page/win/FrameWin.cpp

    # --- portable platform bits -------------------------------------------
    platform/LocalizedStrings.cpp           # STUB: guard the AX/context-menu
                                            # string sets that pair with the
                                            # excluded accessibility + context
                                            # menu code (UWP-TODO).

    platform/generic/KeyedDecoderGeneric.cpp
    platform/generic/KeyedEncoderGeneric.cpp

    # --- OpenType / custom-font table parsing (no GDI) --------------------
    platform/graphics/opentype/OpenTypeUtilities.cpp

    # --- graphics/win: struct <-> Float/Int conversions only (no GDI) -----
    platform/graphics/win/FloatPointWin.cpp
    platform/graphics/win/FloatRectWin.cpp
    platform/graphics/win/IntPointWin.cpp
    platform/graphics/win/IntRectWin.cpp
    platform/graphics/win/IntSizeWin.cpp
    platform/graphics/win/TransformationMatrixWin.cpp

    # --- network/win: CA cert load via crypt32 (App-Container-safe) -------
    platform/network/win/CurlSSLHandleWin.cpp
    # DROPPED L87 NetworkStateNotifierWin.cpp: NotifyAddrChange overlapped
    #   iphlpapi notify is not in the App-Container API set.
    #   UWP-TODO: provide a WinRT connectivity-based NetworkStateNotifier, or
    #   accept the generic/no-op notifier (likely a link stub is needed).

    # --- text -------------------------------------------------------------
    platform/text/Hyphenation.cpp           # generic NotImplemented stub
    platform/text/LocaleICU.cpp             # ICU-based, no Win32

    # --- platform/win: read-only / struct helpers -------------------------
    platform/win/BString.cpp                # BSTR/OLE string (allowed)
    platform/win/BitmapInfo.cpp             # fills BITMAPINFO struct (no GDI
                                            # calls); used by Cairo image bridge
    platform/win/KeyEventWin.cpp            # virtual-key translation tables
    platform/win/LoggingWin.cpp             # debug logging
    platform/win/MIMETypeRegistryWin.cpp    # registry read (allowed)
    platform/win/SystemInfo.cpp             # OS version string (read-only)
    platform/win/UserAgentWin.cpp           # UA string from SystemInfo
    platform/win/WebCoreTextRenderer.cpp    # thin FontCascade draw helper
    platform/win/WindowsKeyNames.cpp        # key-name data tables

    # --- STUB sources: build but need a non-desktop reimplementation ------
    platform/win/MainThreadSharedTimerWin.cpp   # STUB: creates a hidden
            # HWND_MESSAGE window (CreateWindow/RegisterClassEx) to drive the
            # shared timer — forbidden in App Container. Replace with a WTF
            # RunLoop / CreateTimerQueueTimer-based MainThreadSharedTimer.
            # UWP-TODO: most likely the first file we must swap for a generic.
    platform/win/WebCoreInstanceHandle.cpp      # STUB: stores HINSTANCE via
            # DllMain — UWP app has no classic DllMain flow. Set instance
            # handle from GetModuleHandleEx / module base at init.
    platform/win/WebCoreBundleWin.cpp           # STUB: resolves WebKit.resources
            # via GetModuleFileName(instanceHandle). Repoint to the UWP package
            # Install Location (Windows.ApplicationModel.Package).
    platform/win/SearchPopupMenuDB.cpp          # STUB: SQLite-backed search
            # autosave; repoint DB to app-data sandbox path or make in-memory.

    # ======================================================================
    # EXPLICITLY DROPPED (was in PlatformWin.cmake) — reasons:
    #   L33-35 accessibility/win/AX*Win.cpp ............ MSAA/IAccessible (oleacc)
    #   L37   editing/win/EditorWin.cpp ................ clipboard/HGLOBAL editing
    #   L39   html/HTMLSelectElementWin.cpp ............ native Win32 popup menu
    #   L41   page/win/DragControllerWin.cpp .......... OLE drag (DoDragDrop)
    #   L44-45 page/win/ResourceUsage*Win.cpp ......... CreateWindow debug overlay
    #   L47   platform/Cursor.cpp ..................... LoadCursor/SetCursor
    #   L49   platform/StaticPasteboard.cpp ........... clipboard
    #   L51   platform/audio/PlatformMediaSessionManager.cpp . media stack
    #   L56   platform/graphics/PlatformDisplay.cpp ... EGL/ANGLE display
    #   L58   .../angle/PlatformDisplayANGLE.cpp ...... ANGLE display
    #   L60-65 .../egl/GL*.cpp ........................ OpenGL/EGL context+fences
    #   L69   .../win/DIBPixelData.cpp ................ GDI DIB pixels (Cairo used)
    #   L70   .../win/DisplayRefreshMonitorWin.cpp .... Win display-link (generic ok)
    #   L73-74 .../win/FullScreen*.cpp ................ CreateWindow fullscreen
    #   L75   .../win/GraphicsContextWin.cpp .......... GDI HDC bridge
    #         UWP-TODO: if kept code calls getWindowsContext/releaseWindowsContext
    #         we must add a NotImplemented stub instead of re-adding this file.
    #   L76   .../win/IconWin.cpp ..................... HICON/LoadImage GDI
    #   L77   .../win/ImageAdapterWin.cpp ............. GDI HBITMAP (Cairo used)
    #   L81   .../win/MediaPlayerPrivateMediaFoundation.cpp . MediaFoundation
    #   L82   .../win/PlatformDisplayWin.cpp .......... EGL/ANGLE display
    #   L83   .../win/SystemFontDatabaseWin.cpp ....... Win font enum (Fontconfig used)
    #   L94   .../win/ClipboardUtilitiesWin.cpp ....... clipboard
    #   L95   .../win/CursorWin.cpp .................... cursor
    #   L96   .../win/DragDataWin.cpp ................. OLE drag IDataObject
    #   L97   .../win/GDIUtilities.cpp ................ GDI GetDeviceCaps/DPI
    #         UWP-TODO: if DPI is needed, replace with WinRT DisplayInformation.
    #   L102  .../win/PasteboardWin.cpp .............. clipboard
    #   L103  .../win/PlatformMouseEventWin.cpp ....... WM_MOUSE* message pump
    #   L104  .../win/PlatformScreenWin.cpp ........... MonitorFromWindow/HostWindow
    #         UWP-TODO: needs a WinRT DisplayInformation-backed PlatformScreen stub
    #         (PlatformScreen is referenced broadly — likely a required stub).
    #   L106  .../win/SharedMemoryWin.cpp ............. CreateFileMapping named SHM
    #   L109  .../win/WCDataObject.cpp ............... OLE IDataObject
    #   L113  .../win/WheelEventWin.cpp .............. WM_WHEEL message pump
    #   L114  .../win/WindowMessageBroadcaster.cpp .... HWND WndProc subclassing
    # ======================================================================
)

# --------------------------------------------------------------------------
# Private framework headers (trimmed from PlatformWin.cmake L118-145).
# Keep headers needed to compile the kept .cpp + widely-included COM/GDI-type
# RAII wrapper headers (harmless includes even when their GDI paths are unused).
# --------------------------------------------------------------------------
list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
    page/win/FrameWin.h

    platform/graphics/opentype/FontMemoryResource.h  # RAII in-memory font
            # (AddFontMemResourceEx). Header KEPT; impl may need a stub.

    platform/graphics/win/DIBPixelData.h        # used by BitmapInfo/Cairo bridge
    platform/graphics/win/LocalWindowsContext.h  # RAII HDC wrapper header
    platform/graphics/win/SharedGDIObject.h      # RAII GDI handle template

    platform/win/BString.h
    platform/win/BitmapInfo.h
    platform/win/COMPtr.h                        # COM smart ptr, widely included
    platform/win/HWndDC.h                        # header only (impl GDI-stubbed)
    platform/win/SearchPopupMenuDB.h
    platform/win/SystemInfo.h
    platform/win/WebCoreBundleWin.h
    platform/win/WebCoreTextRenderer.h
    platform/win/WindowsKeyNames.h

    # DROPPED headers (with their .cpp):
    #   accessibility/win/AccessibilityObjectWrapperWin.h (MSAA)
    #   platform/graphics/win/FullScreenController*.h / FullScreenWindow.h
    #   platform/win/GDIUtilities.h (GDI)
    #   platform/win/WCDataObject.h (OLE)
    #   platform/win/WindowMessageBroadcaster.h / WindowMessageListener.h (HWND)
)

# --------------------------------------------------------------------------
# Link libraries (trimmed from PlatformWin.cmake L147-151).
#   KEEP : crypt32  (cert store for OpenSSL/Curl) — App-Container-safe
#          iphlpapi (adapter info)                — App-Container-safe
#   DROP : usp10    (Uniscribe) — replaced by HarfBuzz::HarfBuzz / HarfBuzz::ICU
#          (those come from FreeType.cmake above).
# UWP-TODO: in App Container, link against WindowsApp.lib (umbrella) instead of
#   discrete desktop import libs where possible; crypt32/iphlpapi are believed
#   App-Container-safe but verify against the device's allowed import set.
# --------------------------------------------------------------------------
list(APPEND WebCore_LIBRARIES
    crypt32
    iphlpapi
)

# WebCoreTestSupport is not built for this port (ENABLE_API_TESTS OFF), so the
# shlwapi test-support lib (PlatformWin.cmake L153-155) is intentionally omitted.

# --------------------------------------------------------------------------
# Resources: broken-image / resize-corner icons used during paint, plus the
# modern-media-control images (harmless copies). Kept from L157-171.
# --------------------------------------------------------------------------
set(iconFiles
    Resources/missingImage.png
    Resources/missingImage@2x.png
    Resources/missingImage@3x.png
    Resources/panIcon.png
    Resources/textAreaResizeCorner.png
    Resources/textAreaResizeCorner@2x.png
)

file(COPY ${iconFiles} DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/WebKit.resources/icons)

file(COPY ${ModernMediaControlsImageFiles}
    DESTINATION
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/WebKit.resources/media-controls
)

# --------------------------------------------------------------------------
# DROPPED block: PlatformWin.cmake L173-199 — the MediaFoundation INTERFACE
# library (d3d9/dwrite/dxva2/evr/mf/mfplat/mfuuid/strmiids + /DELAYLOAD).
# It is gated by `if (ENABLE_VIDEO AND USE_MEDIA_FOUNDATION)`; both are forced
# OFF in OptionsWinUWP.cmake, so the block never runs. This is also how dwrite
# (the only other DirectWrite reference) is dropped. We do NOT replicate it.
# --------------------------------------------------------------------------

# --------------------------------------------------------------------------
# Graphics-only Cairo glue (the NON-font survivors of PlatformWin.cmake's
# `if (USE_CAIRO)` block L201-220). The font lines L203-214 are REPLACED by
# include(platform/FreeType.cmake) above. The media + drag lines are dropped.
#   KEEP  : GraphicsContextWinCairo.cpp (the Cairo GraphicsContext impl)
#           ImageAdapterWinCairo.cpp    (Cairo image<->surface adapter)
#   DROP  : ComplexTextControllerUniscribe / FontCacheWin / FontDescriptionWin
#           / FontPlatformDataWin / FontWin / GlyphPageTreeNodeWin /
#           SimpleFontDataWin / FontCustomPlatformDataWin (L203-210)  -> FreeType
#           cairo/FontCacheWinCairo / FontCustomPlatformDataWinCairo /
#           FontPlatformDataWinCairo (L212-214)                       -> FreeType
#           cairo/MediaPlayerPrivateMediaFoundationCairo.cpp (L217)   -> no media
#           cairo/DragImageWinCairo.cpp via platform/win/cairo (L219) -> no drag
# UWP-TODO: GraphicsContextWinCairo.cpp may reference getWindowsContext (the
#   GDI HDC bridge from the dropped GraphicsContextWin.cpp). If so, add a
#   NotImplemented stub for getWindowsContext/releaseWindowsContext rather than
#   re-introducing GDI. This is a likely first compile/link failure.
# --------------------------------------------------------------------------
if (USE_CAIRO)
    list(APPEND WebCore_SOURCES
        platform/graphics/win/cairo/GraphicsContextWinCairo.cpp
        platform/graphics/win/cairo/ImageAdapterWinCairo.cpp
    )
endif ()

# --------------------------------------------------------------------------
# WOFF2 web-font decompression (PlatformWin.cmake L229-236). Pure libs, but
# USE_WOFF2 is OFF for the minimal Phase-1 config (avoids the Brotli/WOFF2
# REQUIRED find_package). Block kept verbatim so flipping USE_WOFF2 ON later
# "just works".
# --------------------------------------------------------------------------
if (USE_WOFF2)
    list(APPEND WebCore_LIBRARIES
        Brotli::dec
        WOFF2::common
    )
endif ()

# DROPPED: PlatformWin.cmake L238-240 `if (USE_SKIA) ... SHARPYUV_LIBS` —
# Skia is OFF; never built under USE_CAIRO.