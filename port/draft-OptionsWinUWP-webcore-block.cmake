# ============================================================================
# WebCore enablement for the WinUWP port (Phase 1).
# INSERT into Source/cmake/OptionsWinUWP.cmake.
#
# Placement:
#   (A) The WEBKIT_OPTION_DEFAULT_PORT_VALUE(...) calls MUST go INSIDE the
#       existing WEBKIT_OPTION_BEGIN()/WEBKIT_OPTION_END() block (add them
#       alongside the existing ENABLE_C_LOOP / ENABLE_JIT lines, before
#       WEBKIT_OPTION_END()).
#   (B) Replace the existing `set(ENABLE_WEBCORE OFF)` line (and ideally the
#       sibling Phase-0 `set(ENABLE_* OFF)` cut list) with the SET_AND_EXPOSE /
#       find_package section below, which goes AFTER WEBKIT_OPTION_END() and
#       AFTER the existing `find_package(ICU ...)` line near the end of file.
# UWP-TODO: this is a first-pass minimal config; expect to flip individual
#   flags as configure/link errors appear.
# ============================================================================

# ----------------------------------------------------------------------------
# (A) Inside WEBKIT_OPTION_BEGIN()/END() — feature toggles.
#     Most of these mirror the OptionsWin.cmake defaults we are overriding.
#     C_LOOP (already set ON in this file) auto-CONFLICTs JIT / WEBASSEMBLY /
#     SAMPLING_PROFILER OFF, so those need no explicit line, but we pin
#     WEBASSEMBLY OFF for self-documentation.
# ----------------------------------------------------------------------------
# WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBASSEMBLY        PRIVATE OFF)  # forced by C_LOOP

# --- graphics / GPU: software only ---
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBGL                 PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBGPU                PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBXR                 PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_GPU_PROCESS           PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_OFFSCREEN_CANVAS      PRIVATE OFF)

# --- media: none (drops MediaFoundation + USE_MEDIA_FOUNDATION wiring) ---
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_VIDEO                 PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEB_AUDIO             PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MEDIA_SOURCE          PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MEDIA_STREAM          PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEB_RTC               PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_LEGACY_ENCRYPTED_MEDIA PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_ENCRYPTED_MEDIA       PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MEDIA_STATISTICS      PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_SPEECH_SYNTHESIS      PRIVATE OFF)

# --- desktop-shell features incompatible with App Container ---
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_DRAG_SUPPORT          PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_CONTEXT_MENUS         PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_FULLSCREEN_API        PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_GAMEPAD               PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_GEOLOCATION           PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_DEVICE_ORIENTATION    PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_NOTIFICATIONS         PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_SPELLCHECK            PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_RESOURCE_USAGE        PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_PERIODIC_MEMORY_MONITOR PRIVATE OFF)

# --- trim other heavy/optional subsystems for first bring-up ---
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_XSLT                  PRIVATE OFF)  # drops LibXslt dep
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MATHML                PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_SMOOTH_SCROLLING      PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_ASYNC_SCROLLING       PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_DARK_MODE_CSS         PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_VARIATION_FONTS       PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_TOUCH_EVENTS          PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_POINTER_LOCK          PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_PAYMENT_REQUEST       PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEB_AUTHN             PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_SERVICE_CONTROLS      PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_CONTENT_EXTENSIONS    PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MHTML                 PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_ATTACHMENT_ELEMENT    PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_APPLICATION_MANIFEST  PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_FTPDIR                PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBDRIVER             PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MINIBROWSER           PRIVATE OFF)
# UWP-TODO: several names some tooling expects (MODEL_ELEMENT, DATALIST,
#   INDEXEDDB, SERVICE_WORKERS) are NOT WEBKIT_OPTION entries in 2.52.4 — they
#   are either always-on or nonexistent; do NOT add them (harmless no-ops at
#   best, configure noise at worst).

# --- USE_* graphics/font backend toggles (these ARE WEBKIT_OPTION-defined) ---
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SKIA                     PRIVATE OFF)  # CRITICAL: select Cairo branch
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_WOFF2                    PRIVATE OFF)  # drops WOFF2 + Brotli REQUIRED
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_AVIF                     PRIVATE OFF)  # drops libavif REQUIRED
# USE_JPEGXL / USE_LCMS default OFF in 2.52.4 — leave unset (no-op).

# ----------------------------------------------------------------------------
# (B) After WEBKIT_OPTION_END() and the existing find_package(ICU ...) line.
#     Replace `set(ENABLE_WEBCORE OFF)` with the line below + the
#     SET_AND_EXPOSE_TO_BUILD block + the find_package() calls.
# ----------------------------------------------------------------------------
set(ENABLE_WEBCORE ON)     # <-- was OFF in Phase 0; flips on the WebCore build
# Keep these OFF for Phase 1 (WebCore-only bring-up; no UI process yet):
set(ENABLE_WEBKIT_LEGACY OFF)
set(ENABLE_WEBKIT        OFF)
set(ENABLE_WEBINSPECTORUI OFF)

# Cairo software backend + FreeType/Fontconfig/HarfBuzz font stack.
# USE_CAIRO is auto-set by the find_package(Cairo) branch in OptionsWin.cmake;
# we are NOT including OptionsWin.cmake, so set/expose it ourselves here.
SET_AND_EXPOSE_TO_BUILD(USE_CAIRO          ON)
SET_AND_EXPOSE_TO_BUILD(USE_FREETYPE       ON)
SET_AND_EXPOSE_TO_BUILD(USE_FONTCONFIG     ON)
SET_AND_EXPOSE_TO_BUILD(USE_HARFBUZZ       ON)
SET_AND_EXPOSE_TO_BUILD(USE_CURL           ON)   # only ResourceLoader backend wired
SET_AND_EXPOSE_TO_BUILD(USE_OPENSSL        ON)   # curl TLS/crypto backend
SET_AND_EXPOSE_TO_BUILD(USE_SYSTEM_MALLOC  ON)   # keep Phase-0 system malloc
SET_AND_EXPOSE_TO_BUILD(USE_THEME_ADWAITA  ON)   # Cairo-painted RenderTheme
# Explicitly OFF the accelerated-compositor paths so kept WebCore code does not
# expect TextureMapper/ANGLE/coordinated-graphics symbols:
SET_AND_EXPOSE_TO_BUILD(USE_TEXTURE_MAPPER       OFF)
SET_AND_EXPOSE_TO_BUILD(USE_COORDINATED_GRAPHICS OFF)
SET_AND_EXPOSE_TO_BUILD(USE_ANGLE                OFF)
# UWP-TODO: USE_MEDIA_FOUNDATION must stay OFF (it normally follows ENABLE_VIDEO,
#   which is OFF). Set it explicitly to be safe:
SET_AND_EXPOSE_TO_BUILD(USE_MEDIA_FOUNDATION OFF)

# ----------------------------------------------------------------------------
# find_package for the WebCore deps (modeled on OptionsWin.cmake L46-56 + the
# USE_SKIA=OFF Cairo else-branch). ICU is already found earlier in this file.
# All paths assume vcpkg arm-uwp triplet / our hand-cross-built libs are on
# CMAKE_PREFIX_PATH. Order is informational; CMake resolves regardless.
# ----------------------------------------------------------------------------
find_package(HarfBuzz 1.4.2 REQUIRED COMPONENTS ICU)   # unconditional in 2.52.4
find_package(Cairo    1.18.0 REQUIRED)                 # USE_SKIA=OFF -> Cairo branch
find_package(Freetype REQUIRED)                        # font rasterization (>= 2.9.0)
find_package(Fontconfig REQUIRED)                      # font matching (>= 2.13.0)
find_package(JPEG    1.5.2 REQUIRED)                   # ImageDecoders (unconditional)
find_package(PNG     1.6.34 REQUIRED)                  # ImageDecoders (unconditional)
find_package(WebP    REQUIRED COMPONENTS demux)        # ImageDecoders (unconditional)
find_package(LibXml2 2.9.7 REQUIRED)                   # XML/XHTML/SVG parser (unconditional link)
find_package(SQLite3 3.23.1 REQUIRED)                  # WebStorage/IndexedDB/cookies (unconditional)
find_package(ZLIB    1.2.11 REQUIRED)                  # unconditional link
find_package(CURL    7.87.0 REQUIRED)                  # network backend
find_package(OpenSSL REQUIRED)                         # curl TLS/crypto
find_package(LibPSL  0.20.2 REQUIRED)                  # PublicSuffixStoreCurl
# NOTE: ENABLE_XSLT OFF => no find_package(LibXslt); USE_WOFF2 OFF => no
#   WOFF2/Brotli REQUIRED; USE_AVIF/USE_JPEGXL/USE_LCMS OFF => no avif/jxl/lcms2.
# UWP-TODO: bundled Find modules for Cairo/HarfBuzz/WebP/LibPSL live in
#   Source/cmake/; ensure CMAKE_MODULE_PATH includes them (it does when building
#   in-tree). Freetype/Fontconfig use CMake/pkg-config standard finders — the
#   imported targets are named Freetype::Freetype / Fontconfig::Fontconfig in
#   FreeType.cmake, so the find modules must export those exact target names.