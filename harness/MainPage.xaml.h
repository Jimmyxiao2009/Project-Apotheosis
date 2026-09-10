#pragma once
#include "MainPage.g.h"
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <cstdint>

namespace Harness {

    // 书签/历史/下载条目(原生结构,非 WinRT)。
    struct Entry {
        std::wstring url;
        std::wstring title;
        std::wstring extra;   // 历史:时间;下载:文件名/状态
    };

    enum class DrawerTab { Favorites, History, Downloads };

    // Apotheosis: nested-scroll routing (commit d982774, WebCoreIsScrollableAt/WebCoreWheelAt).
    // Unknown = hit test still in flight (or none posted this gesture): existing main-frame fast
    // path. Yes/No = the async answer for the point the current gesture started at.
    enum class NestedScrollState { Unknown, Yes, No, Drag };
    // Apotheosis (axis lock / rail scrolling): Deciding = still accumulating raw translation to
    // judge the gesture's direction; X/Y = locked to that axis for the rest of the gesture
    // (incl. inertia); Free = the accumulated delta was too diagonal, no lock this gesture.
    enum class AxisLock { Deciding, X, Y, Free };
    // Apotheosis (review 2026-09-04 item 3): why a gesture is ending. Only Completed is the normal
    //   exit (ManipulationCompleted, i.e. after inertia); every other reason is an abort, and an
    //   abort also stops the zoom spring - on Completed the caller owns the pinch commit and is
    //   about to start that spring itself.
    enum class GestureEnd { Completed, Visibility, Navigate, TabSwitch, Suspend };

    // 网页链接命中矩形(位图坐标)+ URL,用于点击交互。
    struct PageLink { int x, y, w, h; std::wstring url; };

    // 标签(Mode A:单热会话+快照)。只有活动标签是引擎活会话;其余只存状态,切回时重载。
    // 活动标签的"实时状态"用 MainPage 现有全局成员表示;切换时与本结构互拷。
    struct Tab {
        std::vector<std::wstring> navStack;
        int navIndex { -1 };
        std::wstring currentUrl { L"about:home" };
        std::wstring currentTitle;
        float pageScale { 1.0f };
        // Apotheosis: 切走前抓下的最后一帧(RGBA8888,kW×kH,~3 MB)。只有**非活动**标签持有;
        // 切回来时先贴出它、再让真实重载在下面跑。snapSeq 用于超出
        // 上限时丢最老的一张。
        std::shared_ptr<std::vector<uint8_t>> snapshot;
        unsigned long long snapSeq { 0 };
        // Apotheosis (landscape/rotation, 0.1.9.41): the engine viewport the snapshot was taken
        //   at. The buffer itself is allocated at the session high-water mark, so its size no
        //   longer says how the pixels are laid out, and a snapshot taken in the other orientation
        //   would be blitted with the wrong stride. ShowTabSnapshot() drops any snapshot whose
        //   size does not match the viewport in force when it is shown.
        int snapW { 0 };
        int snapH { 0 };
    };

    public ref class MainPage sealed {
    public:
        MainPage();
        // App::OnSuspending 调用:把 cookie JSON 落盘转给引擎线程串行执行,完成后 Complete 传入的
        // deferral(见 App.xaml.cpp 注释——这是真正可靠的挂起前落盘点,取代 VisibilityChanged 那种
        // fire-and-forget)。
        void FlushCookiesForSuspend(Windows::ApplicationModel::SuspendingDeferral^ deferral);
        // Apotheosis (event-driven present): the engine asked to be presented. Always arrives on
        // the UI thread (PresentWakeThunk in MainPage.xaml.cpp marshals the driver callback, which
        // may fire on the engine thread or on a raster worker). Rate-limits and schedules one
        // composite; never touches the engine itself.
        void OnPresentWake();

    private:
        // ---- 工具栏 ----
        void OnBack(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnForward(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // 实体返回键(Win10M 硬件 Back):先关浮层,否则浏览器后退,否则交系统(最小化/退出)。
        void OnHardwareBack(Platform::Object^ sender, Windows::UI::Core::BackRequestedEventArgs^ e);
        void OnHome(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnGo(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnUrlKeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void OnMenu(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // 地址栏右侧上下文键:加载中=停止✕(作废在途+取消网络),有未提交输入=Go→,否则=刷新⟳。
        void OnUrlAction(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Reload();
        void UpdateUrlActionGlyph();   // 按 m_loading / 是否有未提交输入 切 ✕/→/⟳
        void UpdateLockIcon();         // 按 m_currentUrl 协议设安全标(https=锁/http=警告)
        // 地址栏输入变化:刷新上下文键 + 弹/收历史+书签建议下拉。
        void OnUrlChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
        void ShowSuggestions(const std::wstring& query);
        void HideSuggestions();
        // Apotheosis (suggestion tap, 2026-09-10): commit one address-bar navigation — the single
        //   path Enter, the go button and a tapped suggestion all take (normalise, navigate, move
        //   focus off the field so OnUrlLostFocus restores the chrome, put the keyboard away).
        void CommitUrlNavigation(Platform::String^ url);
        // Apotheosis (suggestion tap, 2026-09-10): SuggestPanel's own pointer handlers (added with
        //   handledEventsToo, so a Button inside it that marks the event handled is still seen) —
        //   they keep the panel alive while a finger is down on it. See m_suggestPressed.
        void OnSuggestPointerDown(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
        void OnSuggestPointerUp(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
        void OnSuggestPointerCaptureLost(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
        // Apotheosis (suggestion tap, 2026-09-10): queue the deferred collapse of the dropdown that
        //   OnUrlLostFocus and the pointer-up handler share (token + "not while pressed" guard).
        void QueueSuggestionHide();
        // Apotheosis (suggestion tap, 2026-09-10): the same treatment for the keyboard shift — the
        //   dropdown may not be translated out from under a finger either. See ApplyKeyboardShift().
        void QueueKeyboardShiftRestore(const char* why);
        void OnUrlGotFocus(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnUrlLostFocus(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // 编辑地址时白色 ✕ 顶掉刷新/停止键:清空地址栏,不夺焦(IsTabStop=False)。
        void OnUrlClear(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void SetUrlEditingChrome(bool editing);   // 刷新/停止键 <-> 白色清除键
        void HideUrlBoxDeleteButton();            // 收起 UrlBox 模板自带的 ✕(地址栏用胶囊右侧那个)

        // ---- 动作面板(菜单键弹出的 action sheet)----
        void ShowActionMenu();
        void HideActionMenu();
        void OnActionScrimTap(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void OnSheetTap(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void OnAction(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);   // 按 Button.Tag 分发
        void ToggleBookmark();
        void DoShare();
        void DoCopyLink();
        void DoToggleUA();

        // ---- 设置页 ----
        void ShowSettings();
        void HideSettings();
        void OnSettingsBack(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnSettingsBtn(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);   // tag: clearhist/clearfav/cleardl/export/gpu
        void OnZoomChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs^ e);
        // Apotheosis (review 2026-09-03): DISPLAY toggle "Hide navigation bar" =
        //   ApplicationView::SuppressSystemOverlays (phone only, ApiInformation-guarded).
        void OnHideNavBarToggled(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void ApplyHideNavBarSetting();
        // Apotheosis (review 2026-09-04 item 2b): DISPLAY toggle "Hide status bar" — full
        //   StatusBar::HideAsync()/ShowAsync(), on top of (not instead of) the always-on translucent
        //   treatment near ApplyViewInsets() in the constructor.
        void OnHideStatusBarToggled(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void ApplyHideStatusBarSetting();
        void LoadSettings();
        void SaveSettings();
        void ApplySettings();
        void ExportDebug();
        // 检测更新:后台线程拉 GitHub Releases API,比对版本;manual=true 时无更新/失败也提示。
        void CheckForUpdate(bool manual);

        // ---- OOBE / 多语言(首启选语言;英文=遍历已加载 XAML 树把中文串翻成英文)----
        void OnOobeLang(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void ApplyLanguage();
        void TranslateNode(Platform::Object^ node, bool toEn);
        // Apotheosis: 运行期换语言(设置页 LANGUAGE / OOBE)。就地翻译已加载树 + 重刷运行期标签。
        void SetLanguage(const std::wstring& lang);

        // ---- 页内查找 ----
        void ShowFindBar();
        void OnFindChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
        void OnFindKeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void OnFindNext(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnFindPrev(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnFindClose(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void DoFind(int mode);   // 0=查找(标记全部) 1=下一个 2=上一个

        // ---- 标签(Mode A)----
        void OnTabs(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnNewTab(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnTabSwitcherDone(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void ShowTabSwitcher();
        void HideTabSwitcher();
        void RebuildTabSwitcher();
        void SaveActiveTab();       // 当前全局状态 → m_tabs[m_activeTab]
        void RestoreTab(int i);     // m_tabs[i] → 全局状态 + 重载该标签 URL(重建会话)
        void NewTab();
        void CloseTab(int i);
        void SwitchTab(int i);
        void UpdateTabCount();
        // Apotheosis: 标签切换快照。
        void CaptureActiveTabSnapshot();  // 把当前会话最后一帧读回,存进**离开**的那个标签(引擎线程,异步)
        void ShowTabSnapshot(int i);      // 切到 i:立刻贴出它的快照(有的话)并释放之
        void HideTabSnapshot();           // 新会话第一帧到位/加载超时:恢复正常显示面
        void PruneTabSnapshots();         // 只保留最近 kMaxTabSnapshots 张,其余释放
        // UA 切换:手机/桌面,切后重载当前页(遇到对移动 UA 抽风的站点用)。
        void OnToggleUA(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // GPU 合成开关(M2):一次性开启(引擎线程 WebCoreGpuInit 离屏成功→重载当前页走 TextureMapper 合成)。
        void OnToggleGpu(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void EnableGpu();   // 开 GPU 直呈现(OnToggleGpu 首点 + 默认GPU自动触发 共用;含崩溃环路保护)
        // Apotheosis (M4): GPU 优先启动 —— 面板就绪 → EnableGpu() → 回调里才发第一次导航,
        //   使首个会话就带合成(引擎侧合成只在 buildSession 按 g_gpuActive 打开),省掉启动时的重复加载。
        void StartupGpuThenNav();      // 触发源(页面/面板 Loaded、面板 SizeChanged)共用,自带去重
        void StartPendingFirstNav();   // 发出并清空 m_pendingFirstNav(GPU 成功/失败/兜底都走这里)
        void CancelPendingFirstNav();  // 丢弃待发导航 + 停兜底定时器(开/切/关标签时必调)
        void OnStartupNavTimer(Platform::Object^ sender, Platform::Object^ e);   // 兜底定时器:触发源都没来也要导航
        void OnPageLoadedForGpu(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);  // 页面 Loaded:保底触发源
        void ArmStartupNavTimer();      // (重新)武装 6s 兜底定时器:待发导航必须出去
        std::string GpuPanelSizeStr();  // 面板当前尺寸 "WxH"(启动诊断行用)
        // Apotheosis (2026-09-03 崩溃修复): GPU-first 起 GPU 前必须等 GpuPanel 有真实(非零)尺寸——
        //   Loaded 事件可能在面板还没走完第一次 arrange 时就到达(真机 mem.txt 记过 "panel 0x0"),
        //   这时就绑 ANGLE 窗口表面,面板随后第一次真实 SizeChanged 会在 libGLESv2.dll 里空指针崩溃。
        //   两处触发点(构造期 GPU-first 启动、NavigateTo 里的首次导航拦截)都改走这个入口。
        void HookGpuPanelForStartup();  // 面板已有尺寸→立即起 GPU;否则挂 SizeChanged + 武装 2s 等待兜底
        void OnGpuSizeWaitTimer(Platform::Object^ sender, Platform::Object^ e);  // 2s 内没等到真实尺寸→按老行为起 GPU,别把启动卡死

        // ---- 抽屉 ----
        void OnDrawerClose(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnTabFav(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnTabHist(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnTabDl(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnPrimaryAction(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);

        // ---- 导航 ----
        void NavigateTo(Platform::String^ url, bool pushHistory);
        void UpdateNavButtons();
        void SetLoading(bool loading);
        void OnNavDone(Platform::String^ finalTitle, bool ok, bool loadOk);
        void OnLoadWatchdog(Platform::Object^ sender, Platform::Object^ e);
        // 网页点击:有会话则把点击转发到引擎(按钮/表单/链接统一走真实事件);无会话(主页)走链接表。
        void OnPageTapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        // Apotheosis (map-site pin, 2026-09-06): press and hold on a map/canvas. XAML raises
        // Holding for touch while the finger is still on the glass; the engine then gets a real
        // long press (WebCoreLongPressAt) instead of the click a tap delivers.
        void OnPageHolding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);

        // ---- Apotheosis (link context menu, 0.1.9.42): long press on a link ----
        // The long-press route asks the engine (WebCoreLinkAt) what is under the finger before it
        // presses anything. A link opens this menu and the page is told NOTHING - neither the long
        // press nor the click on release - so a hold over a link can no longer navigate by accident.
        // Any other point falls through to the press-and-hold the page has always been given.
        // ShowLinkMenu places the card at m_ctxDip{X,Y} (RootGrid DIP, taken when the hold started).
        void ShowLinkMenu(const std::wstring& url);
        void HideLinkMenu(const char* why);          // why -> stage.txt "ctx dismiss why="
        void CancelPendingLinkMenu(const char* why); // hold turned into a pan: drop the answer in flight
        // Apotheosis (0.1.9.46): the card is placed in DIP against the window it was opened in, so
        //   a rotation (or the software navigation bar moving an edge) can leave it half off the
        //   screen. Record that window when the hold starts, and dismiss the card when it changes -
        //   the title row coming and going deliberately does not count as a change.
        void NoteLinkMenuLayout();
        void DismissLinkMenuIfLayoutMoved();   // -> stage.txt "ctx dismiss why=rotate"
        // Park a URL as a new tab WITHOUT switching to it. On this port's Mode A tab model that is a
        // queued tab, not a background load - see the comment on the definition.
        void OpenUrlInBackgroundTab(const std::wstring& url);
        void OnLinkMenuScrimTap(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void OnLinkMenuCardTap(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void OnLinkMenuOpenNewTab(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // 把内容区显示坐标(DIP)映回引擎像素空间(直呈现下表面被拉伸+设备分辨率缩放),修点击/焦点偏移。
        void MapTapToEngine(double dipX, double dipY, int& outPx, int& outPy);
        // 把位图像素 (px,py) 的点击转发到引擎活会话(WebCoreClickAt),完成后同步地址栏/历史/链接表。
        // longPress = hold the button down first (WebCoreLongPressAt) instead of clicking.
        // Apotheosis (double-tap zoom, 2026-09-09): clickCount is forwarded to WebCoreClickAtCount
        //   (2 = a real DOM 'dblclick', see DtapCompleteSecond); default 1
        //   keeps every existing call site (ordinary tap, long press) unchanged.
        void ForwardClickToEngine(int px, int py, bool longPress = false, int clickCount = 1);
        // 引擎滚动 dy 像素(触发懒加载图片)后重绘。dy>0 向下。
        void EngineScroll(int dy);
        // Apotheosis: show/hide the floating page up/down buttons (developer setting, off by default).
        void UpdateScrollFab();
        // Apotheosis: push the PRIVACY prefetch choice (m_prefetch + current connection cost) to the
        //   engine. UI thread only; posts to the engine thread, never waits on it.
        void ApplyPrefetchSetting();
        void OnScrollUp(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnScrollDown(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // 自由滚动:内容区 ManipulationDelta(单指拖拽 ΔY)→ 累积位移 → 合并成引擎滚动(无 spinner,带惯性)。
        void FreeScrollBy(int dx, int dy);   // 累积 dx/dy 并在引擎空闲时冲刷
        void PumpScroll();           // 把累积位移作为一次 WebCoreScrollBy 派发(完成后若仍有累积再派发)
        void SyncLinksAfterScroll(); // 滚动停止后一次性刷新链接命中表(滚动期跳过了引擎 extractLinks)
        // 手势开始:异步问引擎这一点下面有没有可滚动祖先/iframe(WebCoreIsScrollableAt),决定本次手势
        // (含惯性)走 NestedScrollBy 还是现有主帧快路径。ManipulationStarted 在 XAML 里没有挂钩(见
        // MainPage.xaml,只接了 Delta/Completed)——构造函数里手动订阅(ContentArea->ManipulationStarted +=)。
        void OnImageManipStarted(Platform::Object^ sender, Windows::UI::Xaml::Input::ManipulationStartedRoutedEventArgs^ e);
        void OnImageManipDelta(Platform::Object^ sender, Windows::UI::Xaml::Input::ManipulationDeltaRoutedEventArgs^ e);
        // M4 捏合缩放:捏合期间对显示层做实时 ScaleTransform(零引擎),松手提交给引擎按新尺度重栅格(文字清晰)。
        void OnImageManipCompleted(Platform::Object^ sender, Windows::UI::Xaml::Input::ManipulationCompletedRoutedEventArgs^ e);
        // 嵌套滚动(cookie 浮层/模态框/iframe):累积一次手势内的位移,合并成一次引擎线程调用——
        // WebCoreWheelAt 先派发真实 wheel,消费(1)则到此为止;未消费(0)则在同一次 post 里补
        // WebCoreScrollBy 走主帧(顺序保持,和 FreeScrollBy/PumpScroll 的快路径是姊妹实现)。
        void NestedScrollBy(int px, int py, int dx, int dy);
        void PumpNestedScroll();
        // 手势前几个 delta 抢在 WebCoreIsScrollableAt 答案之前到达时先缓存(见 m_pendingPan*),
        // 答案落地(或手势结束)后按选定路径一次性补发。
        void ReplayPendingPan();
        // Apotheosis (axis lock / rail scrolling, developer setting "Axis lock"): fold in one more
        // raw (DIP, pre-scale) translation delta and, once the accumulated distance since gesture
        // start passes kAxisLockThresholdDip, decide X/Y/Free from the minor/major component ratio
        // (kAxisLockRatio). A no-op once a decision is already made. Never called while pinching.
        void UpdateAxisLock(double rawDx, double rawDy);
        // Zero the minor-axis component of an engine-px delta pair if the gesture is locked and the
        // setting is on. Applied only to page-panning routes (main fast path, nested WebCoreWheelAt)
        // — never to the drag-as-pointer-events route (map/canvas) or to pinch.
        void ApplyAxisLock(int& dx, int& dy);
        // Apotheosis (drag as pointer events): a gesture that started over something which drags
        // itself (map, canvas — WebCoreWantsDragAt said so) is fed to the page as a real mouse
        // drag instead of being turned into scrolling. Same one-in-flight shape as
        // NestedScrollBy/PumpNestedScroll, but the state machine is ordered rather than
        // accumulating: the press must land (and be consumed) before any move is sent, because
        // its return value is what decides whether the gesture belongs to the page at all.
        void DragMoveTo(int px, int py, int fallbackDx, int fallbackDy);   // finger is here now
        void PumpDrag();          // send the next queued phase, one engine post at a time
        // Apotheosis (pinch on map widgets, 2026-09-06): a pinch that started over a map is fed to
        // the page as ctrl+wheel notches instead of scaling the page. Same one-post-in-flight shape
        // as PumpDrag; notches accumulate while a post is out.
        void ZoomWheelBy(int px, int py, int notches);
        void PumpZoomWheel();
        void DragReset();         // forget the gesture (new gesture, session gone, superseded)
        // Apotheosis: an engine scroll frame landed (PumpScroll's completion) — refresh the cached
        //   scroll position/bounds every other consumer reads (MapTapToEngine, ScrollStateUsable,
        //   the live-pinch clamp) and feed the touch-lag diagnostic. haveScrollState=false drops the
        //   cache instead of guessing. fallbackDx/Dy is the coalesced delta that round trip asked
        //   for (PumpScroll's dx/dy); only the diagnostic uses it.
        void EngineScrollFrameApplied(int newScrollX, int newScrollY, bool haveScrollState,
                                      int fallbackDx, int fallbackDy);
        // Apotheosis (touch-lag diagnostic): called where EngineScrollFrameApplied() has just
        //   refreshed m_scrollX/m_scrollY from a real engine present, folding the newly-applied delta
        //   into the running gesture stats opened by OnImageManipStarted/closed by
        //   OnImageManipCompleted.
        //   Apotheosis (2026-09-07, clamp fix): appliedDx/Dy is what m_scrollX/Y actually moved THIS
        //   round trip (new minus old, in the caller's own hands right before it overwrites them);
        //   requestedDx/Dy is the coalesced delta that round trip's WebCoreScrollBy was asked
        //   for (PumpScroll's dx/dy, threaded through as EngineScrollFrameApplied's own
        //   fallbackDx/fallbackDy parameter). When the engine could not honour the
        //   request in full — clamped at a document edge, the one case its position answer can
        //   legitimately disagree with what was sent — the shortfall is not a real lag and is folded
        //   out of the base instead of being left to inflate max/avg forever.
        void NoteScrollLagPresented(int appliedDx, int appliedDy, int requestedDx, int requestedDy);
        void RequestScrollState();                    // seed the cached scroll/bounds (async)
        // Apotheosis: compose the pinch preview scale and its clamp translation onto the
        //   presenting element (TransformGroup, scale first so the translation stays screen-space).
        void ApplyPresentTransform();
        // Apotheosis (review 2026-09-04 item 3): the ONE exit from a gesture - see the comment on the
        //   definition. Resets pinch, pan, nested-scroll and drag state together, whatever ended it.
        void EndGesture(GestureEnd reason);
        // Apotheosis (review 2026-09-04 item 4): complete the suspend deferral, exactly once, on the
        //   UI thread. Called by the engine flush's UI hop and by the 2 s guard timer, whichever
        //   gets there first.
        void CompleteSuspendDeferral();
        // Apotheosis (review 2026-09-03): status-bar / software-nav-bar insets from
        //   ApplicationView::VisibleBounds vs CoreWindow::Bounds → RootGrid bottom padding + top
        //   margin of the top-anchored chrome. UI thread only.
        void ApplyViewInsets();
        // Apotheosis (review 2026-09-03): the URL suggestion dropdown sits in the content row, so
        //   it needs the same soft-keyboard shift as NavBarShift. 0 = back to rest.
        //   (2837ce0 review item 1): TitleRow lives in the same subtree and rides along.
        void ShiftSuggestPanel(double y);
        // Apotheosis (bug fix 2026-09-06 evening): build the dedicated keyboard TranslateTransforms
        //   for the two content-row overlays once, at construction — TitleRow's existing transform is
        //   animated by RevealTitleRow()/CollapseTitleRow() and must not be written to. UI thread only.
        void SetupKeyboardShiftTransforms();
        // Apotheosis (bug fix 2026-09-06 evening): THE place that positions the bottom chrome against
        //   the on-screen keyboard. Recomputes from (recorded keyboard geometry, current bottom inset,
        //   address-bar editing state) and applies the difference; idempotent, so every input that can
        //   change one of the three just calls it. `why` only lands in the stage.txt line. UI thread only.
        void ApplyKeyboardShift(const char* why);
        // Apotheosis (0.1.9.43): re-read the InputPane rectangle (it changes on a rotation without a
        //   new Showing event), then ApplyKeyboardShift. The rectangle is sanity-checked against the
        //   window it is supposed to belong to (0.1.9.44) - a rotation is exactly the moment the
        //   shell still answers with the previous orientation's. UI thread only.
        void RefreshKeyboardMetrics(const char* why);
        // Apotheosis (0.1.9.44): ask again on a low-priority dispatcher hop after the InputPane
        //   answered with a rectangle that cannot belong to the current window. Bounded.
        void QueueKeyboardMetricsRecheck(const char* why);
        // Apotheosis (2837ce0 review item 1): the title/toast row auto-hides. Reveal() shows it and
        //   — unless a page is loading — arms the ~2 s hide; Collapse() fades it out and hands its
        //   strip back to the content area (ApplyViewInsets' titleH). Every write to TitleText::Text
        //   reveals the row through a property-changed callback registered in the constructor, so
        //   the ~25 places that use TitleText as a toast keep working untouched. UI thread only.
        void RevealTitleRow();
        void CollapseTitleRow();
        void OnTitleRowHideTick(Platform::Object^ sender, Platform::Object^ e);
        // Apotheosis (2837ce0 review item 2): is the cached scroll/bounds state still about the
        //   page we are looking at? The cache is engine px at ONE page scale — content size in
        //   engine px scales with m_pageScale — so a commit-time scale change makes every field
        //   stale. Drops the cache (and logs once) instead of clamping against pre-zoom bounds.
        bool ScrollStateUsable();
        void ApplyLiveZoom();
        void PinchCommit(float newScale, int focalX, int focalY);
        // Apotheosis: the layer that shows the engine output (GpuPanel in direct-present mode,
        //   RenderImage otherwise) — the pinch preview transform hangs off it.
        Windows::UI::Xaml::FrameworkElement^ PresentLayer();
        // Apotheosis: fix the pinch anchor (once per gesture) from a ContentArea DIP position;
        //   fills m_focalX/Y (transform centre) and m_focalPx/Py (engine pixels for the commit).
        void SetPinchAnchor(double dipX, double dipY);
        // Apotheosis (0.1.9.40): same, from an anchor already in ENGINE VIEWPORT PX (WebCoreTapPolicyAt's
        //   outAnchorX/Y) — see the definition's comment. Used by RunDoubleTapZoom only.
        void SetPinchAnchorEnginePx(int anchorPx, int anchorPy);
        // Apotheosis: ease the preview from where the fingers left it to the scale we commit
        //   (overview below 1:1, ±6 % snap), then call PinchCommit. Composition-thread animation.
        void SpringBackZoom(float targetLive, float commitScale);
        // Apotheosis (double-tap zoom, 2026-09-09): commit a double-tap zoom through the same
        //   anchor/animate/commit path a pinch release uses — SetPinchAnchorEnginePx(anchorPx,anchorPy)
        //   then SpringBackZoom to targetScale (clamped/snapped), which calls PinchCommit when done.
        //   Apotheosis (0.1.9.40): anchorPx/anchorPy are engine viewport px (WebCoreTapPolicyAt's
        //   outAnchorX/Y — the tap point, or a column's centre x), not a ContentArea DIP position.
        void RunDoubleTapZoom(int anchorPx, int anchorPy, float targetScale);
        // Apotheosis (double-tap zoom, 2026-09-09): m_dtapHoldTimer's one-shot Tick — no second tap
        //   arrived within the hold interval, so the tap OnPageTapped held is just an ordinary click.
        void OnDtapHoldTimer(Platform::Object^ sender, Platform::Object^ e);
        // Apotheosis (double-tap route, 2026-09-10): the second tap of a pair has been recognised and
        //   the engine's policy answer for the first one is in — zoom, or forward one clickCount=2
        //   click. Clears the whole tap-hold state and bumps m_dtapGen, so a late answer cannot act.
        void DtapCompleteSecond();
        // Apotheosis (double-tap route, 2026-09-10): forget the held tap (timer, flags, generation).
        void DtapReset();
        // Apotheosis (double-tap route, 2026-09-10): ONE stage.txt line per tap decision, including
        //   every early return, so a device log shows what the route did even when it did nothing.
        void DtapTrace(const char* what, const std::string& detail);
        // 实时渲染循环:低帧率驱动引擎 WebCoreLiveTick,让 CSS/JS 动画动起来、SPA 多帧渐进挂载。
        // 画面连续静止则自动停帧省电,交互/滚动/导航再启动。
        void StartLiveMode();
        void StopLiveMode();
        // Apotheosis (event-driven present): the tick body — one
        //   WebCoreLiveTick on the engine thread plus the UI-thread continuation that presents it.
        //   Sets m_liveBusy; UI thread only.
        void DispatchLiveFrame();
        // Dispatch one composite if the rate limit allows, otherwise arm m_wakeTimer for the
        //   remainder. Called by OnPresentWake, the wake timer and the frame continuation.
        void ScheduleWakeComposite();
        void OnWakeTimer(Platform::Object^ sender, Platform::Object^ e);
        // 1 s safety net: composites what no wake signalled, self-heals a lost m_liveBusy and
        //   keeps the memory sampling of the old tick going. Slows to 5 s while nothing changes.
        void OnFallbackTick(Platform::Object^ sender, Platform::Object^ e);
        // Register the driver wake callback (engine-thread post) and (re)arm the live loop.
        void ApplyEventPresentSetting();
        // 输入法:点中可编辑元素后唤起屏幕键盘;键入转发给引擎活会话。
        void OnImeTextChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
        void OnImeKeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void OpenKeyboard();
        void CloseKeyboard();
        // Apotheosis: action menu / settings / tab switcher all cover the address bar — hide the
        //   OS keyboard and actually move focus off UrlBox (TryHide() alone can let it reappear on
        //   the next tap while UrlBox still has focus).
        void DismissKeyboardForOverlay();
        // kind: 0=插入文本(text),1=回车,2=退格。转发到引擎并重绘。
        void SendKeyToEngine(int kind, Platform::String^ text);
        // GPU 路径1 探针:SwapChainPanel 就绪后启动 ANGLE 三角形探针(验 GPU 管线在 App Container 通)。
        //   GPU 优先启动时改为在此起引擎 GPU(探针会占住同一面板的窗口表面,故那条路径下不跑探针)。
        void OnGpuPanelLoaded(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // 面板拿到非零尺寸(折叠元素尺寸恒 0)→ 可以建 ANGLE 窗口表面 → StartupGpuThenNav()。
        void OnGpuPanelSizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ e);
        // Apotheosis (landscape/rotation, 0.1.9.41): the engine viewport follows the presenting
        //   panel instead of being the fixed 720x1080 it was until now - see the block above
        //   ComputeEngineViewport in MainPage.xaml.cpp for the root cause and the ANGLE side.
        bool ComputeEngineViewport(bool useGpuPanel, int& outW, int& outH);
        int BottomOcclusionEnginePx();
        // Apotheosis (0.1.9.46): send the current bottom occlusion to the engine and have it
        //   re-reveal the focused field with it, outside a resize - a rotation only learns the
        //   keyboard's new rectangle a few dispatcher hops after the resize has already run.
        //   Silent when the number has not changed. why -> stage.txt "reveal why=".
        void PushBottomOcclusion(const char* why);
        // forceW/forceH > 0 bypass ComputeEngineViewport and install exactly that viewport - used
        //   by the surface-mismatch safety net, which has to adopt the EGL surface VERBATIM rather
        //   than a recomputed panel x scale that could round to a different number again.
        void UpdateEngineViewport(const char* why, bool force, int forceW = 0, int forceH = 0);
        void WirePresentPanelSizeChanged();
        void OnPresentPanelSizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ e);
        // Apotheosis (landscape, 0.1.9.43): re-render the session-less page on screen (start page /
        //   error page) at the current viewport - see RenderStaticPage in MainPage.xaml.cpp.
        void RenderStaticPage(const char* why);
        // 把一帧引擎渲染结果(rgba)贴到位图 + 同步标题/地址/链接表;navUrl 非空表示会话内发生了导航。
        void ApplyEngineFrame(const std::shared_ptr<std::vector<uint8_t>>& rgba,
                              Platform::String^ title, Platform::String^ navUrl,
                              const std::shared_ptr<std::vector<PageLink>>& links);
        // 软件模式:把引擎 RGBA 帧贴上 RenderImage(WriteableBitmap 双缓冲复用);直呈现模式内部自跳过。
        void PresentSoftwareFrame(const std::shared_ptr<std::vector<uint8_t>>& rgba);

        // ---- 抽屉 UI ----
        void ShowDrawer(DrawerTab tab);
        void HideDrawer();
        void RebuildDrawerList();
        void StartDownload(Platform::String^ url);

        // ---- 数据 + 持久化 ----
        void LoadData();
        void SaveBookmarks();
        void SaveHistory();
        void SaveDownloads();
        void AddHistory(const std::wstring& url, const std::wstring& title);
        bool IsBookmarked(const std::wstring& url);

        // 浏览器自管历史(后退/前进的 URL 栈,区别于"历史记录"列表)
        std::vector<std::wstring> m_navStack;
        int m_navIndex { -1 };
        bool m_loading { false };

        std::vector<Entry> m_bookmarks;   // 收藏
        std::vector<Entry> m_historyList; // 历史记录(最新在前)
        std::vector<Entry> m_downloads;   // 下载
        DrawerTab m_tab { DrawerTab::Favorites };

        std::wstring m_currentUrl;
        std::wstring m_currentTitle;
        std::vector<PageLink> m_pageLinks;   // 当前页链接命中表(点击交互)
        // Apotheosis (landscape, 0.1.9.43): the HTML of the static page on screen (start page or
        //   error page), so a viewport change can re-render it. Empty whenever a live session owns
        //   the picture - then WebCoreResize does the relayout.
        std::string m_staticHtml;
        bool m_sessionActive { false };      // 当前是否有引擎常驻会话(网络页=有,主页/错误页=无)
        bool m_interacting { false };        // 正在转发点击/滚动到引擎(防重入,UI 侧)
        // 操作序号:每次导航/点击/滚动 ++。回调在 UI 线程检查捕获的序号是否仍等于最新,过期(被看门狗
        // 强制复位后又起了新操作,或被新操作取代)则丢弃,避免迟到回调冲掉新操作状态 → 永久冻结。
        unsigned long long m_opSeq { 0 };
        // 输入法状态
        std::wstring m_lastImeText;   // ImeBox 上次文本(算增量转发)
        bool m_imeOpen { false };     // 屏幕键盘是否为当前输入打开
        bool m_imeSyncing { false };  // 正在程序化改 ImeBox.Text(避免 TextChanged 回环)
        bool m_uaMobile { true };     // UA 模式:true=手机(默认),false=桌面
        bool m_urlSyncing { false };  // 正在程序化改 UrlBox.Text(导航/回调同步地址栏)→ 抑制建议下拉回环
        bool m_urlFocused { false };  // 地址栏是否聚焦(编辑中)→ 仅聚焦时才弹建议,杜绝"莫名其妙弹出"
        // Apotheosis (2026-09-07): select-all-on-focus bookkeeping — see OnUrlGotFocus(). The token
        // lets a LostFocus/GotFocus pair that runs before the deferred SelectAll fires cancel the
        // stale one instead of selecting text in a box the user has already left or re-entered.
        unsigned long long m_urlSelectToken { 0 };
        bool m_urlDeleteBtnHidden { false };  // UrlBox 模板自带的 ✕ 已收起(只做一次)
        bool m_updateChecking { false };  // 检测更新进行中(防并发重复点)
        bool m_updateAutoChecked { false };  // 启动后已静默自检过一次(首个网络页加载完触发,CA 此时已就绪)
        // 设置(持久化到 LocalState\settings.ini;搜索前缀/主页是全局,见 .cpp)
        bool m_langSet { false };     // settings.ini 里是否已存过 lang(否=首次启动→弹 OOBE)
        int  m_setSearch { 4 };       // 搜索引擎索引(0 Bing/1 Google/2 DuckDuckGo/3 百度/4 Qwant);新装默认 Qwant
        bool m_setUaDesktop { false };// 启动默认请求桌面版网站
        int  m_defaultZoom { 100 };   // 默认缩放百分比(50–200)
        std::wstring m_uaCustom;      // 自定义 UA(空=用 mobile/desktop 开关);settings.ini ua_custom
        bool m_updateAuto { false };  // 隐私：启动后自动查 GitHub 更新（默认关）；settings.ini updatecheck
        int  m_prefetch { 0 };        // 隐私：推测预取 0=关/1=仅 Wi-Fi(不计费连接)/2=始终；settings.ini prefetch
        bool m_showScrollFab { false };// 开发者选项:悬浮翻页按钮(默认关);settings.ini scrollfab
        // Apotheosis (review 2026-09-04 item 4): the suspend deferral in flight, and the timer that
        //   bounds how long the engine may keep the shell waiting for it. Both UI thread only.
        Windows::ApplicationModel::SuspendingDeferral^ m_suspendDeferral { nullptr };
        Windows::UI::Xaml::DispatcherTimer^ m_suspendTimer { nullptr };
        // Apotheosis (axis lock / rail scrolling): SCROLLING toggle, default ON. Pure harness-side
        // logic (see UpdateAxisLock/ApplyAxisLock) — no engine call, so it needs no ApplyXSetting
        // push, m_axisLockEnabled is read directly.
        // settings.ini axislock
        bool m_axisLockEnabled { true };
        // Apotheosis (review 2026-09-03): DISPLAY toggle. Off = the phone keeps its software
        //   back/Windows/search bar; on = SuppressSystemOverlays hands that strip to us and the
        //   bottom inset in ApplyViewInsets() goes to 0. settings.ini hidenavbar
        bool m_hideNavBar { false };
        // Apotheosis (review 2026-09-04 item 2b): DISPLAY toggle. Off = status bar shown (translucent,
        //   clock stays readable via the ForegroundColor set in the constructor); on = fully hidden via
        //   StatusBar::HideAsync(), and the top inset in ApplyViewInsets() follows VisibleBounds to 0.
        //   settings.ini hidestatusbar. Default ON: this is a member initializer, so it only takes
        //   effect for a fresh install (no "hidestatusbar" line in settings.ini yet). Anyone with a
        //   stored value (LoadSettings' "hidestatusbar" branch) keeps whatever they already have on
        //   disk, this default never overrides it.
        bool m_hideStatusBar { true };
        // Apotheosis (review 2026-09-04 item 4): token for the deferred suggestion-dropdown collapse
        //   scheduled from OnUrlLostFocus — see its definition for why the collapse cannot be
        //   synchronous. Bumped on every LostFocus/GotFocus so a stale deferred hide is a no-op.
        unsigned long long m_suggestHideToken { 0 };
        // Apotheosis (suggestion tap, 2026-09-10): a finger is down inside SuggestPanel. A UWP Button
        //   takes focus in its OnPointerPressed, so tapping a suggestion runs OnUrlLostFocus while the
        //   finger is STILL DOWN; the deferred collapse it queues then runs during that idle moment
        //   (Low priority = as soon as the thread has nothing else to do) and collapses the panel out
        //   from under the finger, so the button never gets to raise Click on release and the tap did
        //   nothing but close the list. While this flag is set the deferred collapse stands down; the
        //   pointer-up handler clears it and re-arms the collapse if the field is still unfocused.
        bool m_suggestPressed { false };
        // 标签集合(Mode A:仅活动标签有引擎会话)。
        std::vector<Tab> m_tabs;
        int m_activeTab { 0 };
        bool m_gpuOn { false };       // GPU 合成是否已开(一次性;引擎侧 g_gpuActive 无 teardown,重启回软件)
        bool m_gpuPresent { false };  // GPU 直呈现模式(合成直接画到 GpuPanel,省 readback+blit)
        bool m_gpuDefault { true };   // 默认启用 GPU(设置可关;启动后首个网络页加载完自动开)
        bool m_gpuAutoTried { false };// 本次会话已自动尝试过开 GPU(不重复)
        // Apotheosis (M4): 首次网络导航被推迟到 GpuInit 之后时,URL 暂存在这里(空=没有待发导航)。
        std::wstring m_pendingFirstNav;
        bool m_pendingFirstNavPush { true };   // 那次导航的 pushHistory(后退/前进触发时必须是 false)
        // 兜底定时器:6 s 内没有任何触发源,到点也把待发导航发出去(软件首屏)。
        Windows::UI::Xaml::DispatcherTimer^ m_startupNavTimer;
        bool m_gpuStartupBegun { false };   // 已开始 GPU 优先启动(多触发源去重 + 兜底定时器不抢跑)
        bool m_pageLoadedSeen { false };    // 诊断:页面 Loaded 到过
        bool m_gpuPanelLoadedSeen { false };// 诊断:GpuPanel Loaded 到过(第一版真机上它没来)
        // Apotheosis (2026-09-03 崩溃修复): 见 HookGpuPanelForStartup。2s 等待真实面板尺寸的兜底定时器,
        //   与上面 6s 的 m_startupNavTimer 是两层不同的保险(这层等尺寸,那层等"有没有任何触发源")。
        Windows::UI::Xaml::DispatcherTimer^ m_gpuSizeWaitTimer;
        bool m_gpuSizeHandlerWired { false };
        // Apotheosis (landscape/rotation, 0.1.9.41): engine px per panel DIP. Pinned once, in
        //   EnableGpu() (or on the first software measurement), so that the short side of the
        //   viewport is kEngineShortSidePx; ANGLE was handed the same number as the surface
        //   resolution scale, so it must not move afterwards. 0 = not pinned yet.
        double m_engineScale { 0.0 };
        // true once EnableGpu() has bound ANGLE to GpuPanel with a resolution scale, i.e. the
        //   surface really does follow the panel. false = the fixed-surface fallback, where the
        //   engine viewport must stay put whatever the panel does.
        bool m_engineFollowsGpuPanel { false };
        // EnableGpu() defers itself once when GpuPanel has not been arranged yet, so the ANGLE
        //   resolution scale can be measured off a real panel rectangle. One retry, then the
        //   fixed-surface fallback.
        bool m_gpuEnableRetried { false };
        // How many times the requested viewport has been corrected to the surface ANGLE reported.
        //   Bounded so a surface that never matches cannot turn into a resize loop.
        int m_engineCalibrations { 0 };
        bool m_presentSizeHandlerWired { false };   // GpuPanel->SizeChanged (rotation), subscribed once
        bool m_contentSizeHandlerWired { false };   // ContentArea->SizeChanged (software path), once// GpuPanel->SizeChanged 是否已经挂过(避免 HookGpuPanelForStartup 重入重复订阅)
        Windows::Foundation::Collections::PropertySet^ m_gpuProps;  // ANGLE 原生窗口(SwapChainPanel 包装),保活
        int  m_gpuOrient { 0 };       // 离屏 readback 朝向(bit0=H,bit1=V):0=none(真机实测正确),1=H,2=V,3=HV
        // 自由滚动状态
        int  m_scrollAccum { 0 };     // 未冲刷的累积竖向滚动位移(像素,>0 向下)
        int  m_scrollAccumX { 0 };    // 未冲刷的累积横向滚动位移(像素,>0 向右)
        bool m_scrollBusy { false };  // 有 WebCoreScrollBy 任务在引擎线程飞行
        // Apotheosis: nested-scroll routing state (WebCoreIsScrollableAt/WebCoreWheelAt, d982774).
        NestedScrollState m_nestedScrollState { NestedScrollState::Unknown };  // this gesture's answer
        // Apotheosis (review 2026-09-04 item 3): a finger is on the glass (or its inertia is still
        // running) — set at ManipulationStarted, cleared at ManipulationCompleted.
        bool m_manipActive { false };
        unsigned long long m_nestedScrollGen { 0 };   // bumped at ManipulationStarted; a late hit-test
                                                       // answer whose gen no longer matches is dropped
        int  m_nestedAccumX { 0 };    // 未冲刷的累积横向位移(嵌套滚动路径,同 m_scrollAccumX 但走 wheel)
        int  m_nestedAccumY { 0 };    // 未冲刷的累积竖向位移
        int  m_nestedPx { 0 }, m_nestedPy { 0 };   // wheel 派发点(引擎像素),跟随手指当前位置
        bool m_nestedScrollBusy { false };   // 有 NestedScrollBy 任务在引擎线程飞行
        // Apotheosis (review 2026-09-03): deltas that arrived while m_nestedScrollState was still
        // Unknown. They used to go straight down the main-frame fast path, so a pan started on a
        // cookie overlay jerked the page behind it once before the hit-test answer switched routes.
        // Buffered here instead and replayed through whichever path the answer picks
        // (ReplayPendingPan). Reset per gesture in OnImageManipStarted.
        int  m_pendingPanX { 0 }, m_pendingPanY { 0 };     // accumulated buffered offset delta
        int  m_pendingPanPx { 0 }, m_pendingPanPy { 0 };   // finger position (engine px) of the last of them
        // Apotheosis (axis lock / rail scrolling): per-gesture state. Reset in OnImageManipStarted;
        // dropped to Free the moment a gesture turns out to be a pinch (SetPinchAnchor). The
        // accumulators are raw DIP translation (Manipulation coordinate space), independent of the
        // engine-px dx/dy the accumulated decision is later applied to.
        AxisLock m_axisLockState { AxisLock::Deciding };
        double m_axisAccumX { 0.0 }, m_axisAccumY { 0.0 };
        // Apotheosis (touch-lag diagnostic, 2026-09-07): "does the finger and the page agree" —
        //   per-gesture, main-frame free-scroll path only (the fast path OnImageManipDelta's final
        //   branch takes; nested-scroll/drag/pinch gestures are a different question and are not
        //   tracked here). Zero cost when g_perfLogEnabled is off — every touch point below this
        //   comment is skipped entirely, same convention as imedebug.txt/perf.txt.
        //   m_scrollLagActive: a free-scroll gesture is open and STILL COMPARING finger to engine
        //     (set in OnImageManipStarted while perf logging is on; also cleared the moment inertia
        //     begins — see m_scrollLagStarted below for why that is a separate flag).
        //   m_scrollLagStarted: the gesture-scoped "was this ever tracked at all" flag OnImageManip
        //     Completed gates the stage.txt line on. m_scrollLagActive freezes early (first inertial
        //     delta), but the write must still happen — decoupled so the line for an ordinary swipe
        //     that ends in inertia (nearly all of them) is not silently dropped.
        //   m_scrollLagBaseX/Y: m_scrollX/m_scrollY (engine px) at the moment the base was taken —
        //     "applied so far" is m_scrollX/Y minus this, so it is a plain delta and needs no
        //     separate accumulator.
        //   m_scrollLagBaseValid/m_scrollLagScale (2026-09-07, device bug — "1781 px lag at
        //     ps=5.699"): engine px is CSS px * page scale (GraphicsLayerTextureMapper.cpp's
        //     WK_WINUWP page-scale transform; the same convention ScrollStateUsable()
        //     enforces elsewhere via m_scrollStateScale), so a base and a sample taken at DIFFERENT
        //     page scales are not comparable — their difference is dominated by the scale jump, not
        //     by anything the finger did. OnImageManipStarted used to grab m_scrollX/Y unconditionally
        //     as the base; right after a pinch commit (PinchCommit invalidates m_scrollStateValid but
        //     leaves the m_scrollX/Y NUMBERS at their pre-pinch scale until the first post-pinch
        //     engine round trip) that base was stale by a factor of roughly the pinch's own scale
        //     ratio — exactly the observed magnitude. m_scrollLagScale is the page scale this
        //     gesture is being measured at (stamped once, at gesture start — pan and pinch are
        //     mutually exclusive gestures so it cannot change mid-gesture); m_scrollLagBaseValid is
        //     false until a sample stamped at that same scale (m_scrollStateScale) is seen, mirroring
        //     the same lazy-base idiom ScrollStateUsable() enforces elsewhere.
        //   m_scrollLagFingerX/Y: cumulative engine-px finger delta (the same idx/idy
        //     OnImageManipDelta already computes for FreeScrollBy) since gesture start.
        //   m_scrollLagMoves/Max/Sum/Last: the n=/max=/avg=/last= fields of the stage.txt line, in
        //     engine px (the |finger − applied| 2D distance at each move; a straight Euclidean norm
        //     rather than a single axis, since axis lock can pick either one per gesture).
        //   m_scrollLagPendingSinceMs/MsMax: wall-clock gap between "finger moved, engine hasn't
        //     caught up yet" and the next engine present landing (NoteScrollLagPresented) — opened
        //     on the first move after each present, closed (and folded into MsMax) by the next one.
        //     This is present time, not screen scan-out — see the commit message for the caveat.
        //   m_scrollLagClamped (2026-09-07, device bug — "n=28 max=2045" / "n=187 max=2709" at
        //     ps=1.0): counts round trips where the engine could not apply the full requested delta
        //     — a document-edge (or nested-scroller) clamp, not a real lag — and the shortfall was
        //     folded out of the base by NoteScrollLagPresented instead of being left in max/avg.
        //   m_scrollLagFlingMoves (same bug, second cause): ManipulationDelta keeps firing during
        //     TranslateInertia — a synthetic deceleration curve, not the finger — so those deltas are
        //     no longer summed into m_scrollLagFingerX/Y at all; this just counts how many arrived,
        //     reported as its own fling= field instead of masquerading as finger travel.
        bool   m_scrollLagActive { false };
        bool   m_scrollLagStarted { false };
        bool   m_scrollLagBaseValid { false };
        float  m_scrollLagScale { 1.0f };
        int    m_scrollLagBaseX { 0 }, m_scrollLagBaseY { 0 };
        double m_scrollLagFingerX { 0.0 }, m_scrollLagFingerY { 0.0 };
        int    m_scrollLagMoves { 0 };
        double m_scrollLagMax { 0.0 }, m_scrollLagSum { 0.0 }, m_scrollLagLast { 0.0 };
        unsigned long long m_scrollLagPendingSinceMs { 0 }, m_scrollLagMsMax { 0 };
        int    m_scrollLagClamped { 0 };
        int    m_scrollLagFlingMoves { 0 };
        // Apotheosis (drag as pointer events): state of the WebCoreDragAt route. m_dragGen is bumped
        // only at ManipulationStarted (NOT at ManipulationCompleted like m_nestedScrollGen), because
        // the release is posted while the gesture is still current and its answer must not be dropped.
        unsigned long long m_dragGen { 0 };
        int  m_dragStartPx { 0 }, m_dragStartPy { 0 };   // touch-down point (engine px) = the press point
        bool m_dragBusy { false };            // a WebCoreDragAt post is in flight
        bool m_dragActive { false };          // the page consumed the press and owns this gesture
        bool m_dragPressPending { false };    // phase 0 still to send
        bool m_dragPressSent { false };       // ...and it has been sent once (do not press twice)
        bool m_dragMovePending { false };     // phase 1 still to send (coalesced: latest point wins)
        bool m_dragReleasePending { false };  // phase 2/3 still to send
        bool m_dragCancel { false };          // that end is a cancel (pinch took over), not a release
        int  m_dragMoveX { 0 }, m_dragMoveY { 0 };        // latest finger position (engine px)
        int  m_dragPressX { 0 }, m_dragPressY { 0 };      // press point actually queued
        // Movement that happened before the press was answered. If nothing took the press, this is
        // what the gesture owes the normal scroll path, so it is flushed there instead of lost.
        int  m_dragFallbackDx { 0 }, m_dragFallbackDy { 0 };
        // M4 捏合缩放状态
        bool   m_pinching { false };   // 正在捏合(双指 Scale 手势);期间只变换显示层,松手提交引擎
        // Apotheosis (map-site pin, 2026-09-06): tick count of the last Holding(Started) that was
        //   turned into a long press. A hold normally ends in RightTapped rather than Tapped, but a
        //   Tapped within a second of one is the tail of that same gesture and must not reach the
        //   page as a second click.
        unsigned long long m_holdAtMs { 0 };
        // Apotheosis (link context menu, 0.1.9.42): state of the one hold that may still become a
        //   menu. m_ctxPending is set when a hold is routed to the engine's link probe and cleared
        //   by the answer, by a Holding Canceled, or by the first manipulation delta - the gate that
        //   keeps a hold-then-pan from popping a menu after the finger has left. m_ctxDip{X,Y} is
        //   the finger in RootGrid DIP (NOT engine px: the menu is a XAML element), taken when the
        //   hold started because the engine answer carries no position. m_ctxUrl is what the open
        //   menu will act on; it never leaves this object and is never traced.
        bool   m_ctxPending { false };
        double m_ctxDipX { 0.0 }, m_ctxDipY { 0.0 };
        // Apotheosis (0.1.9.46): the window and the four insets the card was placed against.
        double m_ctxWinW { 0.0 }, m_ctxWinH { 0.0 };
        double m_ctxInsetL { 0.0 }, m_ctxInsetT { 0.0 }, m_ctxInsetR { 0.0 }, m_ctxInsetB { 0.0 };
        std::wstring m_ctxUrl;
        // Apotheosis (pinch on map widgets, 2026-09-06): this pinch is being fed to the page as
        //   ctrl+wheel notches (WebCoreZoomWheelAt) rather than scaled with WebCoreSetPageScale.
        //   Decided once, at the first pinch delta, and cleared by EndGesture with the rest of the
        //   gesture state. m_pinchPageAccum is the scale change not yet worth a notch.
        bool   m_pinchPage { false };
        float  m_pinchPageAccum { 1.0f };
        int    m_pinchPagePx { 0 }, m_pinchPagePy { 0 };   // pinch centre, engine px
        int    m_zoomWheelNotches { 0 };                   // waiting for the engine (coalesced)
        bool   m_zoomWheelBusy { false };                  // a WebCoreZoomWheelAt post is in flight
        float  m_liveScale { 1.0f };   // 捏合期间相对"已提交尺度"的实时缩放(RenderTransform 用)
        float  m_pageScale { 1.0f };   // 已提交给引擎的页面缩放因子(Page::pageScaleFactor)
        // 捏合锚点。手势开始时固定一次(SetPinchAnchor),期间不再跟随焦点移动。
        double m_focalX { 360 }, m_focalY { 540 };  // 显示层 DIP(= ScaleTransform 中心)
        int    m_focalPx { 360 }, m_focalPy { 540 };// 同一点的引擎视口像素(= WebCoreSetPageScale 焦点)
        // 预览缩放变换(每次手势新建;松手后的回弹动画作用在它上面)+ 回弹 Storyboard/目标尺度。
        Windows::UI::Xaml::Media::ScaleTransform^ m_zoomTransform;
        Windows::UI::Xaml::Media::Animation::Storyboard^ m_zoomSpring;
        float m_springTargetLive { 1.0f };
        // Apotheosis: the presenting layer's translation. Written only by the live-pinch clamp
        //   (ApplyLiveZoom/ClampZoomAxis: keep the previewed frame inside the document, recentre it
        //   below 1:1) and zeroed again by SpringBackZoom/PinchCommit; composed after the preview
        //   scale by ApplyPresentTransform, so it stays in screen DIPs.
        Windows::UI::Xaml::Media::TranslateTransform^ m_panTranslate;
        Windows::UI::Xaml::Media::TransformGroup^ m_presentGroup;
        // Apotheosis (double-tap zoom 2026-09-09, route fixed 2026-09-10): OnPageTapped's tap-hold
        //   state machine, active only while m_dtapZoomEnabled is on.
        //
        //   EVERY tap on a live session is held for kDoubleTapHoldMs (m_dtapPending, m_dtapHoldTimer)
        //   instead of being dispatched, and the engine is asked in parallel whether this point is
        //   zoomable (WebCoreTapPolicyAt, answer in m_dtapPolicyReady/m_dtapZoomable/m_dtapTargetScale).
        //   The first version of this state machine only entered the held state once that ANSWER came
        //   back, and dispatched the click immediately otherwise — which cannot work: the answer
        //   travels the engine thread's FIFO queue behind live ticks and composites, so it lands
        //   later than the second tap of a real double tap, and the dispatched first click sets
        //   m_interacting/m_loading, which makes OnPageTapped drop the second tap at its first gate.
        //   The pair was therefore never recognised on the device (0.1.9.36, zero "dtap" lines).
        //   Holding first and asking in parallel decouples the two: recognising the pair is a pure
        //   UI-thread decision on time and distance, and the engine answer only decides zoom vs click.
        //
        //   Second tap near (m_dtapPx,m_dtapPy) within the interval -> DtapCompleteSecond(): zoom via
        //   RunDoubleTapZoom (SetPinchAnchorEnginePx/ApplyLiveZoom/SpringBackZoom — the same commit
        //   path a pinch release uses, see PinchCommit) when the policy said zoomable, else one click with
        //   clickCount=2 (a real DOM 'dblclick' for pages that want one, e.g. a map that zooms
        //   itself). Answer not in yet: m_dtapSecondSeen parks the decision and the answer (or the
        //   restarted timer, so a wedged engine cannot swallow the tap) completes it.
        //   No second tap: OnDtapHoldTimer forwards the ordinary click it held.
        bool   m_dtapZoomEnabled { true };   // INTERACTION toggle, default ON; settings.ini dtapzoom
        bool   m_dtapPending { false };      // a tap is held, waiting for a possible second one
        bool   m_dtapPolicyReady { false };  // WebCoreTapPolicyAt has answered for the held tap
        bool   m_dtapZoomable { false };
        bool   m_dtapSecondSeen { false };   // second tap arrived before the answer did
        int    m_dtapPx { -1 }, m_dtapPy { -1 };         // held tap, engine px (proximity + click target)
        float  m_dtapTargetScale { 1.0f };
        // Apotheosis (0.1.9.40): the zoom anchor WebCoreTapPolicyAt answered with, engine viewport
        //   px — the tap point, or a column's centre x when the driver zoomed to a column. Read by
        //   DtapCompleteSecond, fed to RunDoubleTapZoom/SetPinchAnchorEnginePx.
        int    m_dtapAnchorPx { -1 }, m_dtapAnchorPy { -1 };
        unsigned long long m_dtapGen { 0 };     // bumped per fresh tap; drops a stale WebCoreTapPolicyAt answer
        unsigned long long m_dtapAtMs { 0 };    // when the held tap happened (double-tap interval)
        Windows::UI::Xaml::DispatcherTimer^ m_dtapHoldTimer;
        // Apotheosis (review 2026-09-04 item 1): the status-bar/title-row inset of the direct
        //   present surface. GpuPanel's SIZE must never change once ANGLE has a swap chain on it
        //   (a resize rebuilds the swap chain on the engine thread's next swap, which is the
        //   libGLESv2 AV class), so the panel is translated instead of given a top margin. Always
        //   the outermost transform on GpuPanel — see ApplyPresentTransform().
        Windows::UI::Xaml::Media::TranslateTransform^ m_gpuInset;
        // Apotheosis (review 2026-09-04 item 2): last insets ApplyViewInsets() actually applied.
        //   It is called from every source that could move an edge, so most calls are no-ops.
        double m_lastInsetTop { 0.0 }, m_lastInsetBottom { 0.0 };
        // Apotheosis (landscape, 0.1.9.44): the same on the horizontal edges. In landscape the
        //   software navigation bar sits on the RIGHT, so a window that extends under it (which
        //   ours does, UseCoreWindow) owes an inset there exactly as it owes one at the bottom in
        //   portrait. Nothing looked at those two numbers until now, which is why the settings
        //   panel and the suggestion dropdown ran under the buttons.
        double m_lastInsetLeft { 0.0 }, m_lastInsetRight { 0.0 };
        // Apotheosis (0.1.9.47): m_lastStripH is gone with the loading strip's top inset — the strip
        //   is a pure overlay and no longer part of this cache key (nor of any layout).
        // Apotheosis (2837ce0 review item 1): TitleRow, the auto-hiding title/toast strip
        //   at the bottom edge of the content row — 0 while it is collapsed, its height while it is
        //   shown. It is an overlay, so this is the content area's BOTTOM inset; GpuPanel keeps its
        //   size (see the XAML comment) and simply has that strip covered.
        double m_lastTitleH { 0.0 };
        bool   m_insetsValid { false };
        // Apotheosis (bug fix 2026-09-06 evening): on-screen keyboard state, recorded by the InputPane
        //   Showing/Hiding handlers and consumed by ApplyKeyboardShift(). m_kbTop/m_kbHeight are
        //   InputPane::OccludedRect's Y/Height in CoreWindow-local DIP; the TOP edge is what the shift
        //   is anchored on, so the result does not depend on how far down the rect extends (0.1.9.16
        //   and 0.1.9.18 each guessed that differently and each got it wrong in one direction).
        //   m_kbShiftApplied is what NavBarShift/ShiftSuggestPanel currently carry, so the recompute
        //   can be called from anywhere and stays silent when nothing changes.
        bool   m_kbVisible { false };
        double m_kbTop { 0.0 }, m_kbHeight { 0.0 };
        // Apotheosis (0.1.9.44): true while the recorded rectangle is known NOT to belong to the
        //   current window (it fails KeyboardRectPlausible) - the shell answers a rotation with the
        //   previous orientation's rectangle for a moment. Nothing may be computed from it while
        //   this is set: the shift stays where it is until a plausible rectangle or a Showing event
        //   arrives. m_kbRecheckTries bounds the deferred re-query so it cannot become a loop.
        bool   m_kbMetricsStale { false };
        // Apotheosis (0.1.9.46): the last value handed to WebCoreSetBottomOcclusion(), so a push
        //   that changes nothing costs nothing. -1 = never sent (0 is a legitimate value).
        int    m_bottomOccSent { -1 };
        int    m_kbRecheckTries { 0 };
        double m_kbShiftApplied { 0.0 };
        // Apotheosis (suggestion tap, 2026-09-10): a shift back to rest while the suggestion dropdown
        //   is up is deferred (QueueKeyboardShiftRestore) instead of applied — moving the panel is what
        //   made a tapped suggestion do nothing. The token retires a superseded restore; the bypass is
        //   the one-shot ticket with which the deferred call gets past that same test.
        unsigned long long m_kbShiftToken { 0 };
        bool   m_kbShiftBypass { false };
        // The keyboard offset of the two bottom-anchored content-row overlays. SuggestPanel has no
        //   other transform; TitleRow's own TitleRowShift is ANIMATED (reveal/collapse slide) and a
        //   Storyboard's value overrides a local one both while it runs and, with HoldEnd, afterwards
        //   — so the keyboard offset gets its own transform, grouped with the animated one.
        Windows::UI::Xaml::Media::TranslateTransform^ m_suggestKbShift;
        Windows::UI::Xaml::Media::TranslateTransform^ m_titleKbShift;
        // Apotheosis (2837ce0 review item 1): auto-hide of the title/toast row. The timer is the
        //   one-shot "idle for ~2 s → slide away"; the token drops a slide-up/slide-down that a later
        //   Reveal()/Collapse() overtook (Storyboard::Completed still fires after Stop()).
        // Apotheosis (title row slide, review 2026-09-04): renamed from m_titleFade — the row no
        //   longer fades (Opacity), it slides on TitleRowShift (its TranslateTransform.Y); the same
        //   single Storyboard^ slot is reused for whichever direction is currently running (Reveal
        //   stops a live Collapse and vice versa, so only one is ever active).
        Windows::UI::Xaml::DispatcherTimer^ m_titleHideTimer;
        Windows::UI::Xaml::Media::Animation::Storyboard^ m_titleAnim;
        unsigned long long m_titleRowToken { 0 };
        bool m_titleRowShown { true };   // matches the XAML (TitleRow starts visible)
        // Apotheosis (2026-09-04): the row hides itself ~2 s after a load, which is right while the
        //   user is reading and wrong while they are typing - the address bar is the thing they are
        //   looking at and the row sits right above it. Pinned = no hide timer, no collapse: set
        //   while UrlBox has focus and while the on-screen keyboard is up, cleared on blur/hide,
        //   which re-arms the usual grace period.
        bool m_titleRowPinned { false };
        // Apotheosis (0.1.9.45): the link context card holds the title row up for as long as it is
        //   open, and takes it down with itself. Its own flag rather than m_titleRowPinned, which
        //   belongs to address-bar editing - the two can overlap and neither may clear the other.
        bool m_titleRowCtxPinned { false };
        // Last scroll position/bounds the engine reported (WebCoreGetScrollState). Used to clamp the
        //   pinch preview to the document, to map taps into engine space and to measure how far the
        //   engine really got (the touch-lag diagnostic).
        int  m_scrollX { 0 }, m_scrollY { 0 };
        int  m_contentW { 0 }, m_contentH { 0 }, m_viewW { 0 }, m_viewH { 0 };
        bool m_scrollStateValid { false };
        unsigned long long m_scrollStateGen { 0 };   // drops answers from a superseded gesture
        // Apotheosis (2837ce0 review item 2): the page scale the cache above was captured at. The
        //   cached content/view size and scroll position are engine px, and engine px per CSS px
        //   is exactly m_pageScale — so a pinch commit invalidates all six fields at once, whether
        //   or not anything remembered to say so. See ScrollStateUsable().
        float m_scrollStateScale { 1.0f };
        // Apotheosis (2837ce0 review item 2): ApplyViewInsets() was skipped because a pinch was in
        //   flight; re-run it once the commit lands. Moving the content edges mid-gesture would
        //   resize ContentArea under a pinch anchor that was frozen against the old size, and
        //   ApplyPresentTransform() would rebuild the very transform group SpringBackZoom animates.
        bool m_insetsPending { false };
        bool m_pointerDown { false }; // 指针按下中(拖拽跟踪)
        bool m_dragging { false };    // 已超过阈值判定为拖拽(非点击)
        double m_dragLastY { 0 };     // 上次指针 Y(算增量)
        double m_dragStartY { 0 };    // 按下时指针 Y(算是否越过拖拽阈值)

        // 加载看门狗:保证 m_loading 总能被复位(即使完成回调因 dispatcher 断开/低内存丢失,
        // 避免导航永久锁死)。
        Windows::UI::Xaml::DispatcherTimer^ m_loadWatchdog;

        // 软件呈现双缓冲(PresentSoftwareFrame 交替写,免每帧新建 3MB WriteableBitmap)
        Windows::UI::Xaml::Media::Imaging::WriteableBitmap^ m_frameBmpA;
        Windows::UI::Xaml::Media::Imaging::WriteableBitmap^ m_frameBmpB;
        bool m_frameBmpFlip { false };
        // Apotheosis: 标签切换快照的显示位图(按需建,HideTabSnapshot 里放掉)+ 当前是否正显示快照。
        Windows::UI::Xaml::Media::Imaging::WriteableBitmap^ m_snapBmp;
        bool m_snapshotShown { false };
        unsigned long long m_snapSeq { 0 };   // 快照新鲜度计数(PruneTabSnapshots 用)

        // 实时渲染循环状态
        bool m_liveBusy { false };           // 上一帧 LiveTick 引擎任务未回,避免堆积
        int m_liveBusyAge { 0 };             // m_liveBusy 已持续的 tick 数;>阈值则自愈(RunAsync 丢了不死循环)
        std::atomic<bool> m_appForeground { true };  // 应用在前台(后台暂停);引擎线程也读,故 atomic
        unsigned m_lastFrameHash { 0 };      // 上一帧哈希(判断画面是否变化)
        int m_liveTotalTicks { 0 };          // 连续动画的累计帧数;超阈值降帧率(防永久动画耗电)
        // Apotheosis (event-driven present): the live loop is driven by engine wake-ups.
        //   m_wakeTimer is the one-shot that enforces the ~16 ms minimum gap between presents,
        //   m_fallbackTimer the safety net. All of these are UI-thread only.
        Windows::UI::Xaml::DispatcherTimer^ m_wakeTimer;
        Windows::UI::Xaml::DispatcherTimer^ m_fallbackTimer;
        bool m_wakePending { false };            // a wake arrived that no composite has served yet
        unsigned long long m_lastPresentMs { 0 };// GetTickCount64() when the last frame came back
        unsigned m_lastPresentDurMs { 0 };       // its engine-side cost — the rate limit follows it
        int m_fallbackStaticTicks { 0 };         // consecutive frames with nothing new (200 ms -> 1 s -> 5 s)
    };
}
