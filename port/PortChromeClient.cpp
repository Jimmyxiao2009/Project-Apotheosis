/*
 * PortChromeClient.cpp — out-of-line method bodies for WebCorePort::PortChromeClient.
 *
 * Bodies mirror WebCore's EmptyChromeClient (EmptyClients.cpp). The
 * popup-menu factories return nullptr (EmptyChromeClient returns a file-local
 * EmptyPopupMenu/EmptySearchPopupMenu; nullptr is a valid no-op here since the
 * headless port never shows native popups).
 */

#include "config.h"
#include "PortChromeClient.h"

#include <WebCore/ColorChooser.h>
#include <WebCore/CookieConsentDecisionResult.h>
#include <WebCore/DataListSuggestionPicker.h>
#include <WebCore/DateTimeChooser.h>
#include <WebCore/Icon.h>
#include <WebCore/PopupMenu.h>
#include <WebCore/SearchPopupMenu.h>
#include <wtf/CompletionHandler.h>
#include <wtf/text/CString.h>   // String::utf8() (addMessageToConsole -> consoleLogAppend)
#include <array>

namespace WebCorePort {

using namespace WebCore;

RefPtr<PopupMenu> PortChromeClient::createPopupMenu(PopupMenuClient&) const
{
    return nullptr;
}

RefPtr<SearchPopupMenu> PortChromeClient::createSearchPopupMenu(PopupMenuClient&) const
{
    return nullptr;
}

RefPtr<ColorChooser> PortChromeClient::createColorChooser(ColorChooserClient&, const Color&)
{
    return nullptr;
}

RefPtr<DataListSuggestionPicker> PortChromeClient::createDataListSuggestionPicker(DataListSuggestionsClient&)
{
    return nullptr;
}

RefPtr<DateTimeChooser> PortChromeClient::createDateTimeChooser(DateTimeChooserClient&)
{
    return nullptr;
}

void PortChromeClient::setTextIndicator(RefPtr<TextIndicator>&&) const
{
}

void PortChromeClient::updateTextIndicator(RefPtr<TextIndicator>&&) const
{
}

void PortChromeClient::runOpenPanel(LocalFrame&, FileChooser&)
{
}

void PortChromeClient::showShareSheet(ShareDataWithParsedURL&&, CompletionHandler<void(bool)>&&)
{
}

void PortChromeClient::requestCookieConsent(CompletionHandler<void(CookieConsentDecisionResult)>&& completion)
{
    completion(CookieConsentDecisionResult::NotSupported);
}

RefPtr<Icon> PortChromeClient::createIconForFiles(const Vector<String>& /* filenames */)
{
    return nullptr;
}

// Apotheosis: mirror JS console output to LocalState\console.txt via WebCoreDriver.cpp's
// consoleLogAppend() (declared above, in this namespace). sourceID (script/URL) + lineNumber is
// the "where" a console line actually needs — MessageSource (JS/Network/CSS/...) is a coarser
// category the caller can't use as well, so it is left unnamed here (matches the rest of this
// file's convention for unused ChromeClient params).
void PortChromeClient::addMessageToConsole(JSC::MessageSource, JSC::MessageLevel level, const String& message,
    unsigned lineNumber, unsigned /* columnNumber */, const String& sourceID)
{
    static constexpr std::array<const char*, 5> levelStrings { "log", "warning", "error", "debug", "info" };
    const unsigned levelIndex = static_cast<unsigned>(level);
    const char* levelStr = levelIndex < levelStrings.size() ? levelStrings[levelIndex] : "?";
    consoleLogAppend(levelStr, sourceID.utf8().data(), lineNumber, message.utf8().data());
}

} // namespace WebCorePort
