// stubs-other.cpp — platform symbol stubs ("other" class) for the WebCore→Win10M render DLL.
// Each definition copies the declaration from its WebCore/PAL header verbatim so the mangled
// name matches what WebCore.lib expects. Bodies are no-ops / safe defaults for the
// layout+paint-only render path. See undef-other.txt for the symbol list.

#include "config.h"

#include "DragImage.h"
#include "Cursor.h"
#include "Icon.h"
#include "SharedMemory.h"
#include "PublicSuffixStore.h"
#include "PlatformKeyboardEvent.h"
#include "HTMLSelectElement.h"
#include "GraphicsLayer.h"
#include "ImageAdapter.h"
#include "Image.h"
#include "IntSize.h"
#include "FloatSize.h"
#include "graphics/SystemFontDatabase.h"
#include "graphics/win/DisplayRefreshMonitorWin.h"
#include <pal/text/KillRing.h>
#include <pal/system/Sound.h>
#include <wtf/Assertions.h>
#include <wtf/NeverDestroyed.h>

// ============================================================================
// MSVC compiler intrinsic barrier. clang-cl on ARM may not provide it as an
// intrinsic; supply an empty extern "C" definition so the reference resolves.
// (Real semantics: a compiler-only reordering barrier — harmless here.)
// ============================================================================
extern "C" void _ReadWriteBarrier() { }

namespace WebCore {

// ============================================================================
// DragImage (DragImage.h). On Win, DragImageRef == HBITMAP (struct HBITMAP__*).
// Render path never drags; return null/empty. risky: none (drag not exercised).
// ============================================================================
IntSize dragImageSize(DragImageRef)
{
    return IntSize();
}

DragImageRef scaleDragImage(DragImageRef, FloatSize)
{
    return nullptr;
}

DragImageRef createDragImageFromImage(Image*, ImageOrientation, GraphicsClient*, float)
{
    return nullptr;
}

void deleteDragImage(DragImageRef)
{
}

// ============================================================================
// Cursor (Cursor.h). ensurePlatformCursor is a private no-op: no platform
// cursor is materialized in the render-only path.
// ============================================================================
void Cursor::ensurePlatformCursor() const
{
}

// SharedCursor (Cursor.h, PLATFORM(WIN)). Destructor no-op (does not own the HCURSOR lifecycle here).
SharedCursor::~SharedCursor()
{
}

// ============================================================================
// Icon (graphics/Icon.h). Destructor + paint are no-ops; file-chooser icons
// are not rendered in this port.
// ============================================================================
Icon::~Icon()
{
}

void Icon::paint(GraphicsContext&, const FloatRect&)
{
}

// ============================================================================
// SharedMemory (SharedMemory.h). No real shared-memory backing in the render
// DLL — allocate/map/createHandle return null/nullopt. RISKY: any IPC/shared
// surface path that depends on these will get null and must handle it.
// ============================================================================
SharedMemory::~SharedMemory()
{
}

RefPtr<SharedMemory> SharedMemory::allocate(size_t)
{
    return nullptr;
}

RefPtr<SharedMemory> SharedMemory::map(Handle&&, Protection, CopyOnWrite)
{
    return nullptr;
}

std::optional<SharedMemory::Handle> SharedMemory::createHandle(Protection)
{
    return std::nullopt;
}

// ============================================================================
// PublicSuffixStore (PublicSuffixStore.h). No libpsl: report nothing is a
// public suffix and no top-private domain. Safe defaults (false / empty).
// ============================================================================
bool PublicSuffixStore::platformIsPublicSuffix(StringView) const
{
    return false;
}

String PublicSuffixStore::platformTopPrivatelyControlledDomain(StringView) const
{
    return String();
}

// ============================================================================
// SystemFontDatabase (graphics/SystemFontDatabase.h). singleton returns a
// static local; platformSystemFontShorthandInfo returns a default-constructed
// info; platformInvalidate is a no-op.
// ============================================================================
SystemFontDatabase& SystemFontDatabase::singleton()
{
    // SystemFontDatabase() is protected; expose it via a trivial derived type
    // so the static local single instance can be constructed.
    struct ConstructibleSystemFontDatabase : SystemFontDatabase {
        ConstructibleSystemFontDatabase() : SystemFontDatabase() { }
    };
    static NeverDestroyed<ConstructibleSystemFontDatabase> database;
    return database.get();
}

SystemFontDatabase::SystemFontShorthandInfo SystemFontDatabase::platformSystemFontShorthandInfo(FontShorthand)
{
    return SystemFontShorthandInfo { AtomString(), 0, FontSelectionValue() };
}

void SystemFontDatabase::platformInvalidate()
{
}

// ============================================================================
// DisplayRefreshMonitorWin (graphics/win/DisplayRefreshMonitorWin.h). No display
// link in this port — return null. RISKY: animation/refresh driving that relies
// on a real monitor will get null.
// ============================================================================
RefPtr<DisplayRefreshMonitorWin> DisplayRefreshMonitorWin::create(PlatformDisplayID)
{
    return nullptr;
}

// ============================================================================
// PlatformKeyboardEvent (PlatformKeyboardEvent.h). Type/Modifier are aliases of
// PlatformEventType / PlatformEventModifier (hence the mangled enum names).
// No live keyboard state in the render path: empty modifier set, no-op disambiguate.
// ============================================================================
OptionSet<PlatformEvent::Modifier> PlatformKeyboardEvent::currentStateOfModifierKeys()
{
    return { };
}

void PlatformKeyboardEvent::disambiguateKeyDownEvent(Type, bool)
{
}

// ============================================================================
// GraphicsLayer (graphics/GraphicsLayer.h). Type == GraphicsLayerType. The
// non-compositing render path must never construct a GraphicsLayer.
// GPU 构建(USE_TEXTURE_MAPPER)由 GraphicsLayerTextureMapper.cpp 提供真实现 →
// 这里只在软件构建(无 texmap)给 stub,否则与真实现重复符号(M2 链接撞 duplicate)。
// ============================================================================
#if !USE(TEXTURE_MAPPER)
Ref<GraphicsLayer> GraphicsLayer::create(GraphicsLayerFactory*, GraphicsLayerClient&, Type)
{
    RELEASE_ASSERT_NOT_REACHED();
}
#endif

// ============================================================================
// ImageAdapter (graphics/ImageAdapter.h). loadPlatformResource returns the
// shared null Image (1x1-empty equivalent) so callers get a valid Ref instead
// of crashing; invalidate is a no-op.
// ============================================================================
Ref<Image> ImageAdapter::loadPlatformResource(const char*)
{
    return Ref { Image::nullImage() };
}

void ImageAdapter::invalidate()
{
}

// ============================================================================
// HTMLSelectElement (html/HTMLSelectElement.h). No platform-specific keydown
// handling: report "not handled".
// ============================================================================
bool HTMLSelectElement::platformHandleKeydownEvent(KeyboardEvent*)
{
    return false;
}

} // namespace WebCore

// ============================================================================
// PAL bits.
// ============================================================================
namespace PAL {

// KillRing (pal/text/KillRing.h). Editing kill-ring not used: all no-ops / empty.
void KillRing::append(const String&)
{
}

void KillRing::prepend(const String&)
{
}

String KillRing::yank()
{
    return String();
}

void KillRing::startNewSequence()
{
}

void KillRing::setToYankedState()
{
}

// systemBeep (pal/system/Sound.h). No-op.
void systemBeep()
{
}

} // namespace PAL
