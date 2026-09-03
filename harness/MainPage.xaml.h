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
    enum class NestedScrollState { Unknown, Yes, No };

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
        // 切回来时先贴出它、再让真实重载在下面跑(TABS-PLAN.md 方案 a)。snapSeq 用于超出
        // 上限时丢最老的一张。
        std::shared_ptr<std::vector<uint8_t>> snapshot;
        unsigned long long snapSeq { 0 };
    };

    public ref class MainPage sealed {
    public:
        MainPage();
        // App::OnSuspending 调用:把 cookie JSON 落盘转给引擎线程串行执行,完成后 Complete 传入的
        // deferral(见 App.xaml.cpp 注释——这是真正可靠的挂起前落盘点,取代 VisibilityChanged 那种
        // fire-and-forget)。
        void FlushCookiesForSuspend(Windows::ApplicationModel::SuspendingDeferral^ deferral);

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
        // Apotheosis: 标签切换快照(TABS-PLAN.md 方案 a)。
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
        // 把内容区显示坐标(DIP)映回引擎像素空间(直呈现下表面被拉伸+设备分辨率缩放),修点击/焦点偏移。
        void MapTapToEngine(double dipX, double dipY, int& outPx, int& outPy);
        // 把位图像素 (px,py) 的点击转发到引擎活会话(WebCoreClickAt),完成后同步地址栏/历史/链接表。
        void ForwardClickToEngine(int px, int py);
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
        // ---- Apotheosis (instant pan, developer setting "Instant pan"): the last frame follows
        // the finger. A main-frame pan is applied to the presenting element as a XAML
        // TranslateTransform on the UI thread *immediately*, while the coalesced WebCoreScrollBy
        // goes to the engine exactly as before; every engine frame that lands subtracts what the
        // engine really scrolled, so the transform only ever carries the not-yet-applied remainder.
        void InstantPanBy(int dx, int dy);            // finger asked for this much more (engine px)
        void InstantPanApplied(int newScrollX, int newScrollY, bool haveScrollState,
                               int fallbackDx, int fallbackDy);   // an engine frame landed
        void InstantPanReset();                       // remainder → 0, transform → identity
        void ClampPanRemainder();                     // document bounds + one screen
        void ApplyPanTransform();                     // remainder (engine px) → translation (DIP)
        void RequestScrollState();                    // seed the cached scroll/bounds (async)
        void RestartPanSnapTimer();                   // ~1 s after the last movement the engine wins
        void OnPanSnapTick(Platform::Object^ sender, Platform::Object^ e);
        // Apotheosis: compose the pinch preview scale and the instant-pan translation onto the
        //   presenting element (TransformGroup, scale first so the translation stays screen-space).
        void ApplyPresentTransform();
        // Apotheosis (OFFTHREAD-RASTER-LOG.md): push the "Threaded raster" developer setting to
        //   the engine thread. Never called from the UI thread without a post.
        void ApplyThreadedRasterSetting();
        void ApplyLiveZoom();
        void PinchCommit(float newScale, int focalX, int focalY);
        // Apotheosis: the layer that shows the engine output (GpuPanel in direct-present mode,
        //   RenderImage otherwise) — the pinch preview transform hangs off it.
        Windows::UI::Xaml::FrameworkElement^ PresentLayer();
        // Apotheosis: fix the pinch anchor (once per gesture) from a ContentArea DIP position;
        //   fills m_focalX/Y (transform centre) and m_focalPx/Py (engine pixels for the commit).
        void SetPinchAnchor(double dipX, double dipY);
        // Apotheosis: ease the preview from where the fingers left it to the scale we commit
        //   (overview below 1:1, ±6 % snap), then call PinchCommit. Composition-thread animation.
        void SpringBackZoom(float targetLive, float commitScale);
        // 实时渲染循环:低帧率驱动引擎 WebCoreLiveTick,让 CSS/JS 动画动起来、SPA 多帧渐进挂载。
        // 画面连续静止则自动停帧省电,交互/滚动/导航再启动。
        void StartLiveMode();
        void StopLiveMode();
        void OnLiveTick(Platform::Object^ sender, Platform::Object^ e);
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
        bool m_urlDeleteBtnHidden { false };  // UrlBox 模板自带的 ✕ 已收起(只做一次)
        bool m_updateChecking { false };  // 检测更新进行中(防并发重复点)
        bool m_updateAutoChecked { false };  // 启动后已静默自检过一次(首个网络页加载完触发,CA 此时已就绪)
        // 设置(持久化到 LocalState\settings.ini;搜索前缀/主页是全局,见 .cpp)
        bool m_langSet { false };     // settings.ini 里是否已存过 lang(否=首次启动→弹 OOBE)
        int  m_setSearch { 4 };       // 搜索引擎索引(0 Bing/1 Google/2 DuckDuckGo/3 百度/4 Qwant);新装默认 Qwant
        bool m_setUaDesktop { false };// 启动默认请求桌面版网站
        int  m_defaultZoom { 100 };   // 默认缩放百分比(50–200)
        int  m_tabMode { 0 };         // 0=单热会话 / 1=并发多引擎(实验);增量5/7 使用
        std::wstring m_uaCustom;      // 自定义 UA(空=用 mobile/desktop 开关);settings.ini ua_custom
        bool m_updateAuto { false };  // 隐私：启动后自动查 GitHub 更新（默认关）；settings.ini updatecheck
        int  m_prefetch { 0 };        // 隐私：推测预取 0=关/1=仅 Wi-Fi(不计费连接)/2=始终；settings.ini prefetch
        bool m_showScrollFab { false };// 开发者选项:悬浮翻页按钮(默认关);settings.ini scrollfab
        // Apotheosis: DEVELOPER toggles. Instant pan is on by default (it is what makes a pan feel
        //   like a pan); threaded raster is the off-by-default night A/B (OFFTHREAD-RASTER-LOG.md).
        bool m_instantPan { true };     // settings.ini instantpan
        bool m_threadedRaster { false };// settings.ini threadraster
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
        bool m_gpuSizeHandlerWired { false };// GpuPanel->SizeChanged 是否已经挂过(避免 HookGpuPanelForStartup 重入重复订阅)
        Windows::Foundation::Collections::PropertySet^ m_gpuProps;  // ANGLE 原生窗口(SwapChainPanel 包装),保活
        int  m_gpuOrient { 0 };       // 离屏 readback 朝向(bit0=H,bit1=V):0=none(真机实测正确),1=H,2=V,3=HV
        // 自由滚动状态
        int  m_scrollAccum { 0 };     // 未冲刷的累积竖向滚动位移(像素,>0 向下)
        int  m_scrollAccumX { 0 };    // 未冲刷的累积横向滚动位移(像素,>0 向右)
        bool m_scrollBusy { false };  // 有 WebCoreScrollBy 任务在引擎线程飞行
        // Apotheosis: nested-scroll routing state (WebCoreIsScrollableAt/WebCoreWheelAt, d982774).
        NestedScrollState m_nestedScrollState { NestedScrollState::Unknown };  // this gesture's answer
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
        // M4 捏合缩放状态
        bool   m_pinching { false };   // 正在捏合(双指 Scale 手势);期间只变换显示层,松手提交引擎
        float  m_liveScale { 1.0f };   // 捏合期间相对"已提交尺度"的实时缩放(RenderTransform 用)
        float  m_pageScale { 1.0f };   // 已提交给引擎的页面缩放因子(Page::pageScaleFactor)
        // 捏合锚点。手势开始时固定一次(SetPinchAnchor),期间不再跟随焦点移动。
        double m_focalX { 360 }, m_focalY { 540 };  // 显示层 DIP(= ScaleTransform 中心)
        int    m_focalPx { 360 }, m_focalPy { 540 };// 同一点的引擎视口像素(= WebCoreSetPageScale 焦点)
        // 预览缩放变换(每次手势新建;松手后的回弹动画作用在它上面)+ 回弹 Storyboard/目标尺度。
        Windows::UI::Xaml::Media::ScaleTransform^ m_zoomTransform;
        Windows::UI::Xaml::Media::Animation::Storyboard^ m_zoomSpring;
        float m_springTargetLive { 1.0f };
        // ---- Apotheosis (instant pan) ----
        // m_panRem* = engine viewport px the finger has asked for that the engine has not applied
        //   yet. The transform shows -m_panRem*, converted to presenting-layer DIPs.
        Windows::UI::Xaml::Media::TranslateTransform^ m_panTranslate;
        Windows::UI::Xaml::Media::TransformGroup^ m_presentGroup;
        Windows::UI::Xaml::DispatcherTimer^ m_panSnapTimer;
        int  m_panRemX { 0 }, m_panRemY { 0 };
        // Last scroll position/bounds the engine reported (WebCoreGetScrollState). Only used to
        //   clamp the preview to the document and to measure how far the engine really got.
        int  m_scrollX { 0 }, m_scrollY { 0 };
        int  m_contentW { 0 }, m_contentH { 0 }, m_viewW { 0 }, m_viewH { 0 };
        bool m_scrollStateValid { false };
        unsigned long long m_scrollStateGen { 0 };   // drops answers from a superseded gesture
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
        Windows::UI::Xaml::DispatcherTimer^ m_liveTimer;
        bool m_liveBusy { false };           // 上一帧 LiveTick 引擎任务未回,避免堆积
        int m_liveBusyAge { 0 };             // m_liveBusy 已持续的 tick 数;>阈值则自愈(RunAsync 丢了不死循环)
        std::atomic<bool> m_appForeground { true };  // 应用在前台(后台暂停);引擎线程也读,故 atomic
        unsigned m_lastFrameHash { 0 };      // 上一帧哈希(判断画面是否变化)
        int m_liveStaticTicks { 0 };         // 连续静止帧数,达阈值停帧
        int m_liveTotalTicks { 0 };          // 连续动画的累计帧数;超阈值降帧率(防永久动画耗电)
    };
}
