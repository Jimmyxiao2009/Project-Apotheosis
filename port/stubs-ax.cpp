/*
 * stubs-ax.cpp — platform accessibility stubs for the Win10Mobile (ARM32 thumbv7 UWP)
 * WebCore render DLL. The accessibility backend (MSAA/UIA wrapper) is not compiled
 * for this port, so these platform-specific entry points are provided as no-ops /
 * default-returning definitions. Signatures are copied verbatim from the WebCore
 * headers so the mangled names match what WebCore.lib expects.
 *
 * Symbols covered (see port/undef-ax.txt):
 *   WebCore::AXObjectCache::attachWrapper(AccessibilityObject&)
 *   WebCore::AXObjectCache::frameLoadingEventPlatformNotification(RenderView*, AXLoadingEvent)
 *   WebCore::AXObjectCache::nodeTextChangePlatformNotification(AccessibilityObject*, AXTextChange, unsigned, const String&)
 *   WebCore::AXObjectCache::platformHandleFocusedUIElementChanged(Element*, Element*)
 *   WebCore::AXObjectCache::platformPerformDeferredCacheUpdate()
 *   WebCore::AXObjectCache::postPlatformNotification(AccessibilityObject&, AXNotification)
 *   WebCore::AXObjectCache::handleScrolledToAnchor(const Node&)
 *   WebCore::AccessibilityObject::detachPlatformWrapper(AccessibilityDetachmentType)
 *   WebCore::AccessibilityObject::accessibilityIgnoreAttachment() const
 *   WebCore::AccessibilityObject::accessibilityPlatformIncludesObject() const
 */

#include "config.h"

#include "AXObjectCache.h"
#include "AccessibilityObject.h"

namespace WebCore {

// --- AXObjectCache platform notifications (no AX backend → no-op) ---

void AXObjectCache::attachWrapper(AccessibilityObject&)
{
    // No platform wrapper to attach on this port.
}

void AXObjectCache::postPlatformNotification(AccessibilityObject&, AXNotification)
{
    // No accessibility client to notify.
}

void AXObjectCache::nodeTextChangePlatformNotification(AccessibilityObject*, AXTextChange, unsigned, const String&)
{
    // No accessibility client to notify.
}

void AXObjectCache::frameLoadingEventPlatformNotification(RenderView*, AXLoadingEvent)
{
    // No accessibility client to notify.
}

void AXObjectCache::platformHandleFocusedUIElementChanged(Element*, Element*)
{
    // No accessibility client to notify.
}

void AXObjectCache::platformPerformDeferredCacheUpdate()
{
    // No platform-side deferred work without an AX backend.
}

void AXObjectCache::handleScrolledToAnchor(const Node&)
{
    // No accessibility client to post the scroll notification to.
}

// --- AccessibilityObject platform / attachment hooks ---

void AccessibilityObject::detachPlatformWrapper(AccessibilityDetachmentType)
{
    // No platform wrapper exists on this port.
}

bool AccessibilityObject::accessibilityIgnoreAttachment() const
{
    return false;
}

AccessibilityObjectInclusion AccessibilityObject::accessibilityPlatformIncludesObject() const
{
    return AccessibilityObjectInclusion::DefaultBehavior;
}

} // namespace WebCore
