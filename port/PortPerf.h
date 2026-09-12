// ============================================================================
// PortPerf.h — Apotheosis: driver-internal perf-probe API (M4 step 1).
//
// NOT part of the C ABI (that lives in WebCoreDriver.h). The PerfLog itself —
// ring buffer, CSV writer, all state — is file-local to WebCoreDriver.cpp; this
// header only lets the port-layer clients mark the few phase boundaries they
// are the sole observers of. Today that is LoadingFrameLoaderClient, which sees
// the navigation milestones (provisional start / commit / DOM ready / load).
//
// Every entry point is a no-op unless the harness switched logging on with
// WebCoreSetPerfLogPath(), so the default cost is one predictable branch.
// Engine thread only (like the driver's other g_last* state).
// ============================================================================

#pragma once

namespace WebCorePort {

void perfNavStart();         // dispatchDidStartProvisionalLoad — starts the network clock
void perfNavCommit();        // dispatchDidCommitLoad          — ms_net_commit
void perfNavDocumentReady(); // dispatchDidFinishDocumentLoad   — DOM ready (ms_net_load fallback)
void perfNavLoadEvent();     // dispatchDidFinishLoad           — ms_net_load
void perfNavVisuallyNonEmpty(); // dispatchDidReachLayoutMilestone(DidFirstVisuallyNonEmptyLayout)
                                // = t_firstpaint (the load timeline: something readable is on screen)

} // namespace WebCorePort
