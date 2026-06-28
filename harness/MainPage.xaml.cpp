#include "pch.h"
#include "MainPage.xaml.h"
#include "MainPage.g.hpp"
#include "WebCoreDriver.h"
#include "JitProbe.h"
#include "GpuProbe.h"

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
#include <cwctype>
#include <cstdlib>
#include <ppltasks.h>
#include <collection.h>

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

// 设置:搜索引擎前缀 + 主页(默认值;LoadSettings 从 settings.ini 覆盖)。free 函数 NormalizeUrl/构造用,故放全局。
static std::wstring g_searchPrefix = L"https://cn.bing.com/search?q=";
static std::wstring g_homeUrl = L"about:home";
static std::wstring g_lang = L"zh";   // 界面语言:zh(默认)/ en;首启 OOBE 选定,存 settings.ini
// 代码里动态设置的中/英文案(按当前语言返回)。静态 XAML 串由 TranslateNode 树遍历翻译;
// 这个给"运行期才赋值、会盖掉翻译"的标签/toast 用(收藏状态、UA 状态等)。
static Platform::String^ L8(const wchar_t* zh, const wchar_t* en) {
    return ref new Platform::String(g_lang == L"en" ? en : zh);
}
static std::wstring SearchPrefixFor(int idx)
{
    switch (idx) {
        case 1: return L"https://www.google.com/search?q=";
        case 2: return L"https://duckduckgo.com/?q=";
        case 3: return L"https://www.baidu.com/s?wd=";
        default: return L"https://cn.bing.com/search?q=";
    }
}

static std::string HtmlEscape(const std::string& s)
{
    std::string out; out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}
static std::string HostOfU8(const std::string& u)
{
    size_t p = u.find("://");
    size_t s = (p == std::string::npos) ? 0 : p + 3;
    size_t e = u.find('/', s);
    return u.substr(s, (e == std::string::npos) ? std::string::npos : e - s);
}

// 动态新标签页:书签优先、历史补足的速拨磁贴(最多 8)。磁贴=<a>,经渲染时链接提取→点击导航。
static std::string BuildHomeHtml(const std::vector<Harness::Entry>& bookmarks, const std::vector<Harness::Entry>& history)
{
    std::vector<Harness::Entry> tiles;
    std::vector<std::wstring> seen;
    auto add = [&](const std::vector<Harness::Entry>& src) {
        for (const auto& e : src) {
            if (tiles.size() >= 8) break;
            if (e.url.empty() || e.url == L"about:home") continue;
            if (std::find(seen.begin(), seen.end(), e.url) != seen.end()) continue;
            seen.push_back(e.url);
            tiles.push_back(e);
        }
    };
    add(bookmarks);
    add(history);

    std::string h;
    h += "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>";
    h += "*{box-sizing:border-box}body{margin:0;background:#f5f6f8;font-family:sans-serif;color:#202124}";
    h += ".hero{background:linear-gradient(135deg,#00aa77,#0088cc);color:#fff;padding:46px 26px 38px}";
    h += ".hero h1{margin:0;font-size:46px;letter-spacing:-1px}.hero p{margin:10px 0 0;font-size:20px;opacity:.92}";
    h += ".wrap{padding:24px}.sec{font-size:17px;color:#5f6368;margin:0 0 14px}";
    h += ".grid{display:grid;grid-template-columns:repeat(2,1fr);gap:14px}";
    h += "a.tile{display:block;text-decoration:none;background:#fff;border-radius:16px;padding:18px 18px 20px;box-shadow:0 2px 10px rgba(0,0,0,.08);color:#202124}";
    h += ".tile .t{font-size:20px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    h += ".tile .u{font-size:15px;color:#80868b;margin-top:7px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    h += "</style></head><body>";
    h += "<div class='hero'><h1>EdgeHTML Reborn</h1><p>\xE7\x8E\xB0\xE4\xBB\xA3\xE6\xB5\x8F\xE8\xA7\x88\xE5\x99\xA8\xE5\xBC\x95\xE6\x93\x8E &middot; Windows 10 Mobile &middot; ARM32</p></div>";
    h += "<div class='wrap'>";
    if (tiles.empty()) {
        h += "<p class='sec'>\xE5\x9C\xA8\xE4\xB8\x8A\xE6\x96\xB9\xE5\x9C\xB0\xE5\x9D\x80\xE6\xA0\x8F\xE8\xBE\x93\xE5\x85\xA5\xE7\xBD\x91\xE5\x9D\x80\xE8\xAE\xBF\xE9\x97\xAE\xE7\xBD\x91\xE9\xA1\xB5\xE3\x80\x82</p><div class='grid'>";
        const char* defs[][2] = { {"https://example.com","example.com"}, {"https://github.com","github.com"}, {"https://cn.bing.com","bing.com"}, {"https://en.wikipedia.org","wikipedia.org"} };
        for (auto& d : defs) { h += "<a class='tile' href='"; h += d[0]; h += "'><div class='t'>"; h += d[1]; h += "</div><div class='u'>"; h += d[0]; h += "</div></a>"; }
        h += "</div>";
    } else {
        h += "<p class='sec'>\xE5\xB8\xB8\xE7\x94\xA8\xE7\xAB\x99\xE7\x82\xB9</p><div class='grid'>";
        for (const auto& e : tiles) {
            std::string href = HtmlEscape(WideToUtf8(e.url));
            std::string title = HtmlEscape(WideToUtf8(e.title.empty() ? e.url : e.title));
            std::string host = HtmlEscape(HostOfU8(WideToUtf8(e.url)));
            h += "<a class='tile' href='" + href + "'><div class='t'>" + title + "</div><div class='u'>" + host + "</div></a>";
        }
        h += "</div>";
    }
    h += "</div></body></html>";
    return h;
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
        return ref new String((g_searchPrefix + q).c_str());
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

// GPU 直呈现模式:引擎已 swapBuffers 到可见 GpuPanel,无需把 rgba blit 进 WriteableBitmap(RenderImage 已隐藏)。
//   置位后 BlitToBitmap 直接返回,省掉每帧 3MB 的 RGBA→BGRA 拷贝(冲 60fps)。引擎线程与 UI 线程都可能读,用 atomic。
static std::atomic<bool> g_directPresent { false };

// ===== 渲染缓冲 → WriteableBitmap(RGBA→BGRA)=====
static void BlitToBitmap(WriteableBitmap^ wb, const std::vector<uint8_t>& rgba, int W, int H)
{
    if (g_directPresent.load()) return;   // 直呈现:跳过软件 blit(GpuPanel 已由引擎呈现)
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

// 把 RGBA(上→下)写成 32 位 BMP(BGRA,自下而上)——供自动诊断把 GPU readback 的实际帧落盘,
// 经 WDP 拉回当"截图"看(无 UI、无 PNG 编码器依赖;ARM 上头部用 memcpy 避免非对齐写)。
static void WriteBmp32(const std::string& path, const uint8_t* rgba, int w, int h)
{
    const uint32_t dataSize = (uint32_t)w * h * 4;
    uint8_t fh[54] = {0};
    fh[0] = 'B'; fh[1] = 'M';
    auto put32 = [&](int off, uint32_t v) { memcpy(fh + off, &v, 4); };
    auto put16 = [&](int off, uint16_t v) { memcpy(fh + off, &v, 2); };
    put32(2, 54 + dataSize); put32(10, 54);
    put32(14, 40); put32(18, (uint32_t)w); put32(22, (uint32_t)h);   // 正高=自下而上
    put16(26, 1); put16(28, 32); put32(30, 0); put32(34, dataSize);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write((char*)fh, 54);
    std::vector<uint8_t> row((size_t)w * 4);
    for (int fy = 0; fy < h; ++fy) {
        const uint8_t* src = rgba + (size_t)(h - 1 - fy) * w * 4;
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2]; // B
            row[x * 4 + 1] = src[x * 4 + 1]; // G
            row[x * 4 + 2] = src[x * 4 + 0]; // R
            row[x * 4 + 3] = src[x * 4 + 3]; // A
        }
        f.write((char*)row.data(), (std::streamsize)w * 4);
    }
}

static const int kW = 720, kH = 1080;

// ============================================================================
MainPage::MainPage()
{
    InitializeComponent();

    // 分享:注册一次 DataRequested(原生分享契约,App Container/1607 起可用)。分享当前页 URL+标题。
    try {
        auto dtm = Windows::ApplicationModel::DataTransfer::DataTransferManager::GetForCurrentView();
        dtm->DataRequested += ref new Windows::Foundation::TypedEventHandler<
            Windows::ApplicationModel::DataTransfer::DataTransferManager^,
            Windows::ApplicationModel::DataTransfer::DataRequestedEventArgs^>(
            [this](Windows::ApplicationModel::DataTransfer::DataTransferManager^,
                   Windows::ApplicationModel::DataTransfer::DataRequestedEventArgs^ e) {
                if (m_currentUrl.empty() || m_currentUrl == L"about:home") {
                    e->Request->FailWithDisplayText(ref new Platform::String(L"无可分享内容"));
                    return;
                }
                auto req = e->Request;
                req->Data->Properties->Title = ref new Platform::String(
                    m_currentTitle.empty() ? m_currentUrl.c_str() : m_currentTitle.c_str());
                req->Data->Properties->Description = ref new Platform::String(m_currentUrl.c_str());
                try {
                    req->Data->SetWebLink(ref new Windows::Foundation::Uri(ref new Platform::String(m_currentUrl.c_str())));
                } catch (...) {
                    req->Data->SetText(ref new Platform::String(m_currentUrl.c_str()));
                }
            });
    } catch (...) {}

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
    LoadSettings();   // 搜索引擎/主页/默认UA/缩放/标签模式(在首次导航前应用)
    // OOBE:从没存过 lang(全新安装)→ 弹首启选语言浮层;否则按已选语言应用(英文则翻译整个界面)。
    if (!m_langSet) { if (OobePanel) OobePanel->Visibility = Windows::UI::Xaml::Visibility::Visible; }
    else { ApplyLanguage(); }
    // 初始化标签集合:活动标签的实时状态用全局成员表示,此处占位 1 个(首次导航填充其全局状态)。
    { Tab t0; t0.currentUrl = g_homeUrl; m_tabs.push_back(t0); m_activeTab = 0; }
    UpdateTabCount();
    // GPU 崩溃环路保护:上次自动开 GPU 没干净返回(标记残留)→ 这次别再默认开,并持久化关掉(避免每次启动即崩)。
    {
        std::wstring d = LocalStateDir();
        if (!d.empty() && GetFileAttributesW((d + L"\\gpu-crash.flag").c_str()) != INVALID_FILE_ATTRIBUTES) {
            m_gpuDefault = false;
            try { DeleteFileW((d + L"\\gpu-crash.flag").c_str()); } catch (...) {}
            SaveSettings();
        }
    }
    // 前后台切换:后台暂停实时渲染(省电、避免后台跑引擎被 PLM 冻结时堆积)。
    Window::Current->VisibilityChanged += ref new Windows::UI::Xaml::WindowVisibilityChangedEventHandler(
        [this](Platform::Object^, Windows::UI::Core::VisibilityChangedEventArgs^ e) {
            m_appForeground = e->Visible;
            if (e->Visible) StartLiveMode(); else StopLiveMode();
        });
    // 实体返回键(Win10M 硬件 Back):接管系统返回事件 → 先关浮层/再浏览器后退/否则交系统。
    try {
        Windows::UI::Core::SystemNavigationManager::GetForCurrentView()->BackRequested +=
            ref new Windows::Foundation::EventHandler<Windows::UI::Core::BackRequestedEventArgs^>(this, &MainPage::OnHardwareBack);
    } catch (...) {}
    // 软键盘遮挡:底栏在屏幕底部,键盘弹出会盖住地址栏。仅当地址栏聚焦时把整页上移键盘高度
    //   (地址胶囊+建议浮到键盘上方);网页表单输入(ImeBox)不上移——引擎自管把聚焦框滚进视口。
    try {
        auto ip = Windows::UI::ViewManagement::InputPane::GetForCurrentView();
        ip->Showing += ref new Windows::Foundation::TypedEventHandler<
            Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>(
            [this](Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e) {
                if (m_urlFocused && RootShift) {
                    RootShift->Y = -e->OccludedRect.Height;
                    e->EnsuredFocusedElementInView = true;   // 已自行让位,系统勿再额外滚动
                }
            });
        ip->Hiding += ref new Windows::Foundation::TypedEventHandler<
            Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>(
            [this](Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e) {
                if (RootShift && RootShift->Y != 0) {   // 仅当我们上移过才复位+认领(设置页文本框靠系统自身滚动恢复,别干扰)
                    RootShift->Y = 0;
                    e->EnsuredFocusedElementInView = true;
                }
            });
    } catch (...) {}
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
    // 自动诊断钩子:若 LocalState\autodiag.txt 存在(每行一个 URL,# 开头忽略),启动后在引擎线程
    //   自动 GpuInit(离屏)+ 逐个 GPU 合成加载 + dump 各页 diag/层树到 autodump.txt,供 WDP 全自动抓取
    //   (免 UI 点按 GPU 两下)。用于定位"某些页 GPU 合成全白"——对比能渲染的页与全白页的层树差异。
    std::vector<std::string> diagUrls;
    try {
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ifstream f(WideToUtf8(d) + "\\autodiag.txt", std::ios::binary);
            std::string line;
            while (std::getline(f, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
                    line.pop_back();
                if (!line.empty() && line[0] != '#')
                    diagUrls.push_back(line);
            }
        }
    } catch (...) {}
    // (诊断用)autodiag.txt 存在时才跑;正常浏览不受影响。需要再抓时临时放 autodiag.txt(每行一个 URL,DESKTOP: 前缀=桌面 UA)。
    if (!diagUrls.empty()) {
        CoreDispatcher^ disp = this->Dispatcher;
        Platform::Agile<MainPage^> self(this);
        WebEngine::instance().post([disp, self, diagUrls]() {
            std::string dump;
            int gi = -999;
            try { gi = WebCoreGpuInit(nullptr, kW, kH); } catch (...) { gi = -1000; }
            dump += "WebCoreGpuInit(offscreen) rc=" + std::to_string(gi) + "\n\n";
            auto rgba = std::vector<uint8_t>((size_t)kW * kH * 4, 0);
            std::wstring dd = LocalStateDir();
            int idx = 0;
            for (const auto& rawUrl : diagUrls) {
                std::string url = rawUrl;
                bool desktop = false;
                if (url.rfind("DESKTOP:", 0) == 0) { desktop = true; url = url.substr(8); }
                try { WebCoreSetUserAgentMobile(desktop ? 0 : 1); } catch (...) {}
                dump += "########## URL: " + url + (desktop ? " [desktop UA]" : " [mobile UA]") + " ##########\n";
                int lrc = -999;
                try { lrc = WebCoreSessionLoad(url.c_str(), kW, kH, rgba.data()); } catch (...) { lrc = -1000; }
                dump += "SessionLoad rc=" + std::to_string(lrc) + "\n";
                std::vector<char> dg(4096, 0);
                try { WebCoreGetDiag(dg.data(), (int)dg.size()); } catch (...) {}
                dump += "diag: " + std::string(dg.data()) + "\n";
                std::vector<char> li(65536, 0);
                try { WebCoreGpuLayerInfo(li.data(), (int)li.size()); } catch (...) {}
                dump += std::string(li.data());
                dump += "\n\n";
                // 落盘这页 GPU readback 的实际帧(BMP),供 WDP 拉回当截图看
                try { if (!dd.empty()) WriteBmp32(WideToUtf8(dd) + "\\shot_" + std::to_string(idx) + ".bmp", rgba.data(), kW, kH); } catch (...) {}
                ++idx;
            }
            try {
                std::wstring d2 = LocalStateDir();
                if (!d2.empty()) {
                    std::ofstream f(WideToUtf8(d2) + "\\autodump.txt", std::ios::binary | std::ios::trunc);
                    if (f) f.write(dump.data(), dump.size());
                }
            } catch (...) {}
            try {
                disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    s->TitleText->Text = ref new Platform::String(L"AUTODIAG DONE");
                }));
            } catch (...) {}
        });
    } else if (!testUrl.empty())
        NavigateTo(ref new String(testUrl.c_str()), true);
    else
        NavigateTo(ref new String(g_homeUrl.c_str()), true);   // 主页(设置可改)
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
    UpdateUrlActionGlyph();   // 加载态切到 ✕ 停止 / 结束回 → 或 ⟳
}

void MainPage::NavigateTo(Platform::String^ url, bool pushHistory)
{
    if (m_loading) return;

    std::wstring wurl = url ? std::wstring(url->Data()) : L"about:home";
    const bool isHome = wurl.empty() || wurl == L"about:home";
    m_currentUrl = isHome ? L"about:home" : wurl;

    // M4:导航=新页面,引擎 pageScaleFactor 复位 1.0 → harness 缩放状态/显示变换同步复位(否则下次捏合基准错)。
    m_pinching = false; m_liveScale = 1.0f; m_pageScale = 1.0f;
    if (GpuPanel) GpuPanel->RenderTransform = nullptr;
    if (RenderImage) RenderImage->RenderTransform = nullptr;

    if (pushHistory) {
        if (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() - 1)
            m_navStack.erase(m_navStack.begin() + m_navIndex + 1, m_navStack.end());
        m_navStack.push_back(wurl);
        m_navIndex = (int)m_navStack.size() - 1;
    }
    UpdateNavButtons();
    UpdateLockIcon();
    HideSuggestions();
    m_urlSyncing = true;
    UrlBox->Text = isHome ? ref new String(L"") : url;
    m_urlSyncing = false;
    TitleText->Text = isHome ? L8(L"主页", L"Home") : ref new String(((g_lang == L"en" ? L"Loading  " : L"加载中  ") + wurl).c_str());
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
    // 主页:用当前书签/历史动态生成新标签页(速拨磁贴=<a>,渲染时提取进链接表→点击导航)。
    std::string homeHtml = isHome ? BuildHomeHtml(m_bookmarks, m_historyList) : std::string();

    WebEngine::instance().post([disp, self, surl, isHome, homeHtml, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        bool loadOk = false;   // 网络加载是否真成功(区别于错误页渲染成功),决定是否进历史
        bool sessionActive = false;   // 是否建立了引擎常驻会话(决定点击转发/翻页按钮)
        std::wstring title;
        try {
            if (isHome) {
                WebCoreCloseSession();   // 离开网络页:销毁会话,释放 Page + 取消在途加载
                rc = WebCoreRenderHtml(homeHtml.c_str(), kW, kH, rgba->data());
                loadOk = (rc == 0);
                title = L"主页";
            } else {
                WriteStage(("before-load " + surl).c_str());
                int netRc = WebCoreSessionLoad(surl.c_str(), kW, kH, rgba->data());   // 常驻会话加载
                char t[512] = ""; WebCoreGetTitle(t, sizeof t);
                char diag[4096] = ""; WebCoreGetDiag(diag, sizeof diag);
                char err[512] = ""; WebCoreGetLastError(err, sizeof err);   // curl 错误码+描述(失败时)
                int comp = 0; try { comp = WebCoreEnableCompositing(); } catch (...) {}   // M1 验证:合成是否在跑(根图层已附)
                // 失败原因也写进 stage.txt(原来只进错误页,拉不到)→ 远程诊断"加载失败"必看。
                WriteStage(("after-load url=" + surl + " rc=" + std::to_string(netRc)
                            + " compositing=" + std::to_string(comp)
                            + "\nERR: " + err + "\ndiag: " + diag).c_str());
                if (netRc == 0) {
                    rc = 0;
                    loadOk = true;
                    sessionActive = true;
                    title = ToWide(t);
                    if (title.empty()) title = Utf8ToWide(surl);
                } else {
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
    if (m_currentUrl != L"about:home") {
        m_urlSyncing = true;
        UrlBox->Text = ref new String(m_currentUrl.c_str());
        m_urlSyncing = false;
    }
    UpdateLockIcon();
    SetLoading(false);
    UpdateNavButtons();
    // 启动静默自检更新:首个网络页加载成功后跑一次(此时 CA blob 已注入,WebCoreDownload 才能过 TLS);
    //   有新版才提示,无更新/失败静默。manual 检查在设置里按钮。
    if (!m_updateAutoChecked && loadOk && m_currentUrl != L"about:home") {
        m_updateAutoChecked = true;
        CheckForUpdate(false);
    }
    // 默认 GPU:首个网络页加载完、开关开 → 自动开 GPU(EnableGpu 成功会重载本页,届时再走一遍 OnNavDone)。
    if (m_gpuDefault && !m_gpuOn && !m_gpuAutoTried && m_sessionActive && m_currentUrl != L"about:home") {
        m_gpuAutoTried = true;
        EnableGpu();
        return;   // 缩放在重载后的 OnNavDone 应用,避免双重栅格
    }
    // 默认缩放:有会话且默认非 100% 时,按新尺度重栅格(复用捏合提交路径)。
    if (m_sessionActive && m_defaultZoom != 100)
        PinchCommit(m_defaultZoom / 100.0f, kW / 2, kH / 2);
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
        TitleText->Text = L8(L"加载超时", L"Load timed out");
        SetLoading(false);
        UpdateNavButtons();
    }
}

// 网页点击:RenderImage 局部坐标 = 位图像素(Stretch=None)= 视口像素。
//  有会话(网络页):转发到引擎 WebCoreClickAt,经真实命中测试 + 默认动作(链接/表单/按钮 onclick/SPA)。
//  无会话(主页/错误页):退回链接命中表导航。
// 把内容区显示坐标(DIP)映回引擎像素空间(kW×kH)。直呈现模式下 GpuPanel 把 720×1080 表面拉伸填满内容区
//   (并叠加设备分辨率缩放),故点击/焦点坐标需按 (kW/ActualWidth, kH/ActualHeight) 缩放回引擎像素;
//   软件模式 RenderImage Stretch=None 为 1:1,直接取整。★ 这是"直呈现下点击固定/渐增偏移"的根因修复。
void MainPage::MapTapToEngine(double dipX, double dipY, int& outPx, int& outPy)
{
    if (dipX < 0.0 || dipY < 0.0) {
        outPx = -1;
        outPy = -1;
        return;
    }
    double aw = ContentArea->ActualWidth, ah = ContentArea->ActualHeight;
    if (m_gpuPresent && aw > 1.0 && ah > 1.0) {
        outPx = static_cast<int>(dipX * static_cast<double>(kW) / aw + 0.5);
        outPy = static_cast<int>(dipY * static_cast<double>(kH) / ah + 0.5);
    } else {
        outPx = static_cast<int>(dipX + 0.5);
        outPy = static_cast<int>(dipY + 0.5);
    }
}

void MainPage::OnPageTapped(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    HideSuggestions();   // 点页面即收起地址栏建议下拉(否则只能靠导航/清空关 → "关不掉")
    if (m_loading || m_interacting) return;
    // 取相对 ContentArea(承接手势/点击的层,始终参与布局)的坐标。★ 不能用 RenderImage:直呈现模式下它被
    //   Collapsed(让位给 GpuPanel),对已塌缩元素 GetPosition 坐标无效 → 点击错位(滚动后点底部却命中顶部)。
    //   ContentArea 左上角 = 渲染视口原点,故二者在软件模式下等价,直呈现模式下正确。
    auto pt = e->GetPosition(ContentArea);
    int px, py; MapTapToEngine(pt.X, pt.Y, px, py);
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
    TitleText->Text = L8(L"处理中…", L"Working…");

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
    // present 模式:引擎点击已 swapBuffers 到 GpuPanel,无需建位图(BlitToBitmap 本就空转,RenderImage 已隐藏)。
    if (!m_gpuPresent) {
        auto wb = ref new WriteableBitmap(kW, kH);
        BlitToBitmap(wb, *rgba, kW, kH);
        wb->Invalidate();
        RenderImage->Source = wb;
    }
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
        m_urlSyncing = true;
        UrlBox->Text = navUrl;
        m_urlSyncing = false;
        UpdateLockIcon();
        UpdateUrlActionGlyph();
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
        try { rc = WebCoreScrollBy(0, dy, rgba->data()); } catch (...) { rc = -1000; }
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
                        if (!s->m_gpuPresent) {
                            auto wb = ref new WriteableBitmap(kW, kH);
                            BlitToBitmap(wb, *rgba, kW, kH);
                            wb->Invalidate();
                            s->RenderImage->Source = wb;
                        }
                        s->m_pageLinks = *links;
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
void MainPage::FreeScrollBy(int dx, int dy)
{
    if (!m_sessionActive || (dx == 0 && dy == 0)) return;
    m_scrollAccumX += dx;
    m_scrollAccum += dy;
    if (!m_scrollBusy) PumpScroll();
}
void MainPage::PumpScroll()
{
    if ((m_scrollAccum == 0 && m_scrollAccumX == 0) || !m_sessionActive) { m_scrollBusy = false; return; }
    int dy = m_scrollAccum; m_scrollAccum = 0;
    int dx = m_scrollAccumX; m_scrollAccumX = 0;
    m_scrollBusy = true;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = m_opSeq;   // 不自增:被动滚动不作废点击/导航令牌,但被它们作废(导航后丢弃迟到滚动帧)
    bool present = m_gpuPresent;
    // ★ 提速:滚动期间不再每帧跨 FFI 拷贝链接表(引擎侧也跳过了 extractLinks),present 模式连 WriteableBitmap
    //   都不建(引擎已直呈现到 GpuPanel,BlitToBitmap 本就空转)。链接表在滚动停止后由 SyncLinksAfterScroll 一次性补。
    WebEngine::instance().post([disp, self, dx, dy, mySeq, present]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try { rc = WebCoreScrollBy(dx, dy, rgba->data()); } catch (...) { rc = -1000; }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, mySeq, present]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) { s->m_scrollBusy = false; s->m_scrollAccum = 0; return; }   // 被导航/点击取代,丢弃迟到帧
                if (rcCopy == 0) {
                    if (!present) { auto wb = ref new WriteableBitmap(kW, kH); BlitToBitmap(wb, *rgba, kW, kH); wb->Invalidate(); s->RenderImage->Source = wb; }
                    s->m_lastFrameHash = 0;
                }
                s->m_scrollBusy = false;
                if (s->m_scrollAccum != 0) s->PumpScroll();   // 拖拽期间又攒了位移,继续冲刷
                else { s->SyncLinksAfterScroll(); s->StartLiveMode(); }  // 滚动停了 → 补链接表 + 重启实时(新视口懒加载/动画)
            }));
        } catch (...) {}
    });
}

// 滚动停止后一次性刷新链接命中表(滚动期间为提速跳过了引擎 extractLinks)。点击走引擎实时命中测试(权威),
// 故此刷新主要服务点击兜底/主页路径;陈旧窗口仅限"刚停手到这帧返回"之间,无碍。
void MainPage::SyncLinksAfterScroll()
{
    if (!m_sessionActive) return;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = m_opSeq;
    WebEngine::instance().post([disp, self, mySeq]() {
        int rc = -999;
        try { rc = WebCoreSyncLinks(); } catch (...) { rc = -1000; }
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0) {
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) { int lx=0,ly=0,lw=0,lh=0; char lu[1200]=""; if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) { Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu); links->push_back(std::move(pl)); } }
        }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([self, links, rcCopy, mySeq]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) return;   // 被新操作取代
                if (rcCopy == 0) s->m_pageLinks = *links;
            }));
        } catch (...) {}
    });
}
// 自由滚动:内容区 ManipulationDelta(去掉 ScrollViewer 后,触摸不再被吞)。单指拖拽的累计 ΔY → 引擎滚动。
// TranslateInertia 让松手后继续惯性滚(ManipulationDelta 在惯性期持续触发)。点击经 Tapped 走(手势识别器
// 区分点按 vs 拖拽,小位移=Tapped、越阈值=Manipulation,不会冲突)。
void MainPage::OnImageManipDelta(Platform::Object^, Windows::UI::Xaml::Input::ManipulationDeltaRoutedEventArgs^ e)
{
    if (!m_sessionActive) return;
    // M4 捏合缩放:本次增量 Scale≠1(或已进入捏合)→ 捏合模式:只对显示层做实时 ScaleTransform(零引擎调用,
    //   丝滑),不走引擎滚动;松手(OnImageManipCompleted)再把累计缩放提交给引擎按新尺度重栅格。
    float ds = e->Delta.Scale;
    if (m_pinching || (ds > 0.0f && (ds > 1.002f || ds < 0.998f))) {
        m_pinching = true;
        if (ds > 0.0f) m_liveScale *= ds;
        float total = m_pageScale * m_liveScale;          // 钳总缩放到 [0.5,6.0]
        if (total < 0.5f) m_liveScale = 0.5f / m_pageScale;
        if (total > 6.0f) m_liveScale = 6.0f / m_pageScale;
        auto fp = e->Position;                            // 捏合焦点(相对 ContentArea = 视口坐标)
        m_focalX = fp.X; m_focalY = fp.Y;
        ApplyLiveZoom();
        return;
    }
    double dx = -e->Delta.Translation.X;             // 手指左移(ΔX<0)→ 内容右滚(dx>0)
    double dy = -e->Delta.Translation.Y;             // 手指上移(ΔY<0)→ 内容下滚(dy>0)
    int idx = (int)(dx < 0 ? dx - 0.5 : dx + 0.5);
    int idy = (int)(dy < 0 ? dy - 0.5 : dy + 0.5);
    if (idx != 0 || idy != 0) FreeScrollBy(idx, idy);
}

// 实时缩放变换:把 ScaleTransform(以焦点为中心)挂到当前显示层(present=GpuPanel,readback=RenderImage)。
//   只变换已渲染像素 → 捏合期间 60fps 丝滑,不调引擎。
void MainPage::ApplyLiveZoom()
{
    auto t = ref new Windows::UI::Xaml::Media::ScaleTransform();
    t->ScaleX = m_liveScale; t->ScaleY = m_liveScale;
    t->CenterX = m_focalX; t->CenterY = m_focalY;
    if (m_gpuPresent) GpuPanel->RenderTransform = t;
    else RenderImage->RenderTransform = t;
}

// 捏合结束:把累计缩放提交给引擎(WebCoreSetPageScale 按新尺度重栅格 → 文字清晰),回 UI 后复位变换 + 显示清晰帧。
void MainPage::OnImageManipCompleted(Platform::Object^, Windows::UI::Xaml::Input::ManipulationCompletedRoutedEventArgs^)
{
    if (!m_pinching) return;
    m_pinching = false;
    float live = m_liveScale; m_liveScale = 1.0f;
    float newScale = m_pageScale * live;
    if (newScale < 0.5f) newScale = 0.5f;
    if (newScale > 6.0f) newScale = 6.0f;
    // 焦点同样从显示坐标(DIP,用于 ScaleTransform 中心)映回引擎像素空间再交引擎,避免缩放锚点偏。
    int fpx, fpy; MapTapToEngine(m_focalX, m_focalY, fpx, fpy);
    PinchCommit(newScale, fpx, fpy);
}

// 把缩放提交给引擎线程:WebCoreSetPageScale → 新清晰帧;回 UI 后更新已提交尺度 + 复位 RenderTransform + 显示。
void MainPage::PinchCommit(float newScale, int focalX, int focalY)
{
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;
    bool present = m_gpuPresent;
    WebEngine::instance().post([disp, self, newScale, focalX, focalY, mySeq, present]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try { rc = WebCoreSetPageScale(newScale, focalX, focalY, rgba->data()); } catch (...) { rc = -1000; }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, newScale, mySeq, present]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) return;            // 被新操作取代,丢弃迟到帧
                if (rcCopy == 0) {
                    s->m_pageScale = newScale;
                    if (!present) { auto wb = ref new WriteableBitmap(kW, kH); BlitToBitmap(wb, *rgba, kW, kH); wb->Invalidate(); s->RenderImage->Source = wb; }
                    // present 模式:引擎已 swapBuffers 到 GpuPanel,新清晰帧已在面板上。
                }
                // 复位实时变换(新帧已是按新尺度渲染的清晰图;变换归一,避免叠加二次缩放)。
                s->GpuPanel->RenderTransform = nullptr;
                s->RenderImage->RenderTransform = nullptr;
                s->StartLiveMode();
            }));
        } catch (...) {}
    });
}

// ---- GPU 路径1 探针 ----
void MainPage::OnGpuPanelLoaded(Platform::Object^, RoutedEventArgs^)
{
    static bool s_done = false;
    if (s_done) return;   // 只跑一次
    s_done = true;
    try { RunGpuProbe(GpuPanel, ref new String(LocalStateDir().c_str())); } catch (...) {}
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
    // 诊断埋点:记录本回调是否触发 + 门控状态 + ImeBox 文本长度(写 LocalState\imedebug.txt,真机测后拉取)。
    // 若打字后此文件为空 → OnImeTextChanged 没触发 → 隐藏 ImeBox 收不到 IME 文本(UI 层问题);
    // 若有记录但输入框没字 → 引擎层(看 SendKeyToEngine 写的 rc:kErrNoDocument=canEdit 丢焦点)。
    try {
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ofstream f(WideToUtf8(d) + "\\imedebug.txt", std::ios::app | std::ios::binary);
            if (f) { std::string s = "TC open=" + std::to_string(m_imeOpen) + " sess=" + std::to_string(m_sessionActive)
                + " sync=" + std::to_string(m_imeSyncing) + " textLen=" + std::to_string(ImeBox->Text ? ImeBox->Text->Length() : 0) + "\n"; f.write(s.data(), s.size()); }
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
        char edbg[256] = ""; try { WebCoreEditDebug(edbg, sizeof edbg); } catch (...) {}
        try { std::wstring dd = LocalStateDir(); if (!dd.empty()) { std::ofstream f(WideToUtf8(dd) + "\\imedebug.txt", std::ios::app | std::ios::binary); if (f) { std::string s = "  SK kind=" + std::to_string(kind) + " rc=" + std::to_string(rc) + " [" + edbg + "]\n"; f.write(s.data(), s.size()); } } } catch (...) {}
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
                    if (!s->m_gpuPresent) { auto wb = ref new WriteableBitmap(kW, kH); BlitToBitmap(wb, *rgba, kW, kH); wb->Invalidate(); s->RenderImage->Source = wb; }
                    s->m_lastFrameHash = 0;
                    s->StartLiveMode();   // 打字后重启实时循环 → 后续帧把输入内容再合成/呈现一次(防单帧合成漏掉新文字)
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
    if (ActionMenu->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (SettingsPage->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (TabSwitcher->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
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
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible
        || ActionMenu->Visibility == Windows::UI::Xaml::Visibility::Visible
        || SettingsPage->Visibility == Windows::UI::Xaml::Visibility::Visible
        || TabSwitcher->Visibility == Windows::UI::Xaml::Visibility::Visible) { StopLiveMode(); return; }
    if (m_liveBusy) {                      // 上一帧引擎任务还没回
        if (++m_liveBusyAge < 5) return;   // 正常等(~1s 内)
        m_liveBusy = false;                // 卡过久 → RunAsync 很可能丢了,自愈不死循环
    }
    m_liveBusyAge = 0;
    m_liveBusy = true;
    unsigned long long mySeq = m_opSeq;    // 只读不自增:实时帧是被动的,绝不能作废正在进行的真操作
    bool present = m_gpuPresent;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    WebEngine::instance().post([disp, self, mySeq, present]() {
        MainPage^ s0 = self.Get();
        if (!s0 || !s0->m_appForeground) return;   // 已切后台:别在 PLM 冻结风险下跑 JS+绘制(m_liveBusy 由恢复时 StartLiveMode 清)
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999; unsigned hash = 0; int pending = 0;
        try { rc = WebCoreLiveTick(rgba->data()); if (rc == 0) { hash = WebCoreGetFrameHash(); pending = WebCoreGetPendingResourceCount(); } } catch (...) { rc = -1000; }
        int rcCopy = rc; unsigned hashCopy = hash; int pendingCopy = pending;
        try {
            disp->RunAsync(CoreDispatcherPriority::Low,
                ref new DispatchedHandler([self, rgba, rcCopy, hashCopy, pendingCopy, mySeq, present]() {
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
                        if (pendingCopy > 0) {
                            s->m_liveStaticTicks = 0;            // 仍有图片/子资源在途:继续 tick,等待完成回调和解码
                            int ticks = ++s->m_liveTotalTicks;
                            if (ticks == 150 && s->m_liveTimer) {
                                Windows::Foundation::TimeSpan slow; slow.Duration = 10000000LL;   // 1s ≈ 1fps
                                s->m_liveTimer->Interval = slow;
                            }
                            if (ticks >= 300)
                                s->StopLiveMode();
                            return;
                        }
                        if (++s->m_liveStaticTicks >= 40) s->StopLiveMode();   // ~8s 静止 → 停,给慢图片/解码留余量
                        return;
                    }
                    s->m_liveStaticTicks = 0;
                    s->m_lastFrameHash = hashCopy;
                    if (!present) {
                        auto wb = ref new WriteableBitmap(kW, kH);
                        BlitToBitmap(wb, *rgba, kW, kH);
                        wb->Invalidate();
                        s->RenderImage->Source = wb;
                    }
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
void MainPage::OnHome(Platform::Object^, RoutedEventArgs^) { NavigateTo(ref new String(g_homeUrl.c_str()), true); }
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

// 实体返回键:层级优先关浮层 → 浏览器后退 → 否则交系统(e->Handled 保持 false → 最小化/退出)。
void MainPage::OnHardwareBack(Platform::Object^, Windows::UI::Core::BackRequestedEventArgs^ e)
{
    using V = Windows::UI::Xaml::Visibility;
    if (OobePanel && OobePanel->Visibility == V::Visible) { e->Handled = true; return; }   // 选语言前拦住,别退出
    if (SuggestPanel && SuggestPanel->Visibility == V::Visible) { HideSuggestions(); e->Handled = true; return; }
    if (FindBar && FindBar->Visibility == V::Visible) { OnFindClose(nullptr, nullptr); e->Handled = true; return; }
    if (ActionMenu && ActionMenu->Visibility == V::Visible) { HideActionMenu(); e->Handled = true; return; }
    if (TabSwitcher && TabSwitcher->Visibility == V::Visible) { HideTabSwitcher(); e->Handled = true; return; }
    if (Drawer && Drawer->Visibility == V::Visible) { HideDrawer(); e->Handled = true; return; }
    if (SettingsPage && SettingsPage->Visibility == V::Visible) { HideSettings(); e->Handled = true; return; }
    if (m_navIndex > 0 && !m_loading) { OnBack(nullptr, nullptr); e->Handled = true; return; }
}
void MainPage::OnMenu(Platform::Object^, RoutedEventArgs^) { ShowActionMenu(); }

// UA 切换:手机/桌面。切引擎 UA 后重载当前页生效。(抽屉头部按钮)
void MainPage::OnToggleUA(Platform::Object^, RoutedEventArgs^)
{
    HideDrawer();
    DoToggleUA();
}

// GPU 合成开关(M2):一次性开启(引擎侧 g_gpuActive 无 teardown,重启回软件)。开启 = 引擎线程
// WebCoreGpuInit(nullptr=离屏)成功 → g_gpuActive=true → 重载当前页 → buildSession 开合成 → 经
// TextureMapper 合成到离屏纹理、readback 出像素(仍走 WriteableBitmap 显示)。结果写 gpuinit.txt 供真机回报。
// GPU 朝向标签:🖥 GPU·<HV/H/V/->(供真机循环时看当前组合并回报)。
static Platform::String^ GpuOrientLabel(int orient)
{
    const wchar_t* tag = (orient == 0) ? L"-" : (orient == 1) ? L"H" : (orient == 2) ? L"V" : L"HV";
    return ref new Platform::String((std::wstring(L"\U0001F5A5 GPU·") + tag).c_str());   // 🖥 GPU·HV
}

// GPU 合成开关(M2):首点 = 引擎线程 WebCoreGpuInit(离屏)→ 成功后重载当前页(buildSession 开合成→
//   经 TextureMapper 合成 readback 出像素)。已开后每点一次 = 循环 4 种 readback 朝向(none/H/V/HV)并重绘
//   当前帧——真机朝向经验未定,点到画面正常那个,把标签(H/V/HV/-)告诉我即可定死。重启回软件。
void MainPage::OnToggleGpu(Platform::Object^, RoutedEventArgs^)
{
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);

    if (!m_gpuOn) { HideDrawer(); EnableGpu(); return; }   // 首次开启 → 统一走 EnableGpu(含崩溃环路保护)

    // 已开(朝向已定 none):再点 = 抓合成图层树诊断 → 写 LocalState\layertree.txt + 标题显示关键标量
    //   (scrollPos/contents/view/docBg/usesCompositing),供定位"背景丢失 / 不能滚动"。
    HideDrawer();
    WebEngine::instance().post([disp, self]() {
        auto buf = std::make_shared<std::vector<char>>(65536, 0);
        try { WebCoreGpuLayerInfo(buf->data(), (int)buf->size()); } catch (...) {}
        std::string info(buf->data());
        try {
            std::wstring d = LocalStateDir();
            if (!d.empty()) {
                std::ofstream f(WideToUtf8(d) + "\\layertree.txt", std::ios::binary | std::ios::trunc);
                if (f) f.write(info.data(), info.size());
            }
        } catch (...) {}
        std::string head = info.substr(0, info.find('\n'));
        std::wstring headW = Utf8ToWide(head);
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, headW]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    s->TitleText->Text = ref new Platform::String(headW.c_str());   // 标题临时显示诊断首行
                }));
        } catch (...) {}
    });
}

// ---- 抽屉 ----
void MainPage::OnDrawerClose(Platform::Object^, RoutedEventArgs^) { HideDrawer(); }
void MainPage::OnTabFav(Platform::Object^, RoutedEventArgs^)  { ShowDrawer(DrawerTab::Favorites); }
void MainPage::OnTabHist(Platform::Object^, RoutedEventArgs^) { ShowDrawer(DrawerTab::History); }
void MainPage::OnTabDl(Platform::Object^, RoutedEventArgs^)   { ShowDrawer(DrawerTab::Downloads); }

void MainPage::OnPrimaryAction(Platform::Object^, RoutedEventArgs^)
{
    if (m_tab == DrawerTab::Favorites) {
        ToggleBookmark();   // 收藏/取消(内部含保存 + 抽屉可见时刷新列表)
        ActionBtn->Content = (!m_currentUrl.empty() && IsBookmarked(m_currentUrl)) ? L8(L"★ 取消收藏", L"★ Remove bookmark") : L8(L"★ 收藏此页", L"★ Bookmark this");
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
    HideSuggestions();
    Drawer->Visibility = Windows::UI::Xaml::Visibility::Visible;
    // 主操作按钮文案随标签变化
    if (tab == DrawerTab::Favorites)
        ActionBtn->Content = (!m_currentUrl.empty() && IsBookmarked(m_currentUrl)) ? L8(L"★ 取消收藏", L"★ Remove bookmark") : L8(L"★ 收藏此页", L"★ Bookmark this");
    else if (tab == DrawerTab::History)
        ActionBtn->Content = L8(L"\U0001F5D1 清空", L"\U0001F5D1 Clear");
    else
        ActionBtn->Content = L8(L"↓ 下载此页", L"↓ Download page");
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
        empty->Text = (m_tab == DrawerTab::Favorites) ? L8(L"暂无收藏", L"No bookmarks yet") : (m_tab == DrawerTab::History ? L8(L"暂无历史记录", L"No history yet") : L8(L"暂无下载", L"No downloads yet"));
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
            del->Content = L8(L"✕ 删除收藏", L"✕ Remove");
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

    TitleText->Text = ref new String(((g_lang == L"en" ? L"Downloading  " : L"下载中  ") + fn).c_str());
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

// ============================================================================
// 增量1:地址栏(上下文键 Go/刷新/停止 + 安全锁标 + 历史/书签建议下拉)
// 纯 UI 层,不碰 ContentArea 坐标映射 / 引擎交互路径。
// ============================================================================

void MainPage::Reload()
{
    if (m_currentUrl.empty() || m_currentUrl == L"about:home")
        NavigateTo(ref new String(L"about:home"), false);
    else
        NavigateTo(ref new String(m_currentUrl.c_str()), false);
}

// 上下文键:加载中=停止;有未提交输入=Go;否则=刷新当前页。
void MainPage::OnUrlAction(Platform::Object^, RoutedEventArgs^)
{
    if (m_loading) {
        // 停止:作废在途回调(opSeq++),停看门狗,排队关会话取消网络(单引擎线程串行,加载 job 跑完后才执行)。
        ++m_opSeq;
        m_interacting = false;
        if (m_loadWatchdog) m_loadWatchdog->Stop();
        WebEngine::instance().post([]() { try { WebCoreCloseSession(); } catch (...) {} });
        m_sessionActive = false;
        ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        SetLoading(false);
        TitleText->Text = L8(L"已停止", L"Stopped");
        return;
    }
    std::wstring boxText = UrlBox->Text ? std::wstring(UrlBox->Text->Data()) : L"";
    bool pendingEdit = (m_currentUrl == L"about:home") ? !boxText.empty() : (boxText != m_currentUrl);
    HideSuggestions();
    if (pendingEdit) NavigateTo(NormalizeUrl(UrlBox->Text), true);
    else Reload();
}

void MainPage::UpdateUrlActionGlyph()
{
    if (!UrlActionBtn) return;
    if (m_loading) { UrlActionBtn->Content = ref new String(L"\x2715"); return; }   // ✕ 停止
    std::wstring boxText = UrlBox->Text ? std::wstring(UrlBox->Text->Data()) : L"";
    bool pendingEdit = (m_currentUrl == L"about:home") ? !boxText.empty() : (boxText != m_currentUrl);
    UrlActionBtn->Content = ref new String(pendingEdit ? L"\x2192" : L"\x21BB");     // → Go / ⟳ 刷新
}

// Segoe MDL2 Assets:Lock=E72E,Warning=E7BA。本地/主页留空。
void MainPage::UpdateLockIcon()
{
    if (!LockIcon) return;
    const std::wstring& u = m_currentUrl;
    if (u.empty() || u == L"about:home" || u.rfind(L"about:", 0) == 0) {
        LockIcon->Text = ref new String(L"");
    } else if (u.rfind(L"https://", 0) == 0) {
        LockIcon->Text = ref new String(L"\xE72E");
        LockIcon->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x5C, 0xB8, 0x5C));
    } else if (u.rfind(L"http://", 0) == 0) {
        LockIcon->Text = ref new String(L"\xE7BA");
        LockIcon->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0xE0, 0xA0, 0x30));
    } else {
        LockIcon->Text = ref new String(L"");
    }
}

void MainPage::OnUrlChanged(Platform::Object^, Windows::UI::Xaml::Controls::TextChangedEventArgs^)
{
    UpdateUrlActionGlyph();
    if (m_urlSyncing) { HideSuggestions(); return; }   // 程序化同步地址栏(导航/回调):绝不弹建议
    if (!m_urlFocused) { HideSuggestions(); return; }   // 没在编辑地址栏:绝不弹(杜绝"莫名其妙弹出")
    std::wstring q = UrlBox->Text ? std::wstring(UrlBox->Text->Data()) : L"";
    if (q.empty()) { HideSuggestions(); return; }
    ShowSuggestions(q);
}

void MainPage::OnUrlGotFocus(Platform::Object^, RoutedEventArgs^) { m_urlFocused = true; }
// 不在 LostFocus 里收建议:点建议项会先夺焦再触发其 Click,提前收会取消点击。改由点页面(OnPageTapped)/导航收。
void MainPage::OnUrlLostFocus(Platform::Object^, RoutedEventArgs^) { m_urlFocused = false; }

// 历史 + 书签子串匹配(url/title,忽略大小写),去重,最多 8 条。点项即导航。
void MainPage::ShowSuggestions(const std::wstring& query)
{
    if (!SuggestPanel || !SuggestList) return;
    SuggestList->Children->Clear();
    std::wstring ql = query;
    std::transform(ql.begin(), ql.end(), ql.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });

    std::vector<Entry> matches;
    std::vector<std::wstring> seen;
    auto consider = [&](const std::vector<Entry>& src) {
        for (const auto& e : src) {
            if (matches.size() >= 8) break;
            std::wstring ul = e.url, tl = e.title;
            std::transform(ul.begin(), ul.end(), ul.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });
            std::transform(tl.begin(), tl.end(), tl.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });
            if (ul.find(ql) == std::wstring::npos && tl.find(ql) == std::wstring::npos) continue;
            if (std::find(seen.begin(), seen.end(), e.url) != seen.end()) continue;
            seen.push_back(e.url);
            matches.push_back(e);
        }
    };
    consider(m_bookmarks);
    consider(m_historyList);
    if (matches.empty()) { HideSuggestions(); return; }

    Platform::Agile<MainPage^> self(this);
    for (const auto& e : matches) {
        std::wstring u = e.url;
        auto row = MakeRow(ref new String(e.title.empty() ? e.url.c_str() : e.title.c_str()),
                           ref new String(e.url.c_str()),
                           ColorHelper::FromArgb(255, 0xF0, 0xF0, 0xF0));
        auto btn = ref new Button();
        btn->Background = ref new SolidColorBrush(Colors::Transparent);
        btn->BorderThickness = Thickness(0);
        btn->Padding = Thickness(0);
        btn->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        btn->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        btn->Content = row;
        btn->Click += ref new RoutedEventHandler([self, u](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->HideSuggestions();
            s->m_urlSyncing = true; s->UrlBox->Text = ref new String(u.c_str()); s->m_urlSyncing = false;
            s->NavigateTo(ref new String(u.c_str()), true);
        });
        SuggestList->Children->Append(btn);
    }
    SuggestPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

void MainPage::HideSuggestions()
{
    if (SuggestPanel) SuggestPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

// ============================================================================
// 增量2:动作面板(菜单键弹出的 action sheet)+ 分享/复制链接/收藏/UA
// ============================================================================

void MainPage::ShowActionMenu()
{
    HideSuggestions();
    if (ActFavLabel)
        ActFavLabel->Text = (!m_currentUrl.empty() && IsBookmarked(m_currentUrl)) ? L8(L"已收藏", L"Saved") : L8(L"收藏", L"Bookmark");
    if (ActUaLabel)
        ActUaLabel->Text = m_uaMobile ? L8(L"桌面版网站", L"Desktop site") : L8(L"移动版网站", L"Mobile site");
    ActionMenu->Visibility = Windows::UI::Xaml::Visibility::Visible;
    StopLiveMode();   // 面板盖住网页,暂停实时渲染省电
}

void MainPage::HideActionMenu()
{
    ActionMenu->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    StartLiveMode();
}

void MainPage::OnActionScrimTap(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^)
{
    HideActionMenu();   // 点遮罩空白处关闭
}

void MainPage::OnSheetTap(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;  // 点面板本体不冒泡到遮罩(否则点空白区会误关)
}

// 动作分发:读 Button.Tag。先关面板再执行(避免动作触发的 UI 变化被面板挡住)。
void MainPage::OnAction(Platform::Object^ sender, RoutedEventArgs^)
{
    std::wstring t;
    auto btn = dynamic_cast<Button^>(sender);
    if (btn) { auto tag = dynamic_cast<Platform::String^>(btn->Tag); if (tag) t = std::wstring(tag->Data()); }
    HideActionMenu();
    if (t == L"reload") Reload();
    else if (t == L"share") DoShare();
    else if (t == L"copylink") DoCopyLink();
    else if (t == L"bookmark") ToggleBookmark();
    else if (t == L"newtab") NewTab();
    else if (t == L"home") NavigateTo(ref new String(g_homeUrl.c_str()), true);
    else if (t == L"ua") DoToggleUA();
    else if (t == L"find") ShowFindBar();
    else if (t == L"download") { if (!m_currentUrl.empty() && m_currentUrl != L"about:home") StartDownload(ref new String(m_currentUrl.c_str())); }
    else if (t == L"bookmarks") ShowDrawer(DrawerTab::Favorites);
    else if (t == L"history") ShowDrawer(DrawerTab::History);
    else if (t == L"downloads") ShowDrawer(DrawerTab::Downloads);
    else if (t == L"settings") ShowSettings();
}

void MainPage::ToggleBookmark()
{
    if (m_currentUrl.empty() || m_currentUrl == L"about:home") return;
    if (IsBookmarked(m_currentUrl)) {
        m_bookmarks.erase(std::remove_if(m_bookmarks.begin(), m_bookmarks.end(),
            [&](const Entry& e) { return e.url == m_currentUrl; }), m_bookmarks.end());
        TitleText->Text = L8(L"已取消收藏", L"Bookmark removed");
    } else {
        Entry e; e.url = m_currentUrl; e.title = m_currentTitle.empty() ? m_currentUrl : m_currentTitle;
        m_bookmarks.insert(m_bookmarks.begin(), e);
        TitleText->Text = L8(L"已收藏", L"Bookmarked");
    }
    SaveBookmarks();
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible && m_tab == DrawerTab::Favorites)
        RebuildDrawerList();
}

void MainPage::DoShare()
{
    if (m_currentUrl.empty() || m_currentUrl == L"about:home") { TitleText->Text = L8(L"无可分享内容", L"Nothing to share"); return; }
    try { Windows::ApplicationModel::DataTransfer::DataTransferManager::ShowShareUI(); } catch (...) {}
}

void MainPage::DoCopyLink()
{
    if (m_currentUrl.empty() || m_currentUrl == L"about:home") return;
    try {
        auto dp = ref new Windows::ApplicationModel::DataTransfer::DataPackage();
        dp->SetText(ref new String(m_currentUrl.c_str()));
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
        TitleText->Text = L8(L"已复制链接", L"Link copied");
    } catch (...) {}
}

void MainPage::DoToggleUA()
{
    m_uaMobile = !m_uaMobile;
    if (UaBtn) UaBtn->Content = L8(m_uaMobile ? L"\U0001F4F1 手机UA" : L"\U0001F5A5 桌面UA", m_uaMobile ? L"\U0001F4F1 Mobile UA" : L"\U0001F5A5 Desktop UA");
    int mobile = m_uaMobile ? 1 : 0;
    WebEngine::instance().post([mobile]() { try { WebCoreSetUserAgentMobile(mobile); } catch (...) {} });
    if (!m_currentUrl.empty() && m_currentUrl != L"about:home")
        NavigateTo(ref new String(m_currentUrl.c_str()), false);   // 重载使新 UA 生效
}

// ============================================================================
// 增量3:设置页(搜索引擎/主页/默认UA/缩放/标签模式)+ 清除数据 + 调试导出
// ============================================================================

void MainPage::ApplySettings()
{
    g_searchPrefix = SearchPrefixFor(m_setSearch);
    if (m_defaultZoom < 50) m_defaultZoom = 50;
    if (m_defaultZoom > 200) m_defaultZoom = 200;
    m_uaMobile = !m_setUaDesktop;
    int mobile = m_uaMobile ? 1 : 0;
    std::string ua = WideToUtf8(m_uaCustom);
    WebEngine::instance().post([mobile, ua]() {
        try { WebCoreSetUserAgentMobile(mobile); } catch (...) {}
        try { WebCoreSetUserAgentString(ua.empty() ? nullptr : ua.c_str()); } catch (...) {}   // 自定义 UA(空=清除回退开关)
    });
    if (UaBtn) {
        bool en = (g_lang == L"en");
        UaBtn->Content = ref new String(m_uaMobile ? (en ? L"\U0001F4F1 Mobile UA" : L"\U0001F4F1 手机UA")
                                                   : (en ? L"\U0001F5A5 Desktop UA" : L"\U0001F5A5 桌面UA"));
    }
}

void MainPage::LoadSettings()
{
    std::wstring d = LocalStateDir();
    if (d.empty()) { ApplySettings(); return; }
    std::ifstream f(WideToUtf8(d) + "\\settings.ini", std::ios::binary);
    if (f) {
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "search") m_setSearch = atoi(v.c_str());
            else if (k == "home") g_homeUrl = v.empty() ? L"about:home" : Utf8ToWide(v);
            else if (k == "ua") m_setUaDesktop = (atoi(v.c_str()) != 0);
            else if (k == "zoom") m_defaultZoom = atoi(v.c_str());
            else if (k == "tabmode") m_tabMode = atoi(v.c_str());
            else if (k == "gpudefault") m_gpuDefault = (atoi(v.c_str()) != 0);
            else if (k == "ua_custom") m_uaCustom = Utf8ToWide(v);
            else if (k == "lang") { g_lang = Utf8ToWide(v); m_langSet = true; }
        }
    }
    if (m_setSearch < 0 || m_setSearch > 3) m_setSearch = 0;
    if (g_lang != L"en" && g_lang != L"zh") g_lang = L"zh";
    ApplySettings();
}

void MainPage::SaveSettings()
{
    std::wstring d = LocalStateDir();
    if (d.empty()) return;
    std::string s;
    s += "search=" + std::to_string(m_setSearch) + "\n";
    s += "home=" + (g_homeUrl == L"about:home" ? std::string() : WideToUtf8(g_homeUrl)) + "\n";
    s += "ua=" + std::to_string(m_setUaDesktop ? 1 : 0) + "\n";
    s += "zoom=" + std::to_string(m_defaultZoom) + "\n";
    s += "tabmode=" + std::to_string(m_tabMode) + "\n";
    s += "gpudefault=" + std::to_string(m_gpuDefault ? 1 : 0) + "\n";
    s += "ua_custom=" + WideToUtf8(m_uaCustom) + "\n";
    s += "lang=" + WideToUtf8(g_lang) + "\n";
    std::ofstream f(WideToUtf8(d) + "\\settings.ini", std::ios::binary | std::ios::trunc);
    if (f) f.write(s.data(), s.size());
}

void MainPage::ShowSettings()
{
    HideActionMenu();
    if (SetSearchCombo) SetSearchCombo->SelectedIndex = m_setSearch;
    if (SetHomeBox) SetHomeBox->Text = ref new String(g_homeUrl == L"about:home" ? L"" : g_homeUrl.c_str());
    if (SetUaSwitch) SetUaSwitch->IsOn = m_setUaDesktop;
    if (SetTabModeSwitch) SetTabModeSwitch->IsOn = (m_tabMode == 1);
    if (SetZoomSlider) SetZoomSlider->Value = m_defaultZoom;
    if (SetZoomLabel) SetZoomLabel->Text = ref new String((std::to_wstring(m_defaultZoom) + L"%").c_str());
    if (SetGpuSwitch) SetGpuSwitch->IsOn = m_gpuDefault;
    if (SetUaCustomBox) SetUaCustomBox->Text = ref new String(m_uaCustom.c_str());
    if (VersionText) {
        auto pv = Windows::ApplicationModel::Package::Current->Id->Version;
        std::wstring v = (g_lang == L"en" ? L"Version " : L"版本 ") + std::to_wstring(pv.Major) + L"." + std::to_wstring(pv.Minor)
                       + L"." + std::to_wstring(pv.Build) + L"." + std::to_wstring(pv.Revision);
        VersionText->Text = ref new String(v.c_str());
    }
    SettingsPage->Visibility = Windows::UI::Xaml::Visibility::Visible;
    StopLiveMode();
}

void MainPage::HideSettings()
{
    if (SetSearchCombo && SetSearchCombo->SelectedIndex >= 0) m_setSearch = SetSearchCombo->SelectedIndex;
    if (SetHomeBox) {
        std::wstring h = SetHomeBox->Text ? std::wstring(SetHomeBox->Text->Data()) : L"";
        while (!h.empty() && (h.front() == L' ' || h.front() == L'\t')) h.erase(h.begin());
        while (!h.empty() && (h.back() == L' ' || h.back() == L'\t')) h.pop_back();
        if (h.empty() || h == L"about:home") g_homeUrl = L"about:home";
        else { if (h.rfind(L"http", 0) != 0 && h.rfind(L"about:", 0) != 0) h = L"https://" + h; g_homeUrl = h; }
    }
    if (SetUaSwitch) m_setUaDesktop = SetUaSwitch->IsOn;
    if (SetTabModeSwitch) m_tabMode = SetTabModeSwitch->IsOn ? 1 : 0;
    if (SetZoomSlider) m_defaultZoom = (int)(SetZoomSlider->Value + 0.5);
    if (SetGpuSwitch) m_gpuDefault = SetGpuSwitch->IsOn;
    if (SetUaCustomBox) {
        std::wstring u = SetUaCustomBox->Text ? std::wstring(SetUaCustomBox->Text->Data()) : L"";
        while (!u.empty() && (u.front() == L' ' || u.front() == L'\t')) u.erase(u.begin());
        while (!u.empty() && (u.back() == L' ' || u.back() == L'\t' || u.back() == L'\r' || u.back() == L'\n')) u.pop_back();
        m_uaCustom = u;
    }
    ApplySettings();
    SaveSettings();
    SettingsPage->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    StartLiveMode();
}

void MainPage::OnSettingsBack(Platform::Object^, RoutedEventArgs^) { HideSettings(); }

// ============================== OOBE / 多语言 ==============================
// 中→英串表。选 English 时遍历已加载 XAML 树就地替换(键含 emoji/glyph 前缀的须全字匹配)。
static const wchar_t* const kI18n[][2] = {
    { L"页内查找", L"Find in page" }, { L"搜索或输入网址", L"Search or enter URL" },
    { L"后退", L"Back" }, { L"前进", L"Forward" }, { L"刷新", L"Reload" }, { L"收藏", L"Bookmark" },
    { L"新标签页", L"New tab" }, { L"主页", L"Home" }, { L"桌面版网站", L"Desktop site" },
    { L"分享", L"Share" }, { L"复制链接", L"Copy link" }, { L"下载此页", L"Download page" },
    { L"书签", L"Bookmarks" }, { L"历史记录", L"History" }, { L"下载内容", L"Downloads" }, { L"设置", L"Settings" },
    { L"菜单", L"Menu" },
    { L"\U0001F4F1 手机UA", L"\U0001F4F1 Mobile UA" }, { L"\U0001F5A5 桌面UA", L"\U0001F5A5 Desktop UA" },
    { L"★ 收藏", L"★ Favorites" }, { L"\U0001F551 历史", L"\U0001F551 History" },
    { L"↓ 下载", L"↓ Downloads" }, { L"★ 收藏此页", L"★ Bookmark this" },
    { L"默认搜索引擎", L"Default search engine" }, { L"百度", L"Baidu" },
    { L"主页(URL,留空用内置主页)", L"Home (URL; blank = built-in)" },
    { L"自定义 User-Agent(留空=用上面的开关;改后刷新网页生效)", L"Custom User-Agent (blank = use the switch above; reload to apply)" },
    { L"默认缩放", L"Default zoom" },
    { L"启动请求桌面版网站", L"Request desktop site on launch" },
    { L"并发多引擎标签(暂搁置,后续实现)", L"Concurrent multi-engine tabs (planned)" },
    { L"默认启用 GPU 渲染(加载首个网页后自动开)", L"Enable GPU rendering by default (auto after first page)" },
    { L"立即开启 GPU 合成(重启回软件)", L"Enable GPU compositing now (restart reverts)" },
    { L"清除数据", L"Clear data" }, { L"清除历史记录", L"Clear history" },
    { L"清除全部收藏", L"Clear all bookmarks" }, { L"清除下载记录", L"Clear downloads" },
    { L"诊断", L"Diagnostics" }, { L"导出调试日志 / 崩溃 dump", L"Export debug log / crash dump" },
    { L"关于 / 更新", L"About / Update" }, { L"版本 —", L"Version —" },
    { L"检查更新(GitHub Releases)", L"Check for updates (GitHub Releases)" },
    { L"标签", L"Tabs" }, { L"完成", L"Done" }, { L"新建标签页", L"New tab" },
};
static Platform::String^ I18n(Platform::String^ s, bool toEn) {
    if (s == nullptr) return s;
    std::wstring w(s->Data());
    for (auto& m : kI18n) {
        if (toEn) { if (w == m[0]) return ref new Platform::String(m[1]); }
        else      { if (w == m[1]) return ref new Platform::String(m[0]); }
    }
    return s;
}

void MainPage::TranslateNode(Platform::Object^ node, bool toEn) {
    using namespace Windows::UI::Xaml;
    using namespace Windows::UI::Xaml::Controls;
    if (node == nullptr) return;
    if (auto tb = dynamic_cast<TextBlock^>(node)) { tb->Text = I18n(tb->Text, toEn); return; }
    if (auto tx = dynamic_cast<TextBox^>(node)) { tx->PlaceholderText = I18n(tx->PlaceholderText, toEn); return; }
    if (auto sw = dynamic_cast<ToggleSwitch^>(node)) { if (auto h = dynamic_cast<Platform::String^>(sw->Header)) sw->Header = I18n(h, toEn); return; }
    if (auto cbx = dynamic_cast<ComboBox^>(node)) { for (unsigned i = 0; i < cbx->Items->Size; ++i) { if (auto ci = dynamic_cast<ComboBoxItem^>(cbx->Items->GetAt(i))) if (auto s = dynamic_cast<Platform::String^>(ci->Content)) ci->Content = I18n(s, toEn); } return; }
    if (auto p = dynamic_cast<Panel^>(node)) { for (auto c : p->Children) TranslateNode(c, toEn); return; }
    if (auto bd = dynamic_cast<Border^>(node)) { TranslateNode(bd->Child, toEn); return; }
    if (auto sv = dynamic_cast<ScrollViewer^>(node)) { TranslateNode(sv->Content, toEn); return; }
    if (auto cc = dynamic_cast<ContentControl^>(node)) {   // Button 等
        if (auto s = dynamic_cast<Platform::String^>(cc->Content)) cc->Content = I18n(s, toEn);
        else TranslateNode(cc->Content, toEn);
        return;
    }
}

void MainPage::ApplyLanguage() {
    if (g_lang != L"en") return;   // 默认中文,XAML 原文即中文,无需翻译
    TranslateNode(this->Content, true);
}

void MainPage::OnOobeLang(Platform::Object^ sender, RoutedEventArgs^) {
    std::wstring tag = L"zh";
    if (auto b = dynamic_cast<Windows::UI::Xaml::Controls::Button^>(sender))
        if (auto t = dynamic_cast<Platform::String^>(b->Tag)) tag = std::wstring(t->Data());
    g_lang = (tag == L"en") ? L"en" : L"zh";
    m_langSet = true;
    if (g_lang == L"en") ApplyLanguage();   // 立即把整个界面翻成英文
    SaveSettings();
    if (OobePanel) OobePanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::OnZoomChanged(Platform::Object^, Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs^ e)
{
    if (SetZoomLabel) SetZoomLabel->Text = ref new String((std::to_wstring((int)(e->NewValue + 0.5)) + L"%").c_str());
}

void MainPage::OnSettingsBtn(Platform::Object^ sender, RoutedEventArgs^)
{
    std::wstring t;
    auto b = dynamic_cast<Button^>(sender);
    if (b) { auto tag = dynamic_cast<Platform::String^>(b->Tag); if (tag) t = std::wstring(tag->Data()); }
    if (t == L"clearhist") { m_historyList.clear(); SaveHistory(); TitleText->Text = L8(L"历史记录已清除", L"History cleared"); }
    else if (t == L"clearfav") { m_bookmarks.clear(); SaveBookmarks(); TitleText->Text = L8(L"收藏已清除", L"Bookmarks cleared"); }
    else if (t == L"cleardl") { m_downloads.clear(); SaveDownloads(); TitleText->Text = L8(L"下载记录已清除", L"Downloads cleared"); }
    else if (t == L"export") ExportDebug();
    else if (t == L"gpu") { HideSettings(); OnToggleGpu(nullptr, nullptr); }
    else if (t == L"checkupdate") CheckForUpdate(true);
}

// ---- 检测更新 ----
// 后台线程(独立 curl,WebCoreDownload 不碰引擎 Page 状态,可离引擎线程跑)拉 GitHub Releases API,
// 比对当前 appx 版本(Package.Current)。有新版弹对话框→可直接在本浏览器里打开发布页下载 appx 手动装。
// manual=true:用户在设置里点的,无更新/失败也提示;false:启动静默自检,仅有新版才提示。
static bool ParseDottedVersion(const std::string& s, int out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    int idx = 0; long cur = 0; bool any = false;
    for (size_t i = 0; i <= s.size() && idx < 4; ++i) {
        if (i < s.size() && s[i] >= '0' && s[i] <= '9') { cur = cur * 10 + (s[i] - '0'); any = true; }
        else if (i == s.size() || s[i] == '.') { out[idx++] = (int)cur; cur = 0; if (i == s.size()) break; }
        else break;   // 非数字非点(如 tag 后缀)→ 停
    }
    return any;
}

void MainPage::CheckForUpdate(bool manual)
{
    if (m_updateChecking) return;
    m_updateChecking = true;
    if (manual) TitleText->Text = L8(L"正在检查更新…", L"Checking for updates…");

    // 当前版本(从 appx 清单读,不写死)
    auto pv = Windows::ApplicationModel::Package::Current->Id->Version;
    int cur[4] = { pv.Major, pv.Minor, pv.Build, pv.Revision };
    std::wstring dir = LocalStateDir();
    std::string jsonPath = dir.empty() ? std::string() : WideToUtf8(dir + L"\\update.json");

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    std::thread([disp, self, manual, jsonPath, cur]() {
        int rc = -1;
        std::string body;
        if (!jsonPath.empty()) {
            try { rc = WebCoreDownload(
                "https://api.github.com/repos/Jimmyxiao2009/Project-Apotheosis/releases/latest",
                jsonPath.c_str()); } catch (...) {}
            if (rc == 200) {
                std::ifstream f(jsonPath, std::ios::binary);
                if (f) { std::stringstream ss; ss << f.rdbuf(); body = ss.str(); }
            }
        }
        // 极简 JSON 取值(取 key 后第一个带引号字符串)
        auto pick = [&](const char* key) -> std::string {
            std::string pat = std::string("\"") + key + "\"";
            size_t p = body.find(pat); if (p == std::string::npos) return {};
            p = body.find(':', p + pat.size()); if (p == std::string::npos) return {};
            size_t a = body.find('"', p); if (a == std::string::npos) return {};
            size_t b = body.find('"', a + 1); if (b == std::string::npos) return {};
            return body.substr(a + 1, b - a - 1);
        };
        std::string tag = pick("tag_name");      // 形如 v0.1.8.4
        std::string page = pick("html_url");     // 发布页(release 对象第一个 html_url)
        bool ok = (rc == 200 && !tag.empty());
        bool newer = false;
        std::string verStr = tag;
        if (!verStr.empty() && (verStr[0] == 'v' || verStr[0] == 'V')) verStr = verStr.substr(1);
        if (ok) {
            int rel[4]; ParseDottedVersion(verStr, rel);
            for (int i = 0; i < 4; ++i) { if (rel[i] != cur[i]) { newer = rel[i] > cur[i]; break; } }
        }
        std::wstring tagW = Utf8ToWide(tag), pageW = Utf8ToWide(page);
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler(
                [self, manual, ok, newer, tagW, pageW]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    s->m_updateChecking = false;
                    if (!ok) { if (manual) s->TitleText->Text = L8(L"检查更新失败(网络?)", L"Update check failed (network?)"); return; }
                    if (!newer) { if (manual) s->TitleText->Text = ref new String(((g_lang == L"en" ? L"Up to date " : L"已是最新版 ") + tagW).c_str()); return; }
                    // 有新版:提示 + 可直接在本浏览器打开发布页下载
                    s->TitleText->Text = ref new String(((g_lang == L"en" ? L"New version " : L"发现新版本 ") + tagW).c_str());
                    std::wstring target = pageW.empty()
                        ? std::wstring(L"https://github.com/Jimmyxiao2009/Project-Apotheosis/releases/latest")
                        : pageW;
                    try {
                        auto dlg = ref new Windows::UI::Popups::MessageDialog(
                            ref new String((L"发现新版本 " + tagW + L"\n是否打开发布页下载 appx?").c_str()),
                            ref new String(L"有可用更新"));
                        auto go = ref new Windows::UI::Popups::UICommand(ref new String(L"前往下载"));
                        auto later = ref new Windows::UI::Popups::UICommand(ref new String(L"稍后"));
                        dlg->Commands->Append(go);
                        dlg->Commands->Append(later);
                        dlg->DefaultCommandIndex = 0;
                        dlg->CancelCommandIndex = 1;
                        Platform::Agile<MainPage^> self2(s);
                        concurrency::create_task(dlg->ShowAsync()).then(
                            [self2, go, target](Windows::UI::Popups::IUICommand^ chosen) {
                                MainPage^ s2 = self2.Get(); if (!s2) return;
                                if (chosen == go) {
                                    if (s2->SettingsPage->Visibility == Windows::UI::Xaml::Visibility::Visible) s2->HideSettings();
                                    s2->NavigateTo(ref new String(target.c_str()), true);
                                }
                            });
                    } catch (...) {}
                }));
        } catch (...) {}
    }).detach();
}

// 调试导出:把 LocalState 下的诊断文本拼成一份报告,FileSavePicker 让用户存到 OneDrive/SD 卡。
void MainPage::ExportDebug()
{
    std::wstring d = LocalStateDir();
    std::string report = "=== Apotheosis 调试报告 ===\n";
    report += "harness / WebCore 2.52.4 / ARM32 UWP\n\n";
    if (!d.empty()) {
        std::string dd = WideToUtf8(d);
        const char* names[] = { "stage.txt", "gpuinit.txt", "gpuresult.txt", "layertree.txt", "autodump.txt", "imedebug.txt", "jitresult.txt", "diag.txt" };
        for (const char* fn : names) {
            std::ifstream f(dd + "\\" + fn, std::ios::binary);
            if (!f) continue;
            std::stringstream ss; ss << f.rdbuf();
            report += std::string("---------- ") + fn + " ----------\n" + ss.str() + "\n\n";
        }
        report += "---------- crash dumps ----------\n(崩溃 dump 文件在 LocalState 根目录,可经 Device Portal 拉取)\n";
        try { std::ofstream o(dd + "\\debug-report.txt", std::ios::binary | std::ios::trunc); if (o) o.write(report.data(), report.size()); } catch (...) {}
    }
    Platform::String^ reportW = ref new String(Utf8ToWide(report).c_str());
    try {
        auto picker = ref new Windows::Storage::Pickers::FileSavePicker();
        picker->SuggestedStartLocation = Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary;
        picker->SuggestedFileName = ref new String(L"apotheosis-debug");
        auto exts = ref new Platform::Collections::Vector<Platform::String^>();
        exts->Append(".txt");
        picker->FileTypeChoices->Insert(ref new String(L"文本文件"), exts);
        concurrency::create_task(picker->PickSaveFileAsync()).then([reportW](Windows::Storage::StorageFile^ file) {
            if (file) concurrency::create_task(Windows::Storage::FileIO::WriteTextAsync(file, reportW));
        });
        TitleText->Text = L8(L"选择保存位置以导出…", L"Pick a location to export…");
    } catch (...) {
        TitleText->Text = L8(L"导出失败", L"Export failed");
    }
}

// ============================================================================
// 增量4:页内查找(查找条 + 引擎 WebCoreFindString/Next/Clear)
// ============================================================================

void MainPage::ShowFindBar()
{
    if (!m_sessionActive) { TitleText->Text = L8(L"当前页不可查找", L"Find not available here"); return; }
    HideActionMenu();
    HideSuggestions();
    FindBar->Visibility = Windows::UI::Xaml::Visibility::Visible;
    FindCount->Text = ref new String(L"");
    FindBox->Text = ref new String(L"");   // 触发一次空查找(清除残留高亮),无害
    FindBox->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void MainPage::OnFindChanged(Platform::Object^, Windows::UI::Xaml::Controls::TextChangedEventArgs^) { DoFind(0); }

void MainPage::OnFindKeyDown(Platform::Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (e->Key == Windows::System::VirtualKey::Enter) { e->Handled = true; DoFind(1); }
}

void MainPage::OnFindNext(Platform::Object^, RoutedEventArgs^) { DoFind(1); }
void MainPage::OnFindPrev(Platform::Object^, RoutedEventArgs^) { DoFind(2); }

void MainPage::OnFindClose(Platform::Object^, RoutedEventArgs^)
{
    FindBar->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    FindBox->Text = ref new String(L"");   // 触发空查找 → 引擎清除高亮
}

// 查找派发(引擎线程串行)。mode:0=查找(标记全部+选第一个),1=下一个,2=上一个。空串=清除。
void MainPage::DoFind(int mode)
{
    if (!m_sessionActive) { if (FindCount) FindCount->Text = ref new String(L""); return; }
    if (m_loading || m_interacting) return;
    std::wstring query = FindBox->Text ? std::wstring(FindBox->Text->Data()) : L"";
    bool clear = (mode == 0 && query.empty());

    m_interacting = true;
    SetLoading(true);
    if (m_loadWatchdog) m_loadWatchdog->Start();
    std::string q = WideToUtf8(query);
    bool present = m_gpuPresent;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;
    WebEngine::instance().post([disp, self, q, mode, clear, present, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try {
            if (clear) rc = WebCoreFindClear(rgba->data());
            else if (mode == 0) rc = WebCoreFindString(q.c_str(), /*matchCase*/ 0, /*wrap*/ 1, rgba->data());
            else rc = WebCoreFindNext(mode == 1 ? 1 : 0, rgba->data());
        } catch (...) { rc = -1000; }
        int rcCopy = rc; int modeCopy = mode; bool clearCopy = clear;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, rcCopy, modeCopy, clearCopy, present, mySeq]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // 被更新操作/看门狗取代
                    s->m_interacting = false;
                    if (s->m_loadWatchdog) s->m_loadWatchdog->Stop();
                    s->SetLoading(false);
                    if (rcCopy < 0) {
                        if (rcCopy == -12 || rcCopy == -14) {   // 会话没了
                            s->m_sessionActive = false;
                            s->ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                        }
                        s->FindCount->Text = ref new String(L"");
                        return;
                    }
                    if (!present) {
                        auto wb = ref new WriteableBitmap(kW, kH);
                        BlitToBitmap(wb, *rgba, kW, kH);
                        wb->Invalidate();
                        s->RenderImage->Source = wb;
                    }
                    s->m_lastFrameHash = 0;
                    if (clearCopy) s->FindCount->Text = ref new String(L"");
                    else if (modeCopy == 0) s->FindCount->Text = ref new String(rcCopy > 0 ? (std::to_wstring(rcCopy) + (g_lang == L"en" ? L" found" : L" 处")).c_str() : (g_lang == L"en" ? L"No results" : L"无结果"));
                    else s->FindCount->Text = ref new String(rcCopy ? L"" : (g_lang == L"en" ? L"No more" : L"无更多"));
                }));
        } catch (...) {}
    });
}

// ============================================================================
// 增量5:标签(Mode A 单热会话)。活动标签实时状态=全局成员;切换时与 m_tabs 互拷并重载。
// ============================================================================

void MainPage::UpdateTabCount()
{
    if (TabCountText) TabCountText->Text = ref new String(std::to_wstring(m_tabs.size()).c_str());
}

void MainPage::SaveActiveTab()
{
    if (m_activeTab < 0 || m_activeTab >= (int)m_tabs.size()) return;
    Tab& t = m_tabs[m_activeTab];
    t.navStack = m_navStack;
    t.navIndex = m_navIndex;
    t.currentUrl = m_currentUrl.empty() ? L"about:home" : m_currentUrl;
    t.currentTitle = m_currentTitle;
    t.pageScale = m_pageScale;
}

void MainPage::RestoreTab(int i)
{
    if (i < 0 || i >= (int)m_tabs.size()) return;
    m_activeTab = i;
    const Tab& t = m_tabs[i];
    m_navStack = t.navStack;
    m_navIndex = t.navIndex;
    m_currentUrl = t.currentUrl;
    m_currentTitle = t.currentTitle;
    m_pageScale = t.pageScale;
    // 切到该标签:作废在途、清加载锁,重载其 URL 重建单热会话。
    ++m_opSeq;
    m_interacting = false;
    if (m_loadWatchdog) m_loadWatchdog->Stop();
    m_loading = false;
    UpdateNavButtons();
    UpdateLockIcon();
    m_urlSyncing = true;
    UrlBox->Text = ref new String(m_currentUrl == L"about:home" ? L"" : m_currentUrl.c_str());
    m_urlSyncing = false;
    NavigateTo(ref new String(m_currentUrl.c_str()), false);
}

void MainPage::NewTab()
{
    SaveActiveTab();
    Tab t; t.currentUrl = g_homeUrl;
    m_tabs.push_back(t);
    m_activeTab = (int)m_tabs.size() - 1;
    // 清空全局,作废在途,重载主页。
    ++m_opSeq;
    m_interacting = false;
    if (m_loadWatchdog) m_loadWatchdog->Stop();
    m_loading = false;
    m_navStack.clear(); m_navIndex = -1;
    m_currentUrl.clear(); m_currentTitle.clear();
    m_pageScale = 1.0f;
    UpdateTabCount();
    NavigateTo(ref new String(g_homeUrl.c_str()), true);
}

void MainPage::CloseTab(int i)
{
    if (i < 0 || i >= (int)m_tabs.size()) return;
    bool wasActive = (i == m_activeTab);
    m_tabs.erase(m_tabs.begin() + i);
    if (m_tabs.empty()) {                      // 关到空:留一个主页标签
        Tab t; t.currentUrl = g_homeUrl;
        m_tabs.push_back(t);
        m_activeTab = 0;
        ++m_opSeq; m_interacting = false; if (m_loadWatchdog) m_loadWatchdog->Stop(); m_loading = false;
        m_navStack.clear(); m_navIndex = -1; m_currentUrl.clear(); m_currentTitle.clear(); m_pageScale = 1.0f;
        UpdateTabCount();
        NavigateTo(ref new String(g_homeUrl.c_str()), true);
        return;
    }
    if (m_activeTab >= (int)m_tabs.size()) m_activeTab = (int)m_tabs.size() - 1;
    else if (i < m_activeTab) m_activeTab--;   // 索引左移
    UpdateTabCount();
    if (wasActive) RestoreTab(m_activeTab);    // 关掉的是活动标签 → 载入新活动标签
}

void MainPage::SwitchTab(int i)
{
    if (i == m_activeTab) return;
    SaveActiveTab();
    RestoreTab(i);
}

void MainPage::OnTabs(Platform::Object^, RoutedEventArgs^) { ShowTabSwitcher(); }
void MainPage::OnNewTab(Platform::Object^, RoutedEventArgs^) { HideTabSwitcher(); NewTab(); }
void MainPage::OnTabSwitcherDone(Platform::Object^, RoutedEventArgs^) { HideTabSwitcher(); }

void MainPage::ShowTabSwitcher()
{
    HideActionMenu();
    HideSuggestions();
    SaveActiveTab();
    RebuildTabSwitcher();
    TabSwitcher->Visibility = Windows::UI::Xaml::Visibility::Visible;
    StopLiveMode();
}

void MainPage::HideTabSwitcher()
{
    TabSwitcher->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    StartLiveMode();
}

void MainPage::RebuildTabSwitcher()
{
    UpdateTabCount();
    if (TabSwitcherTitle) TabSwitcherTitle->Text = ref new String(((g_lang == L"en" ? L"Tabs (" : L"标签 (") + std::to_wstring(m_tabs.size()) + L")").c_str());
    TabList->Children->Clear();
    Platform::Agile<MainPage^> self(this);
    Color blue = ColorHelper::FromArgb(255, 0x3A, 0xA0, 0xFF);
    Color white = ColorHelper::FromArgb(255, 0xF0, 0xF0, 0xF0);
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        int idx = (int)i;
        const Tab& t = m_tabs[i];
        bool active = (idx == m_activeTab);
        std::wstring title = t.currentTitle.empty()
            ? (t.currentUrl == L"about:home" ? std::wstring(L"主页") : t.currentUrl)
            : t.currentTitle;
        std::wstring sub = (t.currentUrl == L"about:home") ? std::wstring(L"about:home") : t.currentUrl;

        auto cell = ref new Grid();
        cell->Margin = Thickness(0, 0, 0, 8);

        auto sw = ref new Button();
        sw->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x2B, 0x2D, 0x31));
        sw->BorderThickness = active ? Thickness(2) : Thickness(0);
        sw->BorderBrush = ref new SolidColorBrush(blue);
        sw->Padding = Thickness(0, 0, 40, 0);   // 右留位给关闭键
        sw->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        sw->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        sw->Content = MakeRow(ref new String(title.c_str()), ref new String(sub.c_str()), active ? blue : white);
        sw->Click += ref new RoutedEventHandler([self, idx](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->HideTabSwitcher();
            s->SwitchTab(idx);
        });
        cell->Children->Append(sw);

        auto cb = ref new Button();
        cb->Content = ref new String(L"\x2715");
        cb->Background = ref new SolidColorBrush(Colors::Transparent);
        cb->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x9A, 0xA0, 0xA6));
        cb->BorderThickness = Thickness(0);
        cb->Width = 44; cb->Height = 44;
        cb->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Right;
        cb->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        cb->Click += ref new RoutedEventHandler([self, idx](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->CloseTab(idx);
            s->RebuildTabSwitcher();
        });
        cell->Children->Append(cb);

        TabList->Children->Append(cell);
    }
}

// ============================================================================
// 默认 GPU:把 OnToggleGpu 首点路径抽出复用,带崩溃环路保护(GpuInit 硬崩→下次启动自动关)。
// ============================================================================
void MainPage::EnableGpu()
{
    if (m_gpuOn) return;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    GpuPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    auto props = ref new Windows::Foundation::Collections::PropertySet();
    props->Insert(L"EGLNativeWindowTypeProperty", GpuPanel);
    props->Insert(L"EGLRenderSurfaceSizeProperty",
                  Windows::Foundation::PropertyValue::CreateSize(Windows::Foundation::Size((float)kW, (float)kH)));
    m_gpuProps = props;
    void* win = reinterpret_cast<void*>(reinterpret_cast<IInspectable*>(props));
    // 崩溃环路保护:开 GPU 前落 gpu-crash.flag;回调(成功或优雅失败)删它。GpuInit 硬崩则无回调→标记残留→下次启动检测到→关默认GPU。
    {
        std::wstring fd = LocalStateDir();
        if (!fd.empty()) { try { std::ofstream f(WideToUtf8(fd) + "\\gpu-crash.flag", std::ios::binary | std::ios::trunc); if (f) f << "1"; } catch (...) {} }
    }
    WebEngine::instance().post([disp, self, win]() {
        int rc = -999;
        try { rc = WebCoreGpuInit(win, kW, kH); } catch (...) { rc = -1000; }
        try {
            std::wstring d = LocalStateDir();
            if (!d.empty()) { std::ofstream f(WideToUtf8(d) + "\\gpuinit.txt", std::ios::binary | std::ios::trunc); if (f) { std::string s = "WebCoreGpuInit(window) rc=" + std::to_string(rc) + "\n"; f.write(s.data(), s.size()); } }
        } catch (...) {}
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rcCopy]() {
                MainPage^ s = self.Get(); if (!s) return;
                std::wstring d2 = LocalStateDir();   // 回调到达=没硬崩 → 删崩溃标记
                if (!d2.empty()) { try { DeleteFileW((d2 + L"\\gpu-crash.flag").c_str()); } catch (...) {} }
                if (rcCopy == 0) {
                    s->m_gpuOn = true;
                    s->m_gpuPresent = true;
                    g_directPresent.store(true);
                    s->RenderImage->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                    s->GpuBtn->Content = GpuOrientLabel(s->m_gpuOrient);
                    s->GpuBtn->Foreground = ref new SolidColorBrush(Windows::UI::Colors::LimeGreen);
                    if (!s->m_currentUrl.empty() && s->m_currentUrl != L"about:home")
                        s->NavigateTo(ref new String(s->m_currentUrl.c_str()), false);   // 重载使合成+直呈现生效
                } else {
                    s->GpuPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                    s->GpuBtn->Content = ref new String(L"\U0001F5A5 GPU\x2717");
                    s->GpuBtn->Foreground = ref new SolidColorBrush(Windows::UI::Colors::OrangeRed);
                }
            }));
        } catch (...) {}
    });
}
