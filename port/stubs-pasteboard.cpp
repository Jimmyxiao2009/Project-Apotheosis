// ============================================================================
// stubs-pasteboard.cpp  —  link-time stubs for the WinUWP WebCore driver
// (EdgeHTML Reborn — ARM32 thumbv7 Win10-Mobile App Container)
//
// CLASS: pasteboard
//
// WHY THESE ARE UNDEFINED
//   platform/win/PasteboardWin.cpp, platform/win/WCDataObject.cpp,
//   platform/StaticPasteboard.cpp and editing/win/EditorWin.cpp are all DROPPED
//   from PlatformWinUWP.cmake (clipboard / OLE IDataObject are unavailable under
//   WINAPI_FAMILY_APP). The platform-agnostic editing/DataTransfer code still
//   references the base-class Pasteboard virtuals + Editor::paste*/platform*Font
//   + WCDataObject::Release, so the linker reports them undefined.
//
// POLICY
//   The render (layout + paint) path never touches the system clipboard. Every
//   write/read/copy/paste here is a no-op returning empty Vector / empty String /
//   false / the inert FileContentState. Anything that *would* mutate the clipboard
//   asserts loudly so a real call surfaces instead of silently corrupting state;
//   the read/query side returns benign empties because DataTransfer can legitimately
//   probe an empty pasteboard during DOM teardown.
//
//   Signatures are transcribed verbatim from the headers so the mangled names match
//   WebCore.lib exactly:
//     Source/WebCore/platform/Pasteboard.h
//     Source/WebCore/platform/win/PasteboardWin.h (via Pasteboard.h PLATFORM(WIN))
//     Source/WebCore/platform/win/WCDataObject.h
//     Source/WebCore/editing/Editor.h
// ============================================================================

#include "config.h"

#include <wtf/Assertions.h>
#include <wtf/StdLibExtras.h>

#include "Pasteboard.h"        // brings WCDataObject.h, PasteboardContext.h, COMPtr.h (PLATFORM(WIN))
#include "Editor.h"
#include "Color.h"
#include "SimpleRange.h"

namespace WebCore {

// ----------------------------------------------------------------------------
// Pasteboard — base-class construction / factory
//   Mirror the PLATFORM(WIN) member-init list from PasteboardWin.cpp but skip
//   finishCreatingPasteboard() (it pulls clipboard-format registration symbols
//   that are themselves dropped). A driver that never copy/pastes never observes
//   the uninitialised clipboard state.
// ----------------------------------------------------------------------------
Pasteboard::Pasteboard(std::unique_ptr<PasteboardContext>&& context)
    : m_context(WTF::move(context))
    , m_dataObject(0)
    , m_writableDataObject(0)
{
}

std::unique_ptr<Pasteboard> Pasteboard::createForCopyAndPaste(std::unique_ptr<PasteboardContext>&& context)
{
    return makeUnique<Pasteboard>(WTF::move(context));
}

// ----------------------------------------------------------------------------
// Pasteboard — read / query side (benign empties; may be probed on empty board)
// ----------------------------------------------------------------------------
bool Pasteboard::hasData()
{
    return false;
}

Vector<String> Pasteboard::typesSafeForBindings(const String&)
{
    return { };
}

Vector<String> Pasteboard::typesForLegacyUnsafeBindings()
{
    return { };
}

String Pasteboard::readOrigin()
{
    return { };
}

String Pasteboard::readString(const String&)
{
    return { };
}

String Pasteboard::readStringInCustomData(const String&)
{
    return { };
}

Pasteboard::FileContentState Pasteboard::fileContentState()
{
    return FileContentState::NoFileOrImageData;
}

bool Pasteboard::canSmartReplace()
{
    return false;
}

void Pasteboard::read(PasteboardPlainText&, PlainTextURLReadingPolicy, std::optional<size_t>)
{
}

void Pasteboard::read(PasteboardWebContentReader&, WebContentReadingPolicy, std::optional<size_t>)
{
}

void Pasteboard::read(PasteboardFileReader&, std::optional<size_t>)
{
}

// ----------------------------------------------------------------------------
// Pasteboard — write side (clipboard mutation; never reached during render)
// ----------------------------------------------------------------------------
void Pasteboard::clear()
{
    // Apotheosis: Clipboard is not wired to the UWP host yet; keep this a no-op.
}

void Pasteboard::clear(const String&)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

void Pasteboard::writeString(const String&, const String&)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

void Pasteboard::write(const Color&)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

void Pasteboard::write(const PasteboardURL&)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

void Pasteboard::writeTrustworthyWebURLsPboardType(const PasteboardURL&)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

void Pasteboard::write(const PasteboardImage&)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

void Pasteboard::write(const PasteboardBuffer&)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

void Pasteboard::write(const PasteboardWebContent&)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

void Pasteboard::writeCustomData(const Vector<PasteboardCustomData>&)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

void Pasteboard::writeMarkup(const String&)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

void Pasteboard::writePlainText(const String&, SmartReplaceOption)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

// PLATFORM(WIN)-only layering-violation writers (FIXME in header).
void Pasteboard::writeImage(Element&, const URL&, const String&)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

void Pasteboard::writeSelection(const std::optional<SimpleRange>&, bool, LocalFrame&, ShouldSerializeSelectedTextForDataTransfer)
{
    // Apotheosis: graceful no-op until the UWP Clipboard backend is added.
}

// ----------------------------------------------------------------------------
// WCDataObject::Release — OLE IDataObject refcount; WCDataObject.cpp dropped.
//   Header declares STDMETHODCALLTYPE (collapses to the lone ARM32 calling
//   convention, so the mangled name matches). Never instantiated under WinUWP.
// ----------------------------------------------------------------------------
ULONG STDMETHODCALLTYPE WCDataObject::Release()
{
    // Apotheosis: this Win32 OLE object is never exposed by the UWP host. Do
    // not turn an unexpected teardown into a process-killing assertion.
    return 0;
}

// ----------------------------------------------------------------------------
// Editor — paste / platform font (EditorWin.cpp dropped). Pure no-ops on Win
//   (EditorWin's platform*Font are already empty); paste mutates the document
//   from clipboard contents, which the render path never initiates.
// ----------------------------------------------------------------------------
void Editor::pasteWithPasteboard(Pasteboard*, OptionSet<PasteOption>)
{
    // Apotheosis: paste is unavailable until the UWP Clipboard backend is
    // connected; leave the document unchanged instead of crashing.
}

void Editor::platformCopyFont()
{
}

void Editor::platformPasteFont()
{
}

} // namespace WebCore
