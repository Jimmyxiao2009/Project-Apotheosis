#pragma once
#include "MainPage.g.h"
#include <vector>
#include <string>
#include <memory>
#include <atomic>

namespace Harness {

    // 书签/历史/下载条目(原生结构,非 WinRT)。
    struct Entry {
        std::wstring url;
        std::wstring title;
        std::wstring extra;   // 历史:时间;下载:文件名/状态
    };

    enum class DrawerTab { Favorites, History, Downloads };

    // 网页链接命中矩形(位图坐标)+ URL,用于点击交互。
    struct PageLink { int x, y, w, h; std::wstring url; };

    public ref class MainPage sealed {
    public:
        MainPage();

    private:
        // ---- 工具栏 ----
        void OnBack(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnForward(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnHome(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnGo(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnUrlKeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void OnMenu(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // UA 切换:手机/桌面,切后重载当前页(遇到对移动 UA 抽风的站点用)。
        void OnToggleUA(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);

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
        // 把位图像素 (px,py) 的点击转发到引擎活会话(WebCoreClickAt),完成后同步地址栏/历史/链接表。
        void ForwardClickToEngine(int px, int py);
        // 引擎滚动 dy 像素(触发懒加载图片)后重绘。dy>0 向下。
        void EngineScroll(int dy);
        void OnScrollUp(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnScrollDown(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // 自由滚动:内容区 ManipulationDelta(单指拖拽 ΔY)→ 累积位移 → 合并成引擎滚动(无 spinner,带惯性)。
        void FreeScrollBy(int dy);   // 累积 dy 并在引擎空闲时冲刷
        void PumpScroll();           // 把累积位移作为一次 WebCoreScrollBy 派发(完成后若仍有累积再派发)
        void OnImageManipDelta(Platform::Object^ sender, Windows::UI::Xaml::Input::ManipulationDeltaRoutedEventArgs^ e);
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
        // kind: 0=插入文本(text),1=回车,2=退格。转发到引擎并重绘。
        void SendKeyToEngine(int kind, Platform::String^ text);
        // GPU 路径1 探针:SwapChainPanel 就绪后启动 ANGLE 三角形探针(验 GPU 管线在 App Container 通)。
        void OnGpuPanelLoaded(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // 把一帧引擎渲染结果(rgba)贴到位图 + 同步标题/地址/链接表;navUrl 非空表示会话内发生了导航。
        void ApplyEngineFrame(const std::shared_ptr<std::vector<uint8_t>>& rgba,
                              Platform::String^ title, Platform::String^ navUrl,
                              const std::shared_ptr<std::vector<PageLink>>& links);

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
        // 自由滚动状态
        int  m_scrollAccum { 0 };     // 未冲刷的累积滚动位移(像素,>0 向下)
        bool m_scrollBusy { false };  // 有 WebCoreScrollBy 任务在引擎线程飞行
        bool m_pointerDown { false }; // 指针按下中(拖拽跟踪)
        bool m_dragging { false };    // 已超过阈值判定为拖拽(非点击)
        double m_dragLastY { 0 };     // 上次指针 Y(算增量)
        double m_dragStartY { 0 };    // 按下时指针 Y(算是否越过拖拽阈值)

        // 加载看门狗:保证 m_loading 总能被复位(即使完成回调因 dispatcher 断开/低内存丢失,
        // 避免导航永久锁死)。
        Windows::UI::Xaml::DispatcherTimer^ m_loadWatchdog;

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
