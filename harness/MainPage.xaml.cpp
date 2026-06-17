#include "pch.h"
#include "MainPage.xaml.h"
#include "MainPage.g.hpp"
#include "WebCoreDriver.h"
#include "JitProbe.h"

#include <robuffer.h>
#include <wrl.h>
#include <windows.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <algorithm>
#include <cstdlib>

using namespace Harness;
using namespace Platform;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Media::Imaging;
using namespace Windows::UI::Core;
using namespace Microsoft::WRL;

// ===== 字符串/路径辅助 =====
static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string ToUtf8(Platform::String^ s)
{
    return s ? WideToUtf8(std::wstring(s->Data())) : std::string{};
}
static std::wstring ToWide(const char* s) { return s ? Utf8ToWide(std::string(s)) : std::wstring{}; }

static std::wstring LocalStateDir()
{
    using namespace Windows::Storage;
    try { return std::wstring(ApplicationData::Current->LocalFolder->Path->Data()); }
    catch (...) { return {}; }
}
static std::wstring InstallDir()
{
    using namespace Windows::ApplicationModel;
    try { return std::wstring(Package::Current->InstalledLocation->Path->Data()); }
    catch (...) { return {}; }
}

// ===== 运行期配置:fontconfig(含 SimHei CJK 回退)+ CA blob =====
static void SetupRuntimeEnv()
{
    try {
        std::string installDir = WideToUtf8(InstallDir());
        std::string localDir = WideToUtf8(LocalStateDir());
        if (installDir.empty() || localDir.empty())
            return;

        std::string fontsDir = installDir + "\\Assets\\fonts";
        std::string cacheDir = localDir + "\\fontconfig-cache";
        std::string confPath = localDir + "\\fonts.conf";
        std::ofstream conf(confPath, std::ios::binary | std::ios::trunc);
        if (conf) {
            conf << "<?xml version=\"1.0\"?>\n<fontconfig>\n";
            conf << "  <dir>" << fontsDir << "</dir>\n";
            conf << "  <cachedir>" << cacheDir << "</cachedir>\n";
            conf << "  <match target=\"pattern\"><test name=\"family\"><string>sans-serif</string></test>"
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Segoe UI</string><string>Arial</string><string>SimHei</string></edit></match>\n";
            conf << "  <match target=\"pattern\"><test name=\"family\"><string>serif</string></test>"
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Times New Roman</string><string>SimHei</string></edit></match>\n";
            conf << "  <match target=\"pattern\"><test name=\"family\"><string>monospace</string></test>"
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Courier New</string><string>SimHei</string></edit></match>\n";
            // 兜底:任何字族缺字形 → Segoe UI 再 → SimHei(CJK)。
            conf << "  <match target=\"pattern\"><edit name=\"family\" mode=\"append\" binding=\"weak\"><string>Segoe UI</string><string>SimHei</string></edit></match>\n";
            conf << "</fontconfig>\n";
            conf.close();
            _putenv_s("FONTCONFIG_FILE", confPath.c_str());
        }

        // CA 根证书:内存 blob 注入(绕 App Container 文件式加载限制)。
        std::string srcCa = installDir + "\\cacert.pem";
        std::vector<uint8_t> caBytes;
        std::ifstream in(srcCa, std::ios::binary | std::ios::ate);
        if (in) {
            std::streamsize n = in.tellg();
            if (n > 0) {
                caBytes.resize((size_t)n);
                in.seekg(0);
                in.read(reinterpret_cast<char*>(caBytes.data()), n);
            }
        }
        if (!caBytes.empty())
            WebCoreSetCACertBlob(caBytes.data(), (int)caBytes.size());
    } catch (...) {}
}

static void WriteStage(const char* stage)
{
    try {
        std::wstring d = LocalStateDir();
        if (d.empty()) return;
        std::ofstream f(WideToUtf8(d) + "\\stage.txt", std::ios::binary | std::ios::trunc);
        if (f) f << stage << "\n";
    } catch (...) {}
}

// 本地起始页(主页),WebCoreRenderHtml 渲染。CJK 已可用(SimHei)。
static const char* kHomeHtml =
    "<html><head><meta charset='utf-8'></head>"
    "<body style='margin:0;background:#f5f6f8;font-family:sans-serif;color:#202124'>"
    "<div style='background:linear-gradient(135deg,#00aa77,#0088cc);color:#fff;padding:40px 24px'>"
    "<h1 style='margin:0;font-size:48px'>EdgeHTML Reborn</h1>"
    "<p style='margin:8px 0 0;font-size:22px;opacity:.9'>现代浏览器引擎 &middot; Windows 10 Mobile &middot; ARM32</p></div>"
    "<div style='padding:28px 24px'>"
    "<p style='font-size:26px;margin:0 0 18px'>在上方地址栏输入网址访问网页。</p>"
    "<div style='background:#fff;border-radius:14px;padding:20px 24px;box-shadow:0 2px 8px rgba(0,0,0,.08)'>"
    "<p style='margin:0 0 10px;font-size:20px;color:#5f6368'>引擎能力</p>"
    "<p style='margin:6px 0;font-size:22px'>HTTPS &middot; TLS 1.3 &middot; JavaScript &middot; 重定向 &middot; 中文字体</p>"
    "<p style='margin:6px 0;font-size:22px'>WebKit (WebCore) 2.52.4</p></div>"
    "<p style='margin:22px 0 0;font-size:20px;color:#80868b'>试试 &nbsp;example.com &nbsp;&middot;&nbsp; github.com &nbsp;&middot;&nbsp; bing.com</p>"
    "</div></body></html>";

static std::string MakeErrorHtml(const std::string& url, const char* err)
{
    std::string e = err ? err : "";
    return "<html><head><meta charset='utf-8'></head>"
        "<body style='margin:0;background:#fff;font-family:sans-serif'>"
        "<div style='background:#d93025;color:#fff;padding:32px 24px'><h1 style='margin:0;font-size:38px'>无法访问此页面</h1></div>"
        "<div style='padding:24px;color:#333;font-size:24px'><p style='word-break:break-all;color:#1a73e8'>" + url + "</p>"
        "<p style='color:#d93025;font-size:22px;word-break:break-all'>" + e + "</p></div></body></html>";
}

static Platform::String^ NormalizeUrl(Platform::String^ raw)
{
    std::wstring s = raw ? std::wstring(raw->Data()) : L"";
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    if (s.empty())
        return ref new String(L"about:home");
    // 含空格或没有点且不像域名 → 当作搜索词走 Bing。
    bool looksUrl = (s.find(L"://") != std::wstring::npos) || (s.find(L'.') != std::wstring::npos && s.find(L' ') == std::wstring::npos);
    if (s.rfind(L"about:", 0) == 0)
        return ref new String(s.c_str());
    if (!looksUrl) {
        std::wstring q;
        for (wchar_t c : s) { if (c == L' ') q += L"%20"; else q += c; }
        return ref new String((L"https://cn.bing.com/search?q=" + q).c_str());
    }
    if (s.find(L"://") == std::wstring::npos)
        s = L"https://" + s;
    return ref new String(s.c_str());
}

// ===== 单一引擎线程:WebCore/JSC 严格单线程,所有引擎调用串行其上 =====
class WebEngine {
public:
    static WebEngine& instance() { static WebEngine e; return e; }
    void post(std::function<void()> job)
    {
        { std::lock_guard<std::mutex> lk(m_mtx); m_q.push_back(std::move(job)); }
        m_cv.notify_one();
    }
private:
    WebEngine() { std::thread([this] { loop(); }).detach(); }
    void loop()
    {
        SetupRuntimeEnv();   // 一次,在引擎线程,字体 + CA,必须在首次加载前。
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cv.wait(lk, [this] { return !m_q.empty(); });
                job = std::move(m_q.front());
                m_q.pop_front();
            }
            try { job(); } catch (...) {}
        }
    }
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_q;
};

// ===== 渲染缓冲 → WriteableBitmap(RGBA→BGRA)=====
static void BlitToBitmap(WriteableBitmap^ wb, const std::vector<uint8_t>& rgba, int W, int H)
{
    ComPtr<Windows::Storage::Streams::IBufferByteAccess> bba;
    reinterpret_cast<IInspectable*>(wb->PixelBuffer)->QueryInterface(IID_PPV_ARGS(&bba));
    byte* dst = nullptr;
    bba->Buffer(&dst);
    const size_t n = (size_t)W * H;
    const uint8_t* src = rgba.data();
    for (size_t i = 0; i < n; ++i) {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = src[i * 4 + 3];
    }
}

static const int kW = 720, kH = 1080;

// ============================================================================
MainPage::MainPage()
{
    InitializeComponent();
    // 一次性可执行内存探针(JIT 可行性),结果写 LocalState\jitresult.txt 供 WDP 拉取。
    try {
        std::string jit = RunJitProbe();
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ofstream jf(WideToUtf8(d) + "\\jitresult.txt", std::ios::binary | std::ios::trunc);
            if (jf) jf.write(jit.data(), jit.size());
        }
    } catch (...) {}
    LoadData();
    // 前后台切换:后台暂停实时渲染(省电、避免后台跑引擎被 PLM 冻结时堆积)。
    Window::Current->VisibilityChanged += ref new Windows::UI::Xaml::WindowVisibilityChangedEventHandler(
        [this](Platform::Object^, Windows::UI::Core::VisibilityChangedEventArgs^ e) {
            m_appForeground = e->Visible;
            if (e->Visible) StartLiveMode(); else StopLiveMode();
        });
    // 测试钩子:若 LocalState\testurl.txt 存在,启动直接导航到它(供 WDP 远程自动化测试,免 UI 输入)。
    std::wstring testUrl;
    try {
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ifstream f(WideToUtf8(d) + "\\testurl.txt", std::ios::binary);
            if (f) { std::string s; std::getline(f, s); testUrl = Utf8ToWide(s); }
            while (!testUrl.empty() && (testUrl.back() == L'\r' || testUrl.back() == L'\n' || testUrl.back() == L' ' || testUrl.back() == L'\t'))
                testUrl.pop_back();
        }
    } catch (...) {}
    if (!testUrl.empty())
        NavigateTo(ref new String(testUrl.c_str()), true);
    else
        NavigateTo(ref new String(L"about:home"), true);
}

// ---- 持久化 ----
static std::vector<Entry> ReadEntries(const std::wstring& path)
{
    std::vector<Entry> out;
    std::ifstream f(WideToUtf8(path), std::ios::binary);
    if (!f) return out;
    std::stringstream ss; ss << f.rdbuf();
    std::string all = ss.str();
    std::wstring w = Utf8ToWide(all);
    std::wstringstream ws(w);
    std::wstring line;
    while (std::getline(ws, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        Entry e;
        size_t t1 = line.find(L'\t');
        size_t t2 = (t1 == std::wstring::npos) ? std::wstring::npos : line.find(L'\t', t1 + 1);
        if (t1 == std::wstring::npos) { e.url = line; }
        else {
            e.url = line.substr(0, t1);
            if (t2 == std::wstring::npos) e.title = line.substr(t1 + 1);
            else { e.title = line.substr(t1 + 1, t2 - t1 - 1); e.extra = line.substr(t2 + 1); }
        }
        out.push_back(e);
    }
    return out;
}
static void WriteEntries(const std::wstring& path, const std::vector<Entry>& v)
{
    std::wstring w;
    for (auto& e : v) { w += e.url; w += L'\t'; w += e.title; w += L'\t'; w += e.extra; w += L'\n'; }
    std::ofstream f(WideToUtf8(path), std::ios::binary | std::ios::trunc);
    if (f) { std::string u = WideToUtf8(w); f.write(u.data(), u.size()); }
}

void MainPage::LoadData()
{
    std::wstring d = LocalStateDir();
    if (d.empty()) return;
    m_bookmarks = ReadEntries(d + L"\\bookmarks.tsv");
    m_historyList = ReadEntries(d + L"\\history.tsv");
    m_downloads = ReadEntries(d + L"\\downloads.tsv");
}
void MainPage::SaveBookmarks() { std::wstring d = LocalStateDir(); if (!d.empty()) WriteEntries(d + L"\\bookmarks.tsv", m_bookmarks); }
void MainPage::SaveHistory()   { std::wstring d = LocalStateDir(); if (!d.empty()) WriteEntries(d + L"\\history.tsv", m_historyList); }
void MainPage::SaveDownloads() { std::wstring d = LocalStateDir(); if (!d.empty()) WriteEntries(d + L"\\downloads.tsv", m_downloads); }

void MainPage::AddHistory(const std::wstring& url, const std::wstring& title)
{
    if (url.empty() || url == L"about:home") return;
    m_historyList.erase(std::remove_if(m_historyList.begin(), m_historyList.end(),
        [&](const Entry& e) { return e.url == url; }), m_historyList.end());
    Entry e; e.url = url; e.title = title.empty() ? url : title;
    m_historyList.insert(m_historyList.begin(), e);
    if (m_historyList.size() > 300) m_historyList.resize(300);
    SaveHistory();
}
bool MainPage::IsBookmarked(const std::wstring& url)
{
    for (auto& b : m_bookmarks) if (b.url == url) return true;
    return false;
}

// ---- 导航 ----
void MainPage::UpdateNavButtons()
{
    BackBtn->IsEnabled = (m_navIndex > 0);
    FwdBtn->IsEnabled = (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() - 1);
}
void MainPage::SetLoading(bool loading)
{
    m_loading = loading;
    Progress->IsIndeterminate = loading;
    Progress->Visibility = loading ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::NavigateTo(Platform::String^ url, bool pushHistory)
{
    if (m_loading) return;

    std::wstring wurl = url ? std::wstring(url->Data()) : L"about:home";
    const bool isHome = wurl.empty() || wurl == L"about:home";
    m_currentUrl = isHome ? L"about:home" : wurl;

    if (pushHistory) {
        if (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() - 1)
            m_navStack.erase(m_navStack.begin() + m_navIndex + 1, m_navStack.end());
        m_navStack.push_back(wurl);
        m_navIndex = (int)m_navStack.size() - 1;
    }
    UpdateNavButtons();
    UrlBox->Text = isHome ? ref new String(L"") : url;
    TitleText->Text = isHome ? ref new String(L"主页") : ref new String((L"加载中  " + wurl).c_str());
    SetLoading(true);

    // 加载看门狗:即使完成回调因 dispatcher 断开/低内存而丢失,40s 后也强制复位 m_loading,
    // 避免导航永久锁死(代码审查确认的真实 hang)。引擎侧 30s 看门狗保证 job 必返回,UI 侧 40s 兜底。
    if (!m_loadWatchdog) {
        m_loadWatchdog = ref new Windows::UI::Xaml::DispatcherTimer();
        Windows::Foundation::TimeSpan ts; ts.Duration = 40LL * 10000000LL;   // 40s(100ns 单位)
        m_loadWatchdog->Interval = ts;
        m_loadWatchdog->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnLoadWatchdog);
    }
    m_loadWatchdog->Start();

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    std::string surl = ToUtf8(url);
    unsigned long long mySeq = ++m_opSeq;

    WebEngine::instance().post([disp, self, surl, isHome, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        bool loadOk = false;   // 网络加载是否真成功(区别于错误页渲染成功),决定是否进历史
        bool sessionActive = false;   // 是否建立了引擎常驻会话(决定点击转发/翻页按钮)
        std::wstring title;
        try {
            if (isHome) {
                WebCoreCloseSession();   // 离开网络页:销毁会话,释放 Page + 取消在途加载
                rc = WebCoreRenderHtml(kHomeHtml, kW, kH, rgba->data());
                loadOk = (rc == 0);
                title = L"主页";
            } else {
                WriteStage(("before-load " + surl).c_str());
                int netRc = WebCoreSessionLoad(surl.c_str(), kW, kH, rgba->data());   // 常驻会话加载
                char t[512] = ""; WebCoreGetTitle(t, sizeof t);
                char diag[4096] = ""; WebCoreGetDiag(diag, sizeof diag);
                WriteStage(("after-load rc=" + std::to_string(netRc) + "\n" + diag).c_str());
                if (netRc == 0) {
                    rc = 0;
                    loadOk = true;
                    sessionActive = true;
                    title = ToWide(t);
                    if (title.empty()) title = Utf8ToWide(surl);
                } else {
                    char err[512] = ""; WebCoreGetLastError(err, sizeof err);
                    std::string eh = MakeErrorHtml(surl, err);
                    rc = WebCoreRenderHtml(eh.c_str(), kW, kH, rgba->data());   // 渲染错误页(会话已被引擎清理)
                    loadOk = false;
                    title = L"加载失败";
                }
            }
        } catch (...) { rc = -1000; loadOk = false; title = L"渲染异常"; }

        // 取链接命中表(渲染时已提取到驱动 g_links,这里在引擎线程读出)。
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        try {
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) {
                int lx = 0, ly = 0, lw = 0, lh = 0; char lu[1200] = "";
                if (WebCoreGetLink(i, &lx, &ly, &lw, &lh, lu, sizeof lu)) {
                    Harness::PageLink pl; pl.x = lx; pl.y = ly; pl.w = lw; pl.h = lh; pl.url = Utf8ToWide(lu);
                    links->push_back(std::move(pl));
                }
            }
        } catch (...) {}

        auto titleCopy = std::make_shared<std::wstring>(title);
        bool ok = (rc == 0);   // 渲染是否成功(决定是否贴图)
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, titleCopy, ok, loadOk, sessionActive, links, mySeq]() {
                    MainPage^ s = self.Get();
                    if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // 已被更新操作/看门狗取代,丢弃此迟到回调
                    if (ok) {
                        auto wb = ref new WriteableBitmap(kW, kH);
                        BlitToBitmap(wb, *rgba, kW, kH);
                        wb->Invalidate();
                        s->RenderImage->Source = wb;
                        s->m_pageLinks = *links;   // 存当前页链接表供点击命中
                    }
                    s->m_sessionActive = sessionActive;
                    s->ScrollFab->Visibility = sessionActive
                        ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
                    // 新页面回到顶部(引擎与位图都从 scroll=0 起)
                    s->PageScroller->ChangeView(nullptr, 0.0, nullptr, true);
                    // 实时渲染:有会话则启动(让动画动、SPA 渐进挂载);无会话(主页/错误页)停。
                    s->m_lastFrameHash = 0;
                    if (sessionActive) s->StartLiveMode(); else s->StopLiveMode();
                    s->OnNavDone(ref new String(titleCopy->c_str()), ok, loadOk);
                }));
        } catch (...) {
            // RunAsync 抛了(dispatcher 断开/低内存):OnNavDone 不会跑,m_loading 靠 UI 看门狗复位。
        }
    });
}

void MainPage::OnNavDone(Platform::String^ finalTitle, bool ok, bool loadOk)
{
    if (m_loadWatchdog) m_loadWatchdog->Stop();
    m_currentTitle = finalTitle ? std::wstring(finalTitle->Data()) : L"";
    TitleText->Text = (m_currentTitle.empty() ? ref new String(L"EdgeHTML Reborn") : finalTitle);
    if (loadOk && m_currentUrl != L"about:home")   // 仅真正加载成功才记历史,失败不污染
        AddHistory(m_currentUrl, m_currentTitle);
    if (m_currentUrl != L"about:home")
        UrlBox->Text = ref new String(m_currentUrl.c_str());
    SetLoading(false);
    UpdateNavButtons();
    (void)ok;
}

void MainPage::OnLoadWatchdog(Platform::Object^, Platform::Object^)
{
    if (m_loadWatchdog) m_loadWatchdog->Stop();
    if (m_loading || m_interacting) {   // 完成回调丢失,强制复位以恢复导航/交互
        ++m_opSeq;                      // 作废这次超时操作的迟到回调,使其回 UI 时被丢弃
        m_interacting = false;
        // 会话状态不可知(加载可能半途):退到无会话,隐藏翻页按钮。下次点击若引擎仍有会话会自洽;
        // 没有则返回 -12/-14,已处理。避免停在"以为有会话"却点不动的状态。
        m_sessionActive = false;
        ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        TitleText->Text = ref new String(L"加载超时");
        SetLoading(false);
        UpdateNavButtons();
    }
}

// 网页点击:RenderImage 局部坐标 = 位图像素(Stretch=None)= 视口像素。
//  有会话(网络页):转发到引擎 WebCoreClickAt,经真实命中测试 + 默认动作(链接/表单/按钮 onclick/SPA)。
//  无会话(主页/错误页):退回链接命中表导航。
void MainPage::OnPageTapped(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    if (m_loading || m_interacting) return;
    auto pt = e->GetPosition(RenderImage);
    int px = static_cast<int>(pt.X), py = static_cast<int>(pt.Y);
    if (px < 0 || py < 0 || px >= kW || py >= kH) return;

    // 有会话:一律转发引擎真实点击。引擎命中测试是权威的——正确处理弹窗/遮罩层(z-order)、按钮、表单、
    // 以及链接(锚点默认动作=导航)。链接表感知不到模态层覆盖,故不再"链接表优先"(否则点模态关闭按钮
    // 会被误判成点中被它盖住的下层链接 → 弹窗关不掉)。ForwardClickToEngine 内含链接表兜底。
    if (m_sessionActive) {
        ForwardClickToEngine(px, py);
        return;
    }
    // 无会话(主页/错误页):链接表命中导航。
    for (auto it = m_pageLinks.rbegin(); it != m_pageLinks.rend(); ++it) {
        const PageLink& l = *it;
        if (px >= l.x && px < l.x + l.w && py >= l.y && py < l.y + l.h) {
            NavigateTo(ref new String(l.url.c_str()), true);
            return;
        }
    }
}

// 把 (px,py) 点击转发给引擎活会话。引擎派发真实鼠标事件并处理默认动作;若触发了会话内导航
// (URL 变化),回 UI 后同步地址栏/前进后退栈/历史。引擎点击失败但命中了链接表 → 退回经典导航。
void MainPage::ForwardClickToEngine(int px, int py)
{
    if (m_interacting) return;
    m_interacting = true;
    SetLoading(true);
    if (m_loadWatchdog) m_loadWatchdog->Start();   // 兜底:若引擎/回调卡死,40s 强制复位
    TitleText->Text = ref new String(L"处理中…");

    // 链接表命中(供引擎点击失败时退回经典导航)
    auto linkHit = std::make_shared<std::wstring>();
    for (auto it = m_pageLinks.rbegin(); it != m_pageLinks.rend(); ++it) {
        const PageLink& l = *it;
        if (px >= l.x && px < l.x + l.w && py >= l.y && py < l.y + l.h) { *linkHit = l.url; break; }
    }

    std::wstring prevUrl = m_currentUrl;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;

    WebEngine::instance().post([disp, self, px, py, linkHit, prevUrl, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        unsigned hashBefore = WebCoreGetFrameHash();
        try { rc = WebCoreClickAt(px, py, rgba->data()); } catch (...) { rc = -1000; }
        unsigned hashAfter = (rc == 0) ? WebCoreGetFrameHash() : hashBefore;
        bool changed = (hashAfter != hashBefore);   // 引擎点击是否改变了画面(区分模态关闭/按钮 vs 死链接)
        int editable = 0;
        try { if (rc == 0) editable = WebCoreFocusedEditable(); } catch (...) {}   // 点中的是不是输入框

        std::wstring navUrl, title;
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0) {
            char t[512] = ""; WebCoreGetTitle(t, sizeof t); title = ToWide(t);
            char u[1024] = ""; WebCoreGetUrl(u, sizeof u);
            std::wstring newUrl = ToWide(u);
            if (!newUrl.empty() && newUrl != prevUrl) navUrl = newUrl;   // 会话内发生了导航
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) {
                int lx=0,ly=0,lw=0,lh=0; char lu[1200]="";
                if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) {
                    Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu);
                    links->push_back(std::move(pl));
                }
            }
        }
        auto titleW = std::make_shared<std::wstring>(title);
        auto navW = std::make_shared<std::wstring>(navUrl);
        int rcCopy = rc;
        bool changedCopy = changed;
        int editableCopy = editable;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, titleW, navW, links, rcCopy, changedCopy, editableCopy, linkHit, mySeq]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // 已被取代/看门狗复位,丢弃迟到回调
                    s->m_interacting = false;
                    if (s->m_loadWatchdog) s->m_loadWatchdog->Stop();
                    if (rcCopy == 0) {
                        Platform::String^ title = ref new String(titleW->c_str());
                        Platform::String^ navUrl = navW->empty() ? nullptr : ref new String(navW->c_str());
                        // 引擎点击没导航、画面也没变、却命中了链接表 → 引擎可能没触发锚点默认动作,经典导航兜底。
                        // 模态关闭/按钮等会改变画面(changedCopy=true)→ 不兜底,信任引擎结果。
                        if (navW->empty() && !changedCopy && !linkHit->empty()) {
                            s->SetLoading(false);
                            s->NavigateTo(ref new String(linkHit->c_str()), true);
                        } else {
                            s->ApplyEngineFrame(rgba, title, navUrl, links);
                            s->SetLoading(false);
                            // 未导航(in-page):点中可编辑元素则唤起键盘,否则收起。导航了则收起。
                            if (navW->empty()) { if (editableCopy) s->OpenKeyboard(); else s->CloseKeyboard(); }
                            else s->CloseKeyboard();
                        }
                    } else if (!linkHit->empty()) {
                        s->SetLoading(false);   // 先清 m_loading 否则 NavigateTo 早退
                        s->NavigateTo(ref new String(linkHit->c_str()), true);   // 退回经典导航
                    } else {
                        // 会话丢失(-12 无会话 / -14 帧丢失):清状态、隐藏翻页按钮,避免后续点击空转
                        if (rcCopy == -12 || rcCopy == -14) {
                            s->m_sessionActive = false;
                            s->ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                        }
                        s->SetLoading(false);
                        s->TitleText->Text = ref new String(s->m_currentTitle.empty() ? L"EdgeHTML Reborn" : s->m_currentTitle.c_str());
                    }
                }));
        } catch (...) {}
    });
}

// 把一帧引擎渲染结果贴到位图 + 同步标题/链接表;navUrl 非空 = 会话内发生导航(同步地址栏/栈/历史)。
void MainPage::ApplyEngineFrame(const std::shared_ptr<std::vector<uint8_t>>& rgba,
                               Platform::String^ title, Platform::String^ navUrl,
                               const std::shared_ptr<std::vector<PageLink>>& links)
{
    auto wb = ref new WriteableBitmap(kW, kH);
    BlitToBitmap(wb, *rgba, kW, kH);
    wb->Invalidate();
    RenderImage->Source = wb;
    m_pageLinks = *links;
    m_lastFrameHash = 0;        // 强制下一实时帧重贴(交互改了画面)
    StartLiveMode();            // 交互后重启实时(可能触发了动画/SPA 更新)
    if (title && title->Length() > 0) {
        m_currentTitle = std::wstring(title->Data());
        TitleText->Text = title;
    }
    if (navUrl != nullptr) {
        std::wstring nu = std::wstring(navUrl->Data());
        m_currentUrl = nu;
        UrlBox->Text = navUrl;
        // 仅当与当前栈顶不同才压栈,避免会话内重定向链/同页微变产生相邻重复项(导致"后退无反应")。
        bool dup = (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() && m_navStack[m_navIndex] == nu);
        if (!dup) {
            if (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() - 1)
                m_navStack.erase(m_navStack.begin() + m_navIndex + 1, m_navStack.end());
            m_navStack.push_back(nu);
            m_navIndex = (int)m_navStack.size() - 1;
            UpdateNavButtons();
        }
        AddHistory(m_currentUrl, m_currentTitle);
        PageScroller->ChangeView(nullptr, 0.0, nullptr, true);   // 新页面回顶部
    }
}

// 引擎滚动(触发懒加载图片/查看下方内容)。dy>0 向下。滚动后位图即新视口,native 滚动复位顶。
void MainPage::EngineScroll(int dy)
{
    if (!m_sessionActive || m_loading || m_interacting) return;
    m_interacting = true;
    SetLoading(true);
    if (m_loadWatchdog) m_loadWatchdog->Start();

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;
    WebEngine::instance().post([disp, self, dy, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try { rc = WebCoreScrollBy(dy, rgba->data()); } catch (...) { rc = -1000; }
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0) {
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) {
                int lx=0,ly=0,lw=0,lh=0; char lu[1200]="";
                if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) {
                    Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu);
                    links->push_back(std::move(pl));
                }
            }
        }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, links, rcCopy, mySeq]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // 已被取代/看门狗复位,丢弃迟到回调
                    s->m_interacting = false;
                    if (s->m_loadWatchdog) s->m_loadWatchdog->Stop();
                    s->SetLoading(false);
                    if (rcCopy == 0) {
                        auto wb = ref new WriteableBitmap(kW, kH);
                        BlitToBitmap(wb, *rgba, kW, kH);
                        wb->Invalidate();
                        s->RenderImage->Source = wb;
                        s->m_pageLinks = *links;
                        s->PageScroller->ChangeView(nullptr, 0.0, nullptr, true);
                        s->m_lastFrameHash = 0;
                        s->StartLiveMode();   // 滚动后重启实时(新视口的懒加载/动画)
                    }
                }));
        } catch (...) {}
    });
}
void MainPage::OnScrollUp(Platform::Object^, RoutedEventArgs^)   { EngineScroll(-900); }
void MainPage::OnScrollDown(Platform::Object^, RoutedEventArgs^) { EngineScroll(900); }

// ---- 自由滚动:指针拖拽 / 滚轮 → 累积位移 → 合并成引擎滚动(无 spinner,跟手)----
void MainPage::FreeScrollBy(int dy)
{
    if (!m_sessionActive || dy == 0) return;
    m_scrollAccum += dy;
    if (!m_scrollBusy) PumpScroll();
}
void MainPage::PumpScroll()
{
    if (m_scrollAccum == 0 || !m_sessionActive) { m_scrollBusy = false; return; }
    int dy = m_scrollAccum; m_scrollAccum = 0;
    m_scrollBusy = true;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = m_opSeq;   // 不自增:被动滚动不作废点击/导航令牌,但被它们作废(导航后丢弃迟到滚动帧)
    WebEngine::instance().post([disp, self, dy, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try { rc = WebCoreScrollBy(dy, rgba->data()); } catch (...) { rc = -1000; }
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0) {
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) { int lx=0,ly=0,lw=0,lh=0; char lu[1200]=""; if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) { Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu); links->push_back(std::move(pl)); } }
        }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, links, rcCopy, mySeq]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) { s->m_scrollBusy = false; s->m_scrollAccum = 0; return; }   // 被导航/点击取代,丢弃迟到帧
                if (rcCopy == 0) {
                    auto wb = ref new WriteableBitmap(kW, kH); BlitToBitmap(wb, *rgba, kW, kH); wb->Invalidate(); s->RenderImage->Source = wb;
                    s->m_pageLinks = *links;
                    s->m_lastFrameHash = 0;
                }
                s->m_scrollBusy = false;
                if (s->m_scrollAccum != 0) s->PumpScroll();   // 拖拽期间又攒了位移,继续冲刷
                else s->StartLiveMode();                      // 滚动停了 → 重启实时(新视口懒加载/动画)
            }));
        } catch (...) {}
    });
}
void MainPage::OnPagePointerPressed(Platform::Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e)
{
    if (!m_sessionActive) return;
    if (m_pointerDown) { m_pointerDown = false; m_dragging = false; return; }   // 第二指按下 → 捏合缩放,不当拖拽
    double y = e->GetCurrentPoint(PageScroller)->Position.Y;
    m_pointerDown = true; m_dragging = false;
    m_dragStartY = y; m_dragLastY = y;
}
void MainPage::OnPagePointerMoved(Platform::Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e)
{
    if (!m_pointerDown || !m_sessionActive) return;
    double y = e->GetCurrentPoint(PageScroller)->Position.Y;
    double moved = y - m_dragStartY;
    if (!m_dragging) {
        if (moved < 12.0 && moved > -12.0) return;   // 未越阈值,可能是点击,先不滚
        m_dragging = true;
    }
    double zf = PageScroller->ZoomFactor; if (zf <= 0.01) zf = 1.0;
    double ddy = (m_dragLastY - y) / zf;             // 手指上移 → 内容下滚(dy>0);按缩放换算成内容像素
    m_dragLastY = y;
    int dy = (int)(ddy < 0 ? ddy - 0.5 : ddy + 0.5);
    if (dy != 0) FreeScrollBy(dy);
    e->Handled = true;                                // 认领拖拽手势
}
void MainPage::OnPagePointerReleased(Platform::Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e)
{
    if (m_dragging) e->Handled = true;                // 是拖拽:吃掉,别让它变成 Tapped(点击)
    m_pointerDown = false; m_dragging = false;        // 小位移 → 不 Handled,交给 Tapped 走点击转发
}
void MainPage::OnPageWheel(Platform::Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e)
{
    if (!m_sessionActive) return;
    int delta = e->GetCurrentPoint(PageScroller)->Properties->MouseWheelDelta;   // 上滚=+120
    if (delta != 0) { FreeScrollBy(-delta); e->Handled = true; }                 // 上滚 → 内容上移(dy<0)
}

// ---- 输入法/屏幕键盘 ----
void MainPage::OpenKeyboard()
{
    m_imeOpen = true;
    m_imeSyncing = true;
    ImeBox->Text = ref new String(L"");
    m_lastImeText.clear();
    m_imeSyncing = false;
    ImeBox->Focus(Windows::UI::Xaml::FocusState::Programmatic);   // 聚焦隐藏 TextBox → 唤起屏幕键盘
}
void MainPage::CloseKeyboard()
{
    if (!m_imeOpen) return;
    m_imeOpen = false;
    try { Windows::UI::ViewManagement::InputPane::GetForCurrentView()->TryHide(); } catch (...) {}
}
void MainPage::OnImeTextChanged(Platform::Object^, Windows::UI::Xaml::Controls::TextChangedEventArgs^)
{
    // 诊断埋点:记录本回调是否触发 + 门控状态 + ImeBox 文本(写 LocalState\imedebug.txt,真机测后拉取)。
    // 若打字后此文件为空 → OnImeTextChanged 没触发 → 隐藏 ImeBox 收不到 IME 文本(UI 层问题);
    // 若有记录但输入框没字 → 引擎层(看 SendKeyToEngine 写的 rc:kErrNoDocument=canEdit 丢焦点)。
    try {
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ofstream f(WideToUtf8(d) + "\\imedebug.txt", std::ios::app | std::ios::binary);
            if (f) { std::string s = "TC open=" + std::to_string(m_imeOpen) + " sess=" + std::to_string(m_sessionActive)
                + " sync=" + std::to_string(m_imeSyncing) + " text=[" + WideToUtf8(ImeBox->Text ? std::wstring(ImeBox->Text->Data()) : L"") + "]\n"; f.write(s.data(), s.size()); }
        }
    } catch (...) {}
    if (m_imeSyncing || !m_imeOpen || !m_sessionActive) return;
    std::wstring cur = ImeBox->Text ? std::wstring(ImeBox->Text->Data()) : L"";
    std::wstring prev = m_lastImeText;
    if (cur == prev) return;
    if (cur.size() > prev.size() && cur.compare(0, prev.size(), prev) == 0) {
        SendKeyToEngine(0, ref new String(cur.substr(prev.size()).c_str()));   // 末尾追加
    } else if (cur.size() < prev.size() && prev.compare(0, cur.size(), cur) == 0) {
        int n = static_cast<int>(prev.size() - cur.size());
        for (int i = 0; i < n; ++i) SendKeyToEngine(2, nullptr);   // 末尾退格
    } else {
        // 复杂编辑/IME 重排:简化为整体替换(退完旧的再插新的)。
        for (size_t i = 0; i < prev.size(); ++i) SendKeyToEngine(2, nullptr);
        if (!cur.empty()) SendKeyToEngine(0, ref new String(cur.c_str()));
    }
    m_lastImeText = cur;
}
void MainPage::OnImeKeyDown(Platform::Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (!m_imeOpen) return;
    if (e->Key == Windows::System::VirtualKey::Enter) {
        e->Handled = true;
        SendKeyToEngine(1, nullptr);   // 回车(可能触发表单提交导航)
        m_imeSyncing = true; ImeBox->Text = ref new String(L""); m_imeSyncing = false; m_lastImeText.clear();
    } else if (e->Key == Windows::System::VirtualKey::Back && m_lastImeText.empty()) {
        SendKeyToEngine(2, nullptr);   // 缓冲空时退格(TextChanged 不触发)
    }
}
void MainPage::SendKeyToEngine(int kind, Platform::String^ text)
{
    if (!m_sessionActive) return;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    std::string utf8 = (kind == 0 && text) ? ToUtf8(text) : std::string();
    WebEngine::instance().post([disp, self, kind, utf8]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try {
            if (kind == 0) rc = WebCoreTypeText(utf8.c_str(), rgba->data());
            else if (kind == 1) rc = WebCoreKeyAction(1, rgba->data());
            else rc = WebCoreKeyAction(0, rgba->data());
        } catch (...) { rc = -1000; }
        // 诊断:记引擎返回(rc=-6/kErrNoDocument → 打字时 canEdit 为 false=丢了可编辑焦点;rc=0 → 引擎接受了)。
        try { std::wstring dd = LocalStateDir(); if (!dd.empty()) { std::ofstream f(WideToUtf8(dd) + "\\imedebug.txt", std::ios::app | std::ios::binary); if (f) { std::string s = "  SK kind=" + std::to_string(kind) + " rc=" + std::to_string(rc) + "\n"; f.write(s.data(), s.size()); } } } catch (...) {}
        std::wstring navUrl, title;
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0 && kind == 1) {   // 回车可能导航 → 取新 url/title/链接
            char t[512] = ""; WebCoreGetTitle(t, sizeof t); title = ToWide(t);
            char u[1024] = ""; WebCoreGetUrl(u, sizeof u); navUrl = ToWide(u);
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) { int lx=0,ly=0,lw=0,lh=0; char lu[1200]=""; if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) { Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu); links->push_back(std::move(pl)); } }
        }
        auto navW = std::make_shared<std::wstring>(navUrl); auto titleW = std::make_shared<std::wstring>(title);
        int rcCopy = rc; int kindCopy = kind;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, kindCopy, navW, titleW, links]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (rcCopy != 0) return;
                if (kindCopy == 1 && !navW->empty() && std::wstring(navW->c_str()) != s->m_currentUrl) {
                    s->ApplyEngineFrame(rgba, ref new String(titleW->c_str()), ref new String(navW->c_str()), links);   // 回车导航:同步地址栏/历史
                    s->CloseKeyboard();
                } else {
                    auto wb = ref new WriteableBitmap(kW, kH); BlitToBitmap(wb, *rgba, kW, kH); wb->Invalidate(); s->RenderImage->Source = wb;
                    s->m_lastFrameHash = 0;
                }
            }));
        } catch (...) {}
    });
}

// ---- 实时渲染循环:低帧率驱动引擎推进动画/SPA 渐进挂载 ----
void MainPage::StartLiveMode()
{
    if (!m_sessionActive || !m_appForeground) return;
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    m_liveBusy = false;        // 重置(若上次 RunAsync 抛/后台早退卡住,这里恢复)
    m_liveBusyAge = 0;
    m_liveStaticTicks = 0;
    m_liveTotalTicks = 0;
    if (!m_liveTimer) {
        m_liveTimer = ref new Windows::UI::Xaml::DispatcherTimer();
        m_liveTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnLiveTick);
    }
    // 每次启动都恢复快帧率(撤销之前永久动画的降速)。
    Windows::Foundation::TimeSpan ts; ts.Duration = 2000000LL;   // 200ms(100ns 单位)≈ 5fps
    m_liveTimer->Interval = ts;
    m_liveTimer->Start();
}
void MainPage::StopLiveMode()
{
    if (m_liveTimer) m_liveTimer->Stop();
}
void MainPage::OnLiveTick(Platform::Object^, Platform::Object^)
{
    if (!m_sessionActive || !m_appForeground || m_loading || m_interacting) return;
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible) { StopLiveMode(); return; }
    if (m_liveBusy) {                      // 上一帧引擎任务还没回
        if (++m_liveBusyAge < 5) return;   // 正常等(~1s 内)
        m_liveBusy = false;                // 卡过久 → RunAsync 很可能丢了,自愈不死循环
    }
    m_liveBusyAge = 0;
    m_liveBusy = true;
    unsigned long long mySeq = m_opSeq;    // 只读不自增:实时帧是被动的,绝不能作废正在进行的真操作
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    WebEngine::instance().post([disp, self, mySeq]() {
        MainPage^ s0 = self.Get();
        if (!s0 || !s0->m_appForeground) return;   // 已切后台:别在 PLM 冻结风险下跑 JS+绘制(m_liveBusy 由恢复时 StartLiveMode 清)
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999; unsigned hash = 0;
        try { rc = WebCoreLiveTick(rgba->data()); if (rc == 0) hash = WebCoreGetFrameHash(); } catch (...) { rc = -1000; }
        int rcCopy = rc; unsigned hashCopy = hash;
        try {
            disp->RunAsync(CoreDispatcherPriority::Low,
                ref new DispatchedHandler([self, rgba, rcCopy, hashCopy, mySeq]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    s->m_liveBusy = false;
                    if (s->m_opSeq != mySeq) return;   // 期间发生了导航/滚动/点击/超时 → 丢弃这帧旧像素(防闪回旧页)
                    if (!s->m_sessionActive || s->m_loading || s->m_interacting || !s->m_appForeground) return;
                    if (rcCopy != 0) {
                        if (rcCopy == -12 || rcCopy == -14) {   // 会话没了:停帧 + 收起按钮
                            s->m_sessionActive = false;
                            s->ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                            s->StopLiveMode();
                        }
                        return;
                    }
                    if (hashCopy == s->m_lastFrameHash) {        // 画面没变:连续静止则停帧省电
                        if (++s->m_liveStaticTicks >= 12) s->StopLiveMode();   // ~2.4s 静止 → 停
                        return;
                    }
                    s->m_liveStaticTicks = 0;
                    s->m_lastFrameHash = hashCopy;
                    auto wb = ref new WriteableBitmap(kW, kH);
                    BlitToBitmap(wb, *rgba, kW, kH);
                    wb->Invalidate();
                    s->RenderImage->Source = wb;
                    // 永久动画防失控:连续动画超 ~150 帧(30s)无交互 → 降到 ~1fps(不硬停,免得动画卡死);
                    // 任何交互/导航/滚动都经 StartLiveMode 重置计数并恢复 200ms。
                    if (++s->m_liveTotalTicks == 150 && s->m_liveTimer) {
                        Windows::Foundation::TimeSpan slow; slow.Duration = 10000000LL;   // 1s ≈ 1fps
                        s->m_liveTimer->Interval = slow;
                    }
                }));
        } catch (...) {}
    });
}

// ---- 工具栏事件 ----
void MainPage::OnGo(Platform::Object^, RoutedEventArgs^) { NavigateTo(NormalizeUrl(UrlBox->Text), true); }
void MainPage::OnUrlKeyDown(Platform::Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (e->Key == Windows::System::VirtualKey::Enter) NavigateTo(NormalizeUrl(UrlBox->Text), true);
}
void MainPage::OnHome(Platform::Object^, RoutedEventArgs^) { NavigateTo(ref new String(L"about:home"), true); }
void MainPage::OnBack(Platform::Object^, RoutedEventArgs^)
{
    if (m_loading || m_navIndex <= 0) return;
    --m_navIndex;
    NavigateTo(ref new String(m_navStack[m_navIndex].c_str()), false);
}
void MainPage::OnForward(Platform::Object^, RoutedEventArgs^)
{
    if (m_loading || m_navIndex >= (int)m_navStack.size() - 1) return;
    ++m_navIndex;
    NavigateTo(ref new String(m_navStack[m_navIndex].c_str()), false);
}
void MainPage::OnMenu(Platform::Object^, RoutedEventArgs^) { ShowDrawer(m_tab); }

// UA 切换:手机/桌面。切引擎 UA 后重载当前页生效。
void MainPage::OnToggleUA(Platform::Object^, RoutedEventArgs^)
{
    m_uaMobile = !m_uaMobile;
    UaBtn->Content = ref new String(m_uaMobile ? L"\U0001F4F1 手机UA" : L"\U0001F5A5 桌面UA");
    int mobile = m_uaMobile ? 1 : 0;
    WebEngine::instance().post([mobile]() { try { WebCoreSetUserAgentMobile(mobile); } catch (...) {} });
    HideDrawer();
    if (!m_currentUrl.empty())
        NavigateTo(ref new String(m_currentUrl.c_str()), false);   // 重载使新 UA 生效
}

// ---- 抽屉 ----
void MainPage::OnDrawerClose(Platform::Object^, RoutedEventArgs^) { HideDrawer(); }
void MainPage::OnTabFav(Platform::Object^, RoutedEventArgs^)  { ShowDrawer(DrawerTab::Favorites); }
void MainPage::OnTabHist(Platform::Object^, RoutedEventArgs^) { ShowDrawer(DrawerTab::History); }
void MainPage::OnTabDl(Platform::Object^, RoutedEventArgs^)   { ShowDrawer(DrawerTab::Downloads); }

void MainPage::OnPrimaryAction(Platform::Object^, RoutedEventArgs^)
{
    if (m_tab == DrawerTab::Favorites) {
        // 收藏当前页(或取消收藏)
        if (m_currentUrl.empty() || m_currentUrl == L"about:home") return;
        if (IsBookmarked(m_currentUrl)) {
            m_bookmarks.erase(std::remove_if(m_bookmarks.begin(), m_bookmarks.end(),
                [&](const Entry& e) { return e.url == m_currentUrl; }), m_bookmarks.end());
        } else {
            Entry e; e.url = m_currentUrl; e.title = m_currentTitle.empty() ? m_currentUrl : m_currentTitle;
            m_bookmarks.insert(m_bookmarks.begin(), e);
        }
        SaveBookmarks();
        RebuildDrawerList();
    } else if (m_tab == DrawerTab::History) {
        m_historyList.clear(); SaveHistory(); RebuildDrawerList();
    } else {
        // 下载:下载当前页地址
        if (!m_currentUrl.empty() && m_currentUrl != L"about:home")
            StartDownload(ref new String(m_currentUrl.c_str()));
    }
}

void MainPage::ShowDrawer(DrawerTab tab)
{
    m_tab = tab;
    Drawer->Visibility = Windows::UI::Xaml::Visibility::Visible;
    // 主操作按钮文案随标签变化
    if (tab == DrawerTab::Favorites)
        ActionBtn->Content = (!m_currentUrl.empty() && IsBookmarked(m_currentUrl)) ? ref new String(L"★ 取消收藏") : ref new String(L"★ 收藏此页");
    else if (tab == DrawerTab::History)
        ActionBtn->Content = ref new String(L"\U0001F5D1 清空");
    else
        ActionBtn->Content = ref new String(L"↓ 下载此页");
    RebuildDrawerList();
    StopLiveMode();   // 抽屉盖住网页,暂停实时渲染省电
}
void MainPage::HideDrawer()
{
    Drawer->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    StartLiveMode();   // 抽屉关闭,恢复实时渲染
}

// 构建抽屉列表项(标题 + URL,可点导航;收藏/下载可删)
static Border^ MakeRow(Platform::String^ title, Platform::String^ sub, Color titleColor)
{
    auto sp = ref new StackPanel();
    sp->Margin = Thickness(14, 10, 14, 10);
    auto t = ref new TextBlock();
    t->Text = title; t->FontSize = 20; t->Foreground = ref new SolidColorBrush(titleColor);
    t->TextTrimming = TextTrimming::CharacterEllipsis; t->MaxLines = 1;
    auto u = ref new TextBlock();
    u->Text = sub; u->FontSize = 15; u->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x80, 0x86, 0x8b));
    u->TextTrimming = TextTrimming::CharacterEllipsis; u->MaxLines = 1; u->Margin = Thickness(0, 2, 0, 0);
    sp->Children->Append(t); sp->Children->Append(u);
    auto b = ref new Border();
    b->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x2B, 0x2D, 0x31));
    b->CornerRadius = CornerRadius(10);
    b->Margin = Thickness(0, 0, 0, 8);
    b->Child = sp;
    return b;
}

void MainPage::RebuildDrawerList()
{
    DrawerList->Children->Clear();
    const std::vector<Entry>* list = nullptr;
    if (m_tab == DrawerTab::Favorites) list = &m_bookmarks;
    else if (m_tab == DrawerTab::History) list = &m_historyList;
    else list = &m_downloads;

    if (list->empty()) {
        auto empty = ref new TextBlock();
        empty->Text = (m_tab == DrawerTab::Favorites) ? L"暂无收藏" : (m_tab == DrawerTab::History ? L"暂无历史记录" : L"暂无下载");
        empty->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x80, 0x86, 0x8b));
        empty->FontSize = 18; empty->Margin = Thickness(14, 20, 0, 0);
        DrawerList->Children->Append(empty);
        return;
    }

    Platform::Agile<MainPage^> self(this);
    bool isDownloads = (m_tab == DrawerTab::Downloads);
    bool isFav = (m_tab == DrawerTab::Favorites);
    for (size_t i = 0; i < list->size(); ++i) {
        const Entry& e = (*list)[i];
        Platform::String^ titleS = ref new String(e.title.empty() ? e.url.c_str() : e.title.c_str());
        Platform::String^ subS = ref new String((isDownloads ? (e.extra + L"  ·  " + e.url) : e.url).c_str());
        auto row = MakeRow(titleS, subS, ColorHelper::FromArgb(255, 0xF0, 0xF0, 0xF0));

        if (isDownloads) {
            DrawerList->Children->Append(row);
            continue;
        }
        // 点击行 → 导航
        std::wstring u = e.url;
        auto btn = ref new Button();
        btn->Background = ref new SolidColorBrush(Colors::Transparent);
        btn->BorderThickness = Thickness(0);
        btn->Padding = Thickness(0);
        btn->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        btn->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        btn->Content = row;
        btn->Click += ref new RoutedEventHandler([self, u](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->HideDrawer();
            s->NavigateTo(ref new String(u.c_str()), true);
        });
        DrawerList->Children->Append(btn);

        // 删除按钮(收藏/历史)
        if (isFav) {
            auto del = ref new Button();
            del->Content = L"✕ 删除收藏";
            del->FontSize = 15;
            del->Background = ref new SolidColorBrush(Colors::Transparent);
            del->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0xD9, 0x30, 0x25));
            del->BorderThickness = Thickness(0);
            del->Margin = Thickness(8, -6, 0, 8);
            del->Click += ref new RoutedEventHandler([self, u](Platform::Object^, RoutedEventArgs^) {
                MainPage^ s = self.Get(); if (!s) return;
                s->m_bookmarks.erase(std::remove_if(s->m_bookmarks.begin(), s->m_bookmarks.end(),
                    [&](const Entry& en) { return en.url == u; }), s->m_bookmarks.end());
                s->SaveBookmarks(); s->RebuildDrawerList();
            });
            DrawerList->Children->Append(del);
        }
    }
}

// ---- 下载 ----
void MainPage::StartDownload(Platform::String^ url)
{
    std::wstring wurl = url->Data();
    // 文件名:URL 最后一段(去 query),空则 index.html
    std::wstring fn = wurl;
    size_t q = fn.find(L'?'); if (q != std::wstring::npos) fn = fn.substr(0, q);
    size_t sl = fn.find_last_of(L'/');
    fn = (sl == std::wstring::npos) ? fn : fn.substr(sl + 1);
    if (fn.empty() || fn.find(L'.') == std::wstring::npos) fn = L"index.html";

    std::wstring dlDir = LocalStateDir() + L"\\Downloads";
    CreateDirectoryW(dlDir.c_str(), nullptr);
    // 文件名去重:同名文件已存在则追加 (1)(2)…,避免覆盖之前下载的文件。
    std::wstring outPath = dlDir + L"\\" + fn;
    if (GetFileAttributesW(outPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring stem = fn, ext;
        size_t dot = fn.find_last_of(L'.');
        if (dot != std::wstring::npos) { stem = fn.substr(0, dot); ext = fn.substr(dot); }
        for (int i = 1; i < 1000; ++i) {
            std::wstring cand = stem + L"(" + std::to_wstring(i) + L")" + ext;
            std::wstring candPath = dlDir + L"\\" + cand;
            if (GetFileAttributesW(candPath.c_str()) == INVALID_FILE_ATTRIBUTES) { fn = cand; outPath = candPath; break; }
        }
    }

    TitleText->Text = ref new String((L"下载中  " + fn).c_str());
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    std::string u8url = ToUtf8(url);
    std::string u8out = WideToUtf8(outPath);
    std::wstring fnCopy = fn;
    std::wstring urlCopy = wurl;

    // 下载放专用线程,不占用单引擎线程(否则慢下载会阻塞导航最多 120s)。WebCoreDownload 用独立
    // curl_easy 句柄,不设 CURLOPT_SHARE、不碰 WebKit 主线程调度,与渲染并发安全;CA(g_caBytes)
    // 已由引擎线程的 SetupRuntimeEnv 备好(用户触发下载时主页早已加载)。
    std::thread([disp, self, u8url, u8out, fnCopy, urlCopy]() {
        int code = WebCoreDownload(u8url.c_str(), u8out.c_str());
        long long sz = 0;
        try { std::ifstream f(u8out, std::ios::binary | std::ios::ate); if (f) sz = (long long)f.tellg(); } catch (...) {}
        std::wstring status = (code >= 200 && code < 400)
            ? (L"已完成  " + std::to_wstring(sz / 1024) + L" KB")
            : (L"失败(" + std::to_wstring(code) + L")");
        auto st = std::make_shared<std::wstring>(status);
        auto fnC = std::make_shared<std::wstring>(fnCopy);
        auto urlC = std::make_shared<std::wstring>(urlCopy);
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, st, fnC, urlC]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    Entry e; e.url = *urlC; e.title = *fnC; e.extra = *st;
                    s->m_downloads.insert(s->m_downloads.begin(), e);
                    if (s->m_downloads.size() > 100) s->m_downloads.resize(100);
                    s->SaveDownloads();
                    s->TitleText->Text = ref new String((*fnC + L"  " + *st).c_str());
                    if (s->Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible && s->m_tab == DrawerTab::Downloads)
                        s->RebuildDrawerList();
                }));
        } catch (...) {}
    }).detach();
}
