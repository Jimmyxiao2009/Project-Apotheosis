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
#include <wtf/HashSet.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

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
// PublicSuffixStore (PublicSuffixStore.h).
//
// This answers "where does the registry end and somebody's own site begin":
// www.example.com and cdn.example.com have the same owner, a.example.co.uk and
// b.other.co.uk do not. RegistrableDomain is built on it, and so is the cookie
// jar's accept policy (CookieJarDB::checkCookieAcceptPolicy).
//
// These two used to return "nothing is a public suffix" / "no top-private
// domain", which sounds harmless but is not: RegistrableDomain falls back to
// the *whole host* when the store returns nothing, so the registrable domain of
// www.example.com came out as "www.example.com" and never matched any other
// host of the same site. The cookie jar then refused a Set-Cookie from a sibling
// subdomain, and - because the read path filters by registrable domain too -
// would not have sent such a cookie either. Result: every site that hands out a
// session, auth or bot-check cookie from one subdomain and reads it from
// another was quietly broken, while the first-party case (host == host) worked,
// so it looked like a rendering problem rather than a cookie one.
//
// The GTK port answers this from libsoup's copy of the Public Suffix List. We
// have no libsoup, so the list travels inside the appx as a data file and the
// harness hands it over at startup (WebCoreSetPublicSuffixListBlob below). The
// algorithm is the one publicsuffix.org specifies: longest matching rule wins,
// "*" matches one label, a "!" rule is an exception, and a host that matches no
// rule at all falls under the implicit "*" rule.
//
// Two deliberate limits: the list's internationalised rules are in Unicode, and
// the hosts we see are punycode, so an IDN registry falls through to the
// implicit rule (last label) - the same answer the default rule gives. And with
// no data file at all we keep the old "nothing is a public suffix" answer for
// isPublicSuffix, but return the last two labels for the top-private domain
// instead of nothing, which is what the GTK port does for hosts libsoup does
// not know and is strictly closer to the truth than the whole host.
// ============================================================================
namespace {

struct PublicSuffixData {
    HashSet<String> rules;        // "com", "co.uk", "*.compute.amazonaws.com"
    HashSet<String> exceptions;   // "!city.kobe.jp" is stored as "city.kobe.jp"
    bool loaded { false };
};

PublicSuffixData& publicSuffixData()
{
    static NeverDestroyed<PublicSuffixData> data;
    return data;
}

// Offsets at which each label of `host` starts (0, then every index after a dot).
Vector<unsigned, 8> labelStarts(const String& host)
{
    Vector<unsigned, 8> starts;
    starts.append(0);
    for (unsigned i = 0; i < host.length(); ++i) {
        if (host[i] == '.' && i + 1 < host.length())
            starts.append(i + 1);
    }
    return starts;
}

// The public suffix of an already-lowercased host, per the publicsuffix.org
// algorithm. Never empty: with no matching rule the implicit "*" rule makes the
// last label the suffix.
String publicSuffixOf(const String& host)
{
    auto& data = publicSuffixData();
    auto starts = labelStarts(host);
    const unsigned labels = starts.size();

    // Longest candidate first, so the first hit is the prevailing rule.
    for (unsigned i = 0; i < labels; ++i) {
        String candidate = host.substring(starts[i]);

        // An exception rule beats every other rule; the suffix is the rule
        // without its leftmost label.
        if (data.exceptions.contains(candidate))
            return (i + 1 < labels) ? host.substring(starts[i + 1]) : String();

        if (data.rules.contains(candidate))
            return candidate;

        // A "*.rest" rule matches this candidate if "rest" is what follows its
        // own leftmost label.
        if (i + 1 < labels && data.rules.contains(makeString("*."_s, StringView(host).substring(starts[i + 1]))))
            return candidate;
    }

    return host.substring(starts[labels - 1]);
}

// What the GTK port falls back to when the list does not know the host: assume
// the last pair of labels is the site.
String lastTwoLabels(const String& host)
{
    size_t lastDot = host.reverseFind('.');
    if (lastDot == notFound || !lastDot)
        return String();
    size_t previousDot = host.reverseFind('.', lastDot - 1);
    return previousDot == notFound ? host : host.substring(previousDot + 1);
}

} // namespace

bool PublicSuffixStore::platformIsPublicSuffix(StringView domain) const
{
    if (domain.isEmpty() || !publicSuffixData().loaded)
        return false;

    auto host = domain.convertToASCIILowercase();
    return publicSuffixOf(host) == host;
}

String PublicSuffixStore::platformTopPrivatelyControlledDomain(StringView domain) const
{
    // Called with cookie-shaped domains too, which may carry a leading dot.
    unsigned position = 0;
    while (position < domain.length() && domain[position] == '.')
        ++position;
    auto view = domain.substring(position);
    if (view.isEmpty())
        return String();

    auto host = view.convertToASCIILowercase();
    if (!publicSuffixData().loaded)
        return lastTwoLabels(host);

    auto suffix = publicSuffixOf(host);
    // The host is itself a registry (".com", ".co.uk"): nobody owns it, so
    // there is no base domain - the same answer libsoup gives.
    if (suffix.isEmpty() || suffix == host)
        return String();

    // One more label to the left of the suffix.
    unsigned suffixStart = host.length() - suffix.length();
    if (suffixStart < 2)
        return host;
    size_t dot = host.reverseFind('.', suffixStart - 2);
    return dot == notFound ? host : host.substring(dot + 1);
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

// Apotheosis: PlatformKeyboardEvent::currentStateOfModifierKeys /
// disambiguateKeyDownEvent used to be stubbed here; WebCore now compiles
// platform/win/KeyEventWin.cpp with WK_WINUWP guards (no GetKeyState in the
// App Container), so the stubs would collide with WebCore.lib (LNK2005).

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

// ============================================================================
// C ABI: hand the Public Suffix List to the engine.
//
// Same shape as WebCoreSetCACertBlob - the app container cannot read the
// install directory through the engine's own file paths, so the harness reads
// the packaged file and passes the bytes. Call it once, on the engine thread,
// before the first navigation; it returns the number of rules parsed, which the
// harness logs so a device round can tell "the list was there" from "the list
// was missing" without guessing.
//
// The data is the publicsuffix.org file verbatim: "//" comments, blank lines,
// one rule per line, and only the text up to the first whitespace counts.
//
// Declared extern "C" inside the namespace on purpose: C linkage ignores the
// namespace, and this is the only place the (file-local) store is visible.
// ============================================================================
extern "C" int WebCoreSetPublicSuffixListBlob(const uint8_t* data, int len)
{
    auto& store = publicSuffixData();
    store.rules.clear();
    store.exceptions.clear();
    store.loaded = false;
    if (!data || len <= 0)
        return 0;

    const char* text = reinterpret_cast<const char*>(data);
    int pos = 0;
    while (pos < len) {
        int lineEnd = pos;
        while (lineEnd < len && text[lineEnd] != '\n')
            ++lineEnd;

        int tokenEnd = pos;
        while (tokenEnd < lineEnd) {
            const char c = text[tokenEnd];
            if (c == ' ' || c == '\t' || c == '\r')
                break;
            ++tokenEnd;
        }

        const int tokenLength = tokenEnd - pos;
        // Skip blank lines and "//" comments; everything else is a rule.
        if (tokenLength > 0 && !(tokenLength >= 2 && text[pos] == '/' && text[pos + 1] == '/')) {
            auto rule = String::fromUTF8(std::span { text + pos, static_cast<size_t>(tokenLength) }).convertToASCIILowercase();
            if (!rule.isEmpty()) {
                if (rule.startsWith('!')) {
                    auto exception = rule.substring(1);
                    if (!exception.isEmpty())
                        store.exceptions.add(WTF::move(exception));
                } else
                    store.rules.add(WTF::move(rule));
            }
        }

        pos = lineEnd + 1;
    }

    store.loaded = !store.rules.isEmpty();
    return static_cast<int>(store.rules.size());
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
